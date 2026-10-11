/* Copyright 2026 Peter Schlaile
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <string.h>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <FL/Fl.H>

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>

#include <rfb/DeviceLink.h>

#include "DeviceLinkShare.h"

using namespace rfb;

static core::LogWriter vlog("DeviceLink");

static const double pollInterval = 0.02;
// What we keep for a participant that does not take it
static const size_t maxPending = 1024 * 1024;

#ifdef WIN32
typedef SOCKET socket_t;
static void closeSocket(socket_t s) { closesocket(s); }
static bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
typedef int socket_t;
#define INVALID_SOCKET (-1)
static void closeSocket(socket_t s) { close(s); }
static bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK ||
                                  errno == EINTR; }
#endif

static void setNonBlocking(socket_t s)
{
#ifdef WIN32
  u_long one = 1;
  ioctlsocket(s, FIONBIO, &one);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
  fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
  int one_ = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one_, sizeof(one_));
}

// -=- Frames between a host and its guests

enum {
  frameData = 1,      // both
  frameParams,        // guest -> host
  frameLines,
  framePurge,
  frameLock,          // U8 flags, U16 seconds
  frameUnlock,
  frameFocus,         // U8 focused
  frameWritten,       // host -> guest: U32
  frameLineState,     // U8 lines, U8 errors
  frameLockResult,    // U8 status, message
};

namespace {

  // A connection between a host and a guest: a line "TOKEN SESSION" from
  // the guest, then frames of U8 type, U32 length, payload
  struct Link {
    Link(socket_t fd_) : fd(fd_), closed(false), greeted(false) {}
    ~Link() { closeSocket(fd); }

    void send(uint8_t type, const std::string& payload) {
      uint32_t len = payload.size();
      out += (char)type;
      out += (char)(len >> 24);
      out += (char)(len >> 16);
      out += (char)(len >> 8);
      out += (char)len;
      out += payload;
      flush();
    }

    void sendRaw(const std::string& text) {
      out += text;
      flush();
    }

    void flush() {
      while (!out.empty() && !closed) {
        int n = ::send(fd, out.data(), out.size(), 0);
        if (n < 0) {
          if (!wouldBlock())
            closed = true;
          return;
        }
        out.erase(0, n);
      }
    }

    // Reads what is there; false once the other side is gone
    bool receive() {
      char buf[4096];
      while (!closed) {
        int n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) {
          closed = true;
        } else if (n < 0) {
          if (!wouldBlock())
            closed = true;
          break;
        } else {
          in.append(buf, n);
        }
      }
      flush();
      return !closed;
    }

    bool frame(uint8_t* type, std::string* payload) {
      if (in.size() < 5)
        return false;
      uint32_t len = ((uint32_t)(uint8_t)in[1] << 24) |
                     ((uint8_t)in[2] << 16) | ((uint8_t)in[3] << 8) |
                     (uint8_t)in[4];
      if (len > 1024 * 1024) {
        closed = true;
        return false;
      }
      if (in.size() < 5 + len)
        return false;
      *type = in[0];
      payload->assign(in, 5, len);
      in.erase(0, 5 + len);
      return true;
    }

    socket_t fd;
    std::string in, out;
    bool closed;
    bool greeted;
  };

  struct Host;

  // What a viewer holds: a participant of a host in this process, or a
  // guest connection to a host in another viewer
  struct Handle : public DeviceLinkShare {
    Handle() : host(nullptr), pid(0), link(nullptr), connecting(false) {}
    ~Handle() override;

    Events take() override {
      Events e(events);
      events = Events();
      return e;
    }
    void write(const std::string& data) override;
    void setParams(const std::string& payload) override;
    void setLines(const std::string& payload) override;
    void purge(uint8_t which) override;
    void lock(int wait) override;
    void unlock() override;

    void toHost(uint8_t type, const std::string& payload);
    void pollGuest();
    void tryAttach();

    DeviceLinkTarget target;
    std::string session;
    Events events;

    // In this process
    Host* host;
    int pid;

    // Guest of another viewer
    Link* link;
    bool connecting;
    std::chrono::steady_clock::time_point connectUntil;
  };

  struct Participant {
    Participant() : local(nullptr), guest(nullptr), focused(false),
                    focusSeq(0) {}
    Handle* local;
    Link* guest;
    std::string session;
    bool focused;
    uint64_t focusSeq;
  };

  struct Waiter {
    int pid;
    bool forever;
    std::chrono::steady_clock::time_point until;
  };

  // The viewer that has the device open
  struct Host {
    Host() : lock(nullptr), port(nullptr), stopping(false), openDone(false),
             gone(false), lineChanged(false), lines(0), errors(0),
             listenFd(INVALID_SOCKET), opened(false), nextPid(1),
             holder(0) {}
    ~Host();

    void start();
    void poll();
    void fromParticipant(int pid, uint8_t type, const std::string& payload);
    void requestLock(int pid, int wait);
    void releaseLock(int pid);
    void lockResult(int pid, uint8_t status, const std::string& message);
    void removeParticipant(int pid);
    int target() const;
    void publish();

    static void readLoop(Host* host);
    static void writeLoop(Host* host);

    DeviceLinkTarget device;
    std::string key;
    std::string session;
    DeviceLinkLock* lock;
    DeviceLinkPort* port;

    std::thread reader, writer;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> stopping;

    // Shared with the threads, under m
    bool openDone;
    std::string openError;
    bool gone;
    std::string goneError;
    std::string incoming;
    bool lineChanged;
    uint8_t lines, errors;
    struct Op {
      int pid;
      uint8_t type;
      std::string data;
    };
    std::deque<Op> ops;
    std::map<int, uint32_t> written;

    // Main thread
    socket_t listenFd;
    std::string token;
    bool opened;
    std::list<Link*> pending;
    std::map<int, Participant> parts;
    int nextPid;
    int holder;
    std::deque<Waiter> waiters;
  };

}

static std::list<Host*> hosts;
static std::set<Handle*> handles;
static bool focused = false;
static uint64_t focusCounter = 0;

static void pollTimer(void*);

static void ensureTimer()
{
  if (!Fl::has_timeout(pollTimer, nullptr))
    Fl::add_timeout(pollInterval, pollTimer, nullptr);
}

static void pollTimer(void*)
{
  DeviceLinkShare::pollAll();
  if (DeviceLinkShare::active())
    Fl::repeat_timeout(pollInterval, pollTimer, nullptr);
}

static std::string deviceKey(const DeviceLinkTarget& t)
{
  std::string key(std::to_string(t.kind) + ":" + t.where);
#ifdef WIN32
  for (char& c : key)
    c = tolower((unsigned char)c);
#endif
  return key;
}

static std::string randomToken()
{
  std::random_device random;
  std::string token;
  for (int i = 0; i < 4; i++)
    token += core::format("%08x", (unsigned)random());
  return token;
}

// -=- Host

void Host::start()
{
  reader = std::thread(readLoop, this);
  writer = std::thread(writeLoop, this);
}

Host::~Host()
{
  {
    std::lock_guard<std::mutex> l(m);
    stopping = true;
    cv.notify_all();
  }
  if (reader.joinable())
    reader.join();
  if (writer.joinable())
    writer.join();
  delete port;

  for (auto& entry : parts) {
    if (entry.second.guest)
      delete entry.second.guest;
  }
  for (Link* link : pending)
    delete link;
  if (listenFd != INVALID_SOCKET)
    closeSocket(listenFd);
  // Last, so that a new host finds the device closed
  delete lock;
}

void Host::readLoop(Host* host)
{
  std::string error;
  DeviceLinkPort* port = DeviceLinkPort::open(host->device, &error);

  {
    std::lock_guard<std::mutex> l(host->m);
    host->port = port;
    host->openDone = true;
    host->openError = error;
    host->cv.notify_all();
  }
  if (port == nullptr)
    return;

  uint8_t buf[deviceLinkMaxData];
  bool linesKnown = false;
  uint8_t lastLines = 0;
  while (!host->stopping) {
    int n = port->read(buf, sizeof(buf), &error);
    uint8_t lines, errors;
    bool haveLines = port->lineState(&lines, &errors);

    std::lock_guard<std::mutex> l(host->m);
    if (n < 0) {
      host->gone = true;
      host->goneError = error;
      return;
    }
    if (n > 0)
      host->incoming.append((const char*)buf, n);
    if (haveLines && (!linesKnown || lines != lastLines || errors)) {
      linesKnown = true;
      lastLines = lines;
      host->lines = lines;
      host->errors |= errors;
      host->lineChanged = true;
    }
  }
}

void Host::writeLoop(Host* host)
{
  DeviceLinkPort* port;

  {
    std::unique_lock<std::mutex> l(host->m);
    host->cv.wait(l, [&] { return host->stopping || host->openDone; });
    port = host->port;
  }
  if (port == nullptr)
    return;

  while (true) {
    Op op;
    {
      std::unique_lock<std::mutex> l(host->m);
      host->cv.wait(l, [&] {
        return host->stopping || !host->ops.empty();
      });
      if (host->stopping)
        return;
      op = host->ops.front();
      host->ops.pop_front();
    }

    const uint8_t* d = (const uint8_t*)op.data.data();
    size_t len = op.data.size();
    std::string error;
    bool fixed = host->device.fixed;

    switch (op.type) {
    case frameData:
      if (!port->write(d, len, host->stopping, &error)) {
        std::lock_guard<std::mutex> l(host->m);
        host->gone = true;
        host->goneError = error;
        return;
      } else {
        std::lock_guard<std::mutex> l(host->m);
        host->written[op.pid] += len;
      }
      break;
    case frameParams:
      if (len >= 9 && !fixed) {
        uint32_t speed = ((uint32_t)d[1] << 24) | (d[2] << 16) |
                         (d[3] << 8) | d[4];
        port->setParams(d[0], speed, d[5], d[6], d[7], d[8]);
      }
      break;
    case frameLines:
      if (len >= 4) {
        if (d[0] && !fixed)
          port->setLines(d[0], d[1]);
        unsigned ms = (d[2] << 8) | d[3];
        if (ms)
          port->sendBreak(ms);
      }
      break;
    case framePurge:
      if (len >= 1)
        port->purge(d[0]);
      break;
    }
  }
}

// Who gets what the device says: the session with the lock, else the
// one whose viewer has the focus, else the one that had it last
int Host::target() const
{
  if (holder != 0 && parts.count(holder))
    return holder;

  int best = 0;
  bool bestFocused = false;
  uint64_t bestSeq = 0;
  for (const auto& entry : parts) {
    const Participant& p = entry.second;
    if (best == 0 || (p.focused && !bestFocused) ||
        (p.focused == bestFocused && p.focusSeq > bestSeq)) {
      best = entry.first;
      bestFocused = p.focused;
      bestSeq = p.focusSeq;
    }
  }
  return best;
}

void Host::fromParticipant(int pid, uint8_t type, const std::string& payload)
{
  auto it = parts.find(pid);
  if (it == parts.end())
    return;

  switch (type) {
  case frameData:
  case frameParams:
  case frameLines:
  case framePurge:
    if (holder != 0 && holder != pid) {
      // Another session has the device; its sale must not be disturbed.
      // The data counts as written, so that the session can go on.
      if (type == frameData) {
        std::lock_guard<std::mutex> l(m);
        written[pid] += payload.size();
      }
      return;
    }
    {
      std::lock_guard<std::mutex> l(m);
      ops.push_back({pid, type, payload});
      cv.notify_all();
    }
    return;
  case frameLock:
    if (payload.size() >= 3) {
      int seconds = ((uint8_t)payload[1] << 8) | (uint8_t)payload[2];
      requestLock(pid, payload[0] & deviceLinkLockWait ? seconds : -1);
    }
    return;
  case frameUnlock:
    releaseLock(pid);
    return;
  case frameFocus:
    if (!payload.empty()) {
      it->second.focused = payload[0] != 0;
      if (it->second.focused)
        it->second.focusSeq = ++focusCounter;
    }
    return;
  }
}

void Host::lockResult(int pid, uint8_t status, const std::string& message)
{
  auto it = parts.find(pid);
  if (it == parts.end())
    return;
  if (it->second.local) {
    it->second.local->events.lockResult = status;
    it->second.local->events.lockMessage = message;
  } else {
    it->second.guest->send(frameLockResult, std::string(1, (char)status) +
                                            message);
  }
}

void Host::requestLock(int pid, int wait)
{
  if (holder == 0 || holder == pid) {
    holder = pid;
    lockResult(pid, deviceLinkOpen, "");
    return;
  }
  if (wait < 0) {
    lockResult(pid, deviceLinkBusy,
               core::format(_("%s is in use by %s"), device.where.c_str(),
                            parts[holder].session.c_str()));
    return;
  }
  Waiter w;
  w.pid = pid;
  w.forever = wait == 0;
  w.until = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
  waiters.push_back(w);
}

void Host::releaseLock(int pid)
{
  for (auto it = waiters.begin(); it != waiters.end(); ) {
    if (it->pid == pid)
      it = waiters.erase(it);
    else
      ++it;
  }
  if (holder != pid)
    return;
  holder = 0;
  // The next one in line
  while (!waiters.empty()) {
    Waiter w = waiters.front();
    waiters.pop_front();
    if (parts.count(w.pid)) {
      holder = w.pid;
      lockResult(w.pid, deviceLinkOpen, "");
      break;
    }
  }
}

void Host::removeParticipant(int pid)
{
  releaseLock(pid);
  auto it = parts.find(pid);
  if (it == parts.end())
    return;
  delete it->second.guest;
  parts.erase(it);
}

// Lets other viewers on this computer find us
void Host::publish()
{
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);

  listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd == INVALID_SOCKET)
    return;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
      listen(listenFd, 8) != 0 ||
      getsockname(listenFd, (struct sockaddr*)&addr, &len) != 0) {
    closeSocket(listenFd);
    listenFd = INVALID_SOCKET;
    return;
  }
  setNonBlocking(listenFd);

  token = randomToken();
  lock->setInfo(core::format("%d %s %s", ntohs(addr.sin_port),
                             token.c_str(), session.c_str()));
}

void Host::poll()
{
  // The device was opened, or not
  if (!opened) {
    bool done, ok;
    std::string error;
    {
      std::lock_guard<std::mutex> l(m);
      done = openDone;
      ok = port != nullptr;
      error = openError;
    }
    if (!done)
      return;
    for (auto& entry : parts) {
      if (entry.second.local) {
        entry.second.local->events.opened = ok;
        entry.second.local->events.failed = !ok;
        entry.second.local->events.error = error;
      }
    }
    if (!ok) {
      gone = true;
      return;
    }
    opened = true;
    if (lock != nullptr)
      publish();
  }

  // New guests
  while (listenFd != INVALID_SOCKET) {
    socket_t fd = accept(listenFd, nullptr, nullptr);
    if (fd == INVALID_SOCKET)
      break;
    setNonBlocking(fd);
    pending.push_back(new Link(fd));
  }
  for (auto it = pending.begin(); it != pending.end(); ) {
    Link* link = *it;
    link->receive();
    size_t eol = link->in.find('\n');
    if (link->closed || (eol == std::string::npos && link->in.size() > 512)) {
      delete link;
      it = pending.erase(it);
      continue;
    }
    if (eol == std::string::npos) {
      ++it;
      continue;
    }
    std::string hello(link->in.substr(0, eol));
    link->in.erase(0, eol + 1);
    it = pending.erase(it);
    size_t space = hello.find(' ');
    if (hello.substr(0, space) != token) {
      vlog.error(_("A program on this computer tried to use %s without "
                   "the secret"), device.where.c_str());
      delete link;
      continue;
    }
    Participant p;
    p.guest = link;
    p.session = space == std::string::npos ? "" : hello.substr(space + 1);
    int pid = nextPid++;
    parts[pid] = p;
    vlog.info(_("Session %s shares %s"), p.session.c_str(),
              device.where.c_str());
  }

  // What the guests want
  std::list<int> leaving;
  for (auto& entry : parts) {
    Link* link = entry.second.guest;
    if (link == nullptr)
      continue;
    bool alive = link->receive();
    uint8_t type;
    std::string payload;
    while (link->frame(&type, &payload))
      fromParticipant(entry.first, type, payload);
    if (!alive || link->closed)
      leaving.push_back(entry.first);
  }
  for (int pid : leaving)
    removeParticipant(pid);

  // Waiting for the lock too long
  auto now = std::chrono::steady_clock::now();
  for (auto it = waiters.begin(); it != waiters.end(); ) {
    if (!it->forever && now > it->until) {
      int pid = it->pid;
      it = waiters.erase(it);
      if (parts.count(holder))
        lockResult(pid, deviceLinkBusy,
                   core::format(_("%s is in use by %s"), device.where.c_str(),
                                parts[holder].session.c_str()));
    } else {
      ++it;
    }
  }

  // What the device did
  std::string data;
  std::map<int, uint32_t> done;
  bool changed, isGone;
  uint8_t l, e;
  std::string error;
  {
    std::lock_guard<std::mutex> g(m);
    data.swap(incoming);
    done.swap(written);
    changed = lineChanged;
    lineChanged = false;
    l = lines;
    e = errors;
    errors = 0;
    isGone = gone;
    error = goneError;
  }

  if (!data.empty()) {
    int to = target();
    auto it = parts.find(to);
    if (it != parts.end()) {
      if (it->second.local) {
        std::string& d = it->second.local->events.data;
        if (d.size() + data.size() <= maxPending)
          d += data;
      } else {
        it->second.guest->send(frameData, data);
      }
    }
  }
  for (auto& entry : done) {
    auto it = parts.find(entry.first);
    if (it == parts.end())
      continue;
    if (it->second.local) {
      it->second.local->events.written += entry.second;
    } else {
      uint32_t n = entry.second;
      std::string payload;
      payload += (char)(n >> 24);
      payload += (char)(n >> 16);
      payload += (char)(n >> 8);
      payload += (char)n;
      it->second.guest->send(frameWritten, payload);
    }
  }
  if (changed) {
    for (auto& entry : parts) {
      if (entry.second.local) {
        entry.second.local->events.lineChanged = true;
        entry.second.local->events.lines = l;
        entry.second.local->events.errors |= e;
      } else {
        std::string payload;
        payload += (char)l;
        payload += (char)e;
        entry.second.guest->send(frameLineState, payload);
      }
    }
  }
  if (isGone) {
    vlog.error(_("%s is gone: %s"), device.where.c_str(), error.c_str());
    for (auto& entry : parts) {
      if (entry.second.local) {
        entry.second.local->events.gone = true;
        entry.second.local->events.error = error;
      }
    }
    // The guests notice that we closed the connection
    this->gone = true;
  }
}

// -=- Handle

void Handle::toHost(uint8_t type, const std::string& payload)
{
  if (host != nullptr)
    host->fromParticipant(pid, type, payload);
  else if (link != nullptr && !connecting)
    link->send(type, payload);
}

void Handle::write(const std::string& data) { toHost(frameData, data); }
void Handle::setParams(const std::string& p) { toHost(frameParams, p); }
void Handle::setLines(const std::string& p) { toHost(frameLines, p); }
void Handle::purge(uint8_t which)
{
  toHost(framePurge, std::string(1, (char)which));
}

void Handle::lock(int wait)
{
  std::string payload;
  payload += (char)(wait >= 0 ? deviceLinkLockWait : 0);
  payload += (char)((wait > 0 ? wait : 0) >> 8);
  payload += (char)(wait > 0 ? wait : 0);
  toHost(frameLock, payload);
}

void Handle::unlock()
{
  toHost(frameUnlock, "");
}

Handle::~Handle()
{
  handles.erase(this);
  if (host != nullptr)
    host->removeParticipant(pid);
  delete link;
}

// Becomes the host of the device, or a guest of the viewer that is
void Handle::tryAttach()
{
  std::string holder;
  DeviceLinkLock* lock = nullptr;

  if (target.kind != deviceLinkTail) {
    // A host in this process
    for (Host* h : hosts) {
      if (h->key == deviceKey(target) && !h->gone) {
        host = h;
        break;
      }
    }
    if (host == nullptr)
      lock = DeviceLinkLock::tryLock(target, session, &holder);
  }

  if (host == nullptr && (lock != nullptr || target.kind == deviceLinkTail)) {
    Host* h = new Host;
    h->device = target;
    h->key = deviceKey(target);
    h->session = session;
    h->lock = lock;
    hosts.push_back(h);
    h->start();
    host = h;
  }

  if (host != nullptr) {
    connecting = false;
    Participant p;
    p.local = this;
    p.session = session;
    p.focused = focused;
    p.focusSeq = focused ? ++focusCounter : 0;
    pid = host->nextPid++;
    host->parts[pid] = p;
    if (host->opened)
      events.opened = true;
    return;
  }

  // Another viewer has it: "PORT TOKEN SESSION"
  std::istringstream in(holder);
  int port = 0;
  std::string token, theirs;
  in >> port >> token;
  std::getline(in >> std::ws, theirs);
  if (port <= 0 || token.empty())
    return;     // not published yet, try again

  socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET)
    return;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    closeSocket(fd);
    return;
  }
  setNonBlocking(fd);

  link = new Link(fd);
  link->sendRaw(token + " " + session + "\n");
  connecting = false;
  events.opened = true;
  vlog.info(_("Sharing %s with %s"), target.where.c_str(), theirs.c_str());
  if (focused)
    link->send(frameFocus, std::string(1, (char)1));
}

void Handle::pollGuest()
{
  if (connecting) {
    tryAttach();
    if (connecting && std::chrono::steady_clock::now() > connectUntil) {
      connecting = false;
      events.failed = true;
      events.error = core::format(_("%s is in use by another program"),
                                  target.where.c_str());
    }
    return;
  }

  if (link == nullptr)
    return;

  bool alive = link->receive();
  uint8_t type;
  std::string payload;
  while (link->frame(&type, &payload)) {
    const uint8_t* d = (const uint8_t*)payload.data();
    switch (type) {
    case frameData:
      if (events.data.size() + payload.size() <= maxPending)
        events.data += payload;
      break;
    case frameWritten:
      if (payload.size() >= 4)
        events.written += ((uint32_t)d[0] << 24) | (d[1] << 16) |
                          (d[2] << 8) | d[3];
      break;
    case frameLineState:
      if (payload.size() >= 2) {
        events.lineChanged = true;
        events.lines = d[0];
        events.errors |= d[1];
      }
      break;
    case frameLockResult:
      if (payload.size() >= 1) {
        events.lockResult = d[0];
        events.lockMessage = payload.substr(1);
      }
      break;
    }
  }
  if (!alive) {
    delete link;
    link = nullptr;
    events.gone = true;
    events.error = _("The viewer that has the device was closed");
  }
}

// -=- DeviceLinkShare

DeviceLinkShare* DeviceLinkShare::attach(const DeviceLinkTarget& target,
                                         const std::string& session)
{
  Handle* handle = new Handle;
  handle->target = target;
  handle->session = session;
  handle->connecting = true;
  handle->connectUntil = std::chrono::steady_clock::now() +
                         std::chrono::seconds(5);
  handles.insert(handle);
  handle->tryAttach();
  ensureTimer();
  return handle;
}

void DeviceLinkShare::setFocus(bool focus)
{
  if (focus == focused)
    return;
  focused = focus;
  for (Handle* h : handles) {
    if (h->host != nullptr) {
      auto it = h->host->parts.find(h->pid);
      if (it != h->host->parts.end()) {
        it->second.focused = focus;
        if (focus)
          it->second.focusSeq = ++focusCounter;
      }
    } else if (h->link != nullptr) {
      h->link->send(frameFocus, std::string(1, (char)(focus ? 1 : 0)));
    }
  }
}

void DeviceLinkShare::pollAll()
{
  std::list<Handle*> all(handles.begin(), handles.end());
  for (Handle* h : all)
    h->pollGuest();

  for (auto it = hosts.begin(); it != hosts.end(); ) {
    Host* h = *it;
    h->poll();
    // A host stays while somebody uses the device
    if (h->gone || h->parts.empty()) {
      for (Handle* handle : handles) {
        if (handle->host == h)
          handle->host = nullptr;
      }
      it = hosts.erase(it);
      delete h;
    } else {
      ++it;
    }
  }
}

bool DeviceLinkShare::active()
{
  return !hosts.empty() || !handles.empty();
}
