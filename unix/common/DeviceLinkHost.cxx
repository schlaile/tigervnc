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
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <sstream>
#include <vector>

#include <core/LogWriter.h>

#include <rfb/DeviceLink.h>
#include <rfb/VNCServer.h>

#include <DeviceLinkHost.h>

using namespace rfb;

static core::LogWriter vlog("DeviceLink");

// Lines an application may send before we give up on it
static const size_t maxLine = 1024;
// Output we buffer for an application that does not read its events
static const size_t maxControlOutput = 65536;

struct DeviceLinkHost::Control {
  Control(int fd_) : fd(fd_) {}
  int fd;
  std::string in, out;
};

struct DeviceLinkHost::Device {
  Device()
    : kind(0), control(nullptr), releasedBy(nullptr), channel(0),
      state(Pending),
      ptyFd(-1), slaveFd(-1), listenFd(-1), connFd(-1),
      sendWindow(deviceLinkWindow),
      termSpeed(0), termFlow(deviceLinkFlowNone),
      speed(0), dataBits(8), parity(deviceLinkParityNone),
      stopBits(deviceLinkStopBits1), flow(deviceLinkFlowNone) {}

  std::string name;
  std::string label;
  uint8_t kind;
  // nullptr once the application has released the device
  Control* control;
  // Waits for "ok NAME" once the viewer has closed the device
  Control* releasedBy;
  uint16_t channel;
  enum { Pending, Open, Closing } state;

  int ptyFd, slaveFd;   // serial
  int listenFd, connFd; // tcp, tail
  std::string path;

  // From the viewer, not yet taken by the application
  std::string toApp;
  // Bytes we may still send to the viewer
  uint32_t sendWindow;

  // Settings of the pseudo terminal when we last looked
  uint32_t termSpeed;
  uint8_t termFlow;

  // Settings as last sent to the viewer
  uint32_t speed;
  uint8_t dataBits, parity, stopBits, flow;
};

static const struct {
  speed_t code;
  uint32_t speed;
} speeds[] = {
  { B50, 50 }, { B75, 75 }, { B110, 110 }, { B134, 134 },
  { B150, 150 }, { B200, 200 }, { B300, 300 }, { B600, 600 },
  { B1200, 1200 }, { B1800, 1800 }, { B2400, 2400 }, { B4800, 4800 },
  { B9600, 9600 }, { B19200, 19200 }, { B38400, 38400 },
  { B57600, 57600 }, { B115200, 115200 }, { B230400, 230400 },
#ifdef B460800
  { B460800, 460800 },
#endif
#ifdef B921600
  { B921600, 921600 },
#endif
};

static uint32_t speedOf(speed_t code)
{
  for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
    if (speeds[i].code == code)
      return speeds[i].speed;
  }
  return 0;
}

static bool validName(const std::string& name)
{
  if (name.empty() || name.size() > deviceLinkMaxName)
    return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-'))
      return false;
  }
  return true;
}

static void setNonBlock(int fd)
{
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static void makeDirs(const std::string& path)
{
  size_t pos = 0;

  while ((pos = path.find('/', pos + 1)) != std::string::npos)
    mkdir(path.substr(0, pos).c_str(), 0700);
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("Cannot create " + path + ": " +
                             strerror(errno));
  chmod(path.c_str(), 0700);
}

static int listenUnix(const std::string& path)
{
  struct sockaddr_un addr;
  int fd;

  if (path.size() >= sizeof(addr.sun_path))
    return -1;

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  setNonBlock(fd);

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path.c_str());

  unlink(path.c_str());
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
      chmod(path.c_str(), 0600) != 0 || listen(fd, 4) != 0) {
    close(fd);
    unlink(path.c_str());
    return -1;
  }

  return fd;
}

std::string DeviceLinkHost::defaultDir(const char* display)
{
  const char* runtime = getenv("XDG_RUNTIME_DIR");
  std::string base;

  if (runtime && runtime[0] == '/')
    base = std::string(runtime) + "/vnc-device-link";
  else
    base = "/tmp/vnc-device-link-" + std::to_string(getuid());

  return base + "/:" + display;
}

DeviceLinkHost::DeviceLinkHost(Watcher* watcher_, VNCServer* server_,
                               const char* dir_, unsigned maxChannels_)
  : watcher(watcher_), server(server_), dir(dir_),
    maxChannels(maxChannels_), ownerPresent(false), listenFd(-1),
    nextChannel(1)
{
  struct stat st;
  std::string control;

  makeDirs(dir);
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != getuid())
    throw std::runtime_error(dir + " is not a directory of ours");

  control = dir + "/control";
  listenFd = listenUnix(control);
  if (listenFd < 0)
    throw std::runtime_error("Cannot listen on " + control + ": " +
                             strerror(errno));
  watcher->watchFd(listenFd, true, false);

  server->enableDeviceLink(maxChannels);

  vlog.info("Device links on %s", control.c_str());
}

DeviceLinkHost::~DeviceLinkHost()
{
  while (!channels.empty())
    finish(channels.begin()->second);
  while (!controls.empty())
    closeControl(controls.front());

  watcher->unwatchFd(listenFd);
  close(listenFd);
  unlink((dir + "/control").c_str());
  rmdir(dir.c_str());
}

bool DeviceLinkHost::handleFdEvent(int fd, bool read, bool write)
{
  if (fd == listenFd) {
    acceptControl();
    return true;
  }

  for (Control* control : controls) {
    if (control->fd == fd) {
      handleControl(control, read, write);
      return true;
    }
  }

  for (auto& entry : channels) {
    Device* device = entry.second;
    if (fd == device->ptyFd || fd == device->listenFd ||
        fd == device->connFd) {
      handleDevice(device, fd, read, write);
      return true;
    }
  }

  return false;
}

// -=- Control socket

void DeviceLinkHost::acceptControl()
{
  int fd;

  fd = accept(listenFd, nullptr, nullptr);
  if (fd < 0)
    return;
  setNonBlock(fd);

  Control* control = new Control(fd);
  controls.push_back(control);
  watcher->watchFd(fd, true, false);

  reply(control, ownerPresent ? "viewer present" : "viewer absent");
}

void DeviceLinkHost::handleControl(Control* control, bool read,
                                   bool write)
{
  if (write && !control->out.empty()) {
    ssize_t n = ::write(control->fd, control->out.data(),
                        control->out.size());
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
      closeControl(control);
      return;
    }
    if (n > 0)
      control->out.erase(0, n);
    watcher->watchFd(control->fd, true, !control->out.empty());
  }

  if (!read)
    return;

  char buf[1024];
  ssize_t n = ::read(control->fd, buf, sizeof(buf));
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    closeControl(control);
    return;
  }
  if (n < 0)
    return;

  control->in.append(buf, n);

  size_t eol;
  while ((eol = control->in.find('\n')) != std::string::npos) {
    std::string line(control->in, 0, eol);
    control->in.erase(0, eol + 1);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    handleCommand(control, line);
    // The command may have closed the connection
    bool alive = false;
    for (Control* c : controls)
      alive = alive || c == control;
    if (!alive)
      return;
  }

  if (control->in.size() > maxLine)
    closeControl(control);
}

void DeviceLinkHost::reply(Control* control, const std::string& line)
{
  if (control->out.size() + line.size() > maxControlOutput) {
    vlog.error("Application does not read its device link events");
    closeControl(control);
    return;
  }
  bool idle = control->out.empty();
  control->out += line + "\n";
  if (idle)
    handleControl(control, false, true);
}

void DeviceLinkHost::broadcast(const std::string& line)
{
  std::list<Control*> all(controls);
  for (Control* control : all)
    reply(control, line);
}

void DeviceLinkHost::closeControl(Control* control)
{
  std::list<std::string> names;

  for (auto& entry : devices) {
    if (entry.second->control == control)
      names.push_back(entry.first);
  }
  for (const std::string& name : names)
    release(control, name, false);
  for (auto& entry : channels) {
    if (entry.second->releasedBy == control)
      entry.second->releasedBy = nullptr;
  }

  watcher->unwatchFd(control->fd);
  close(control->fd);
  controls.remove(control);
  delete control;
}

static bool parseFlag(const std::string& word, const char* name,
                      bool* value)
{
  std::string prefix(std::string(name) + "=");
  if (word.compare(0, prefix.size(), prefix) != 0)
    return false;
  std::string v(word, prefix.size());
  if (v != "0" && v != "1")
    return false;
  *value = v == "1";
  return true;
}

void DeviceLinkHost::handleCommand(Control* control,
                                   const std::string& line)
{
  std::istringstream in(line);
  std::string command, name;

  in >> command >> name;
  if (command.empty())
    return;

  if (command == "request") {
    // request NAME KIND [wait[=SECONDS]] LABEL
    std::string kind, label;
    int wait = -1;
    in >> kind;
    std::getline(in >> std::ws, label);
    if (label == "wait" || label.compare(0, 5, "wait ") == 0 ||
        label.compare(0, 5, "wait=") == 0) {
      std::string option(label.substr(0, label.find(' ')));
      size_t rest = label.find_first_not_of(' ', option.size());
      label = rest == std::string::npos ? "" : label.substr(rest);
      wait = 0;
      if (option.size() > 5) {
        char* end;
        unsigned long seconds = strtoul(option.c_str() + 5, &end, 10);
        if (*end != '\0' || seconds > 65535) {
          reply(control, "error " + name + " invalid wait");
          return;
        }
        wait = seconds;
      }
    }
    request(control, name, kind, label, wait);
    return;
  }

  if (command == "release") {
    auto it = devices.find(name);
    if (it == devices.end() || it->second->control != control) {
      reply(control, "error " + name + " not-requested");
      return;
    }
    // "ok" once the viewer has closed it, so that the device is free
    // for other sessions when the application hears about it
    release(control, name, true);
    return;
  }

  Device* device = serialFor(control, name);
  if (device == nullptr)
    return;

  std::string arg;
  in >> arg;

  if (command == "format") {
    // 8N1, 7E1, 8N1.5 ...
    static const char paritiesChars[] = "NOEMS";
    const char* p;
    if (arg.size() < 3 || arg[0] < '5' || arg[0] > '8' ||
        (p = strchr(paritiesChars, arg[1])) == nullptr ||
        (arg.substr(2) != "1" && arg.substr(2) != "2" &&
         arg.substr(2) != "1.5")) {
      reply(control, "error " + name + " invalid format");
      return;
    }
    device->dataBits = arg[0] - '0';
    device->parity = p - paritiesChars;
    device->stopBits = arg.substr(2) == "1" ? deviceLinkStopBits1 :
                       arg.substr(2) == "2" ? deviceLinkStopBits2 :
                                              deviceLinkStopBits15;
    sendParams(device, 2);
  } else if (command == "speed") {
    char* end;
    unsigned long speed = strtoul(arg.c_str(), &end, 10);
    if (arg.empty() || *end != '\0' || speed == 0 || speed > 0xffffffff) {
      reply(control, "error " + name + " invalid speed");
      return;
    }
    device->speed = speed;
    sendParams(device, 1);
  } else if (command == "flow") {
    if (arg == "none")
      device->flow = deviceLinkFlowNone;
    else if (arg == "rtscts")
      device->flow = deviceLinkFlowRTSCTS;
    else if (arg == "xonxoff")
      device->flow = deviceLinkFlowXONXOFF;
    else {
      reply(control, "error " + name + " invalid flow");
      return;
    }
    sendParams(device, 4);
  } else if (command == "lines") {
    uint8_t msg[4] = { 0, 0, 0, 0 };
    std::string word(arg);
    do {
      bool value;
      if (parseFlag(word, "dtr", &value)) {
        msg[0] |= deviceLinkDTR;
        msg[1] |= value ? deviceLinkDTR : 0;
      } else if (parseFlag(word, "rts", &value)) {
        msg[0] |= deviceLinkRTS;
        msg[1] |= value ? deviceLinkRTS : 0;
      } else {
        reply(control, "error " + name + " invalid lines");
        return;
      }
    } while (in >> word);
    send(deviceLinkSetLines, device->channel, msg, sizeof(msg));
  } else if (command == "break") {
    char* end;
    unsigned long ms = strtoul(arg.c_str(), &end, 10);
    if (arg.empty() || *end != '\0' || ms == 0 || ms > 65535) {
      reply(control, "error " + name + " invalid break");
      return;
    }
    uint8_t msg[4] = { 0, 0, (uint8_t)(ms >> 8), (uint8_t)ms };
    send(deviceLinkSetLines, device->channel, msg, sizeof(msg));
  } else if (command == "purge") {
    uint8_t which;
    if (arg == "receive")
      which = deviceLinkPurgeReceive;
    else if (arg == "transmit")
      which = deviceLinkPurgeTransmit;
    else if (arg == "both")
      which = deviceLinkPurgeReceive | deviceLinkPurgeTransmit;
    else {
      reply(control, "error " + name + " invalid purge");
      return;
    }
    send(deviceLinkPurge, device->channel, &which, 1);
  } else {
    reply(control, "error " + name + " unknown-command");
    return;
  }

  reply(control, "ok " + name);
}

DeviceLinkHost::Device* DeviceLinkHost::serialFor(Control* control,
                                                  const std::string& name)
{
  auto it = devices.find(name);
  if (it == devices.end() || it->second->control != control ||
      it->second->state != Device::Open) {
    reply(control, "error " + name + " not-open");
    return nullptr;
  }
  if (it->second->kind != deviceLinkSerial) {
    reply(control, "error " + name + " not-serial");
    return nullptr;
  }
  return it->second;
}

// -=- Requests

void DeviceLinkHost::request(Control* control, const std::string& name,
                             const std::string& kindName,
                             const std::string& label, int wait)
{
  uint8_t kind;

  if (!validName(name)) {
    reply(control, "error " + name + " invalid name");
    return;
  }
  if (kindName == "serial")
    kind = deviceLinkSerial;
  else if (kindName == "tcp")
    kind = deviceLinkTCP;
  else if (kindName == "tail")
    kind = deviceLinkTail;
  else {
    reply(control, "error " + name + " invalid kind");
    return;
  }

  if (!ownerPresent) {
    reply(control, "error " + name + " no-viewer");
    return;
  }
  if (devices.count(name)) {
    reply(control, "error " + name + " busy");
    return;
  }
  if (channels.size() >= maxChannels) {
    reply(control, "error " + name + " too-many");
    return;
  }

  while (nextChannel == 0 || channels.count(nextChannel))
    nextChannel++;

  Device* device = new Device;
  device->name = name;
  device->label = label.substr(0, 255);
  device->kind = kind;
  device->control = control;
  device->channel = nextChannel++;
  devices[name] = device;
  channels[device->channel] = device;

  std::vector<uint8_t> msg;
  msg.push_back(kind);
  msg.push_back(name.size());
  msg.insert(msg.end(), name.begin(), name.end());
  msg.push_back(device->label.size());
  msg.insert(msg.end(), device->label.begin(), device->label.end());
  // The viewer waits for a device that another session uses
  msg.push_back(wait >= 0 ? deviceLinkRequestWait : 0);
  msg.push_back((wait > 0 ? wait : 0) >> 8);
  msg.push_back(wait > 0 ? wait : 0);
  send(deviceLinkRequest, device->channel, msg.data(), msg.size());
}

void DeviceLinkHost::release(Control* control, const std::string& name,
                             bool ack)
{
  auto it = devices.find(name);
  if (it == devices.end())
    return;

  Device* device = it->second;
  devices.erase(it);
  device->control = nullptr;
  if (ack)
    device->releasedBy = control;
  closeEndpoint(device);

  if (!ownerPresent) {
    finish(device);
    return;
  }

  uint8_t reason = deviceLinkCloseNormal;
  send(deviceLinkClose, device->channel, &reason, 1);
  device->state = Device::Closing;
}

// -=- Devices

bool DeviceLinkHost::openEndpoint(Device* device)
{
  device->path = dir + "/" + device->name;
  unlink(device->path.c_str());

  if (device->kind != deviceLinkSerial) {
    device->listenFd = listenUnix(device->path);
    return device->listenFd >= 0;
  }

  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0)
    return false;
  setNonBlock(master);

  const char* slaveName;
  if (grantpt(master) != 0 || unlockpt(master) != 0 ||
      (slaveName = ptsname(master)) == nullptr) {
    close(master);
    return false;
  }
  std::string slavePath(slaveName);

  // We keep the slave open ourselves, so that the master does not hang
  // up while no application has the device open
  int slave = open(slavePath.c_str(), O_RDWR | O_NOCTTY);
  if (slave < 0) {
    close(master);
    return false;
  }
  fcntl(slave, F_SETFD, FD_CLOEXEC);

  // Raw like a serial port. EXTPROC makes the kernel tell us about
  // every tcsetattr() of the application. Speed 0 means "not set", so
  // that the first speed the application sets is reported.
  struct termios tio;
  tcgetattr(slave, &tio);
  cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  tio.c_lflag |= EXTPROC;
  cfsetispeed(&tio, B0);
  cfsetospeed(&tio, B0);
  tcsetattr(slave, TCSANOW, &tio);

  int one = 1;
  if (ioctl(master, TIOCPKT, &one) != 0 ||
      symlink(slavePath.c_str(), device->path.c_str()) != 0) {
    close(slave);
    close(master);
    return false;
  }

  device->ptyFd = master;
  device->slaveFd = slave;
  device->termSpeed = 0;
  device->termFlow = deviceLinkFlowNone;
  return true;
}

void DeviceLinkHost::closeEndpoint(Device* device)
{
  int* fds[] = { &device->ptyFd, &device->slaveFd, &device->listenFd,
                 &device->connFd };

  for (int* fd : fds) {
    if (*fd < 0)
      continue;
    if (fd != &device->slaveFd)
      watcher->unwatchFd(*fd);
    close(*fd);
    *fd = -1;
  }

  if (!device->path.empty())
    unlink(device->path.c_str());
  device->path.clear();
  device->toApp.clear();
}

void DeviceLinkHost::updateWatches(Device* device)
{
  bool open = device->state == Device::Open;
  bool canSend = open && device->sendWindow > 0;
  bool hasOutput = !device->toApp.empty();

  if (device->ptyFd >= 0)
    watcher->watchFd(device->ptyFd, canSend, hasOutput);
  if (device->listenFd >= 0)
    watcher->watchFd(device->listenFd, open, false);
  if (device->connFd >= 0)
    watcher->watchFd(device->connFd,
                     canSend || device->kind == deviceLinkTail,
                     hasOutput);
}

void DeviceLinkHost::handleDevice(Device* device, int fd, bool read,
                                  bool write)
{
  if (fd == device->listenFd) {
    int conn = accept(device->listenFd, nullptr, nullptr);
    if (conn < 0)
      return;
    if (device->connFd >= 0) {
      // One application at a time
      close(conn);
      return;
    }
    setNonBlock(conn);
    device->connFd = conn;
    updateWatches(device);
    return;
  }

  if (write)
    writeToApp(device);

  if (read) {
    if (fd == device->ptyFd)
      readPty(device);
    else
      readConnection(device);
  }
}

void DeviceLinkHost::readPty(Device* device)
{
  uint8_t buf[1 + deviceLinkMaxData];
  size_t want = device->sendWindow;

  if (want > deviceLinkMaxData)
    want = deviceLinkMaxData;
  if (want == 0)
    return;

  ssize_t n = ::read(device->ptyFd, buf, 1 + want);
  if (n <= 0)
    return;

  if (buf[0] == TIOCPKT_DATA) {
    forwardFromApp(device, buf + 1, n - 1);
    return;
  }

  // A status packet: the application changed the terminal
  if (buf[0] & TIOCPKT_FLUSHREAD) {
    // The application discarded what the device sent
    uint8_t which = deviceLinkPurgeReceive;
    send(deviceLinkPurge, device->channel, &which, 1);
  }
  if (buf[0] & TIOCPKT_FLUSHWRITE) {
    uint8_t which = deviceLinkPurgeTransmit;
    send(deviceLinkPurge, device->channel, &which, 1);
  }
#ifdef TIOCPKT_IOCTL
  if (buf[0] & TIOCPKT_IOCTL)
    termiosChanged(device);
#endif
}

void DeviceLinkHost::termiosChanged(Device* device)
{
  struct termios tio;
  uint8_t mask = 0;

  if (tcgetattr(device->ptyFd, &tio) != 0)
    return;

  uint32_t speed = speedOf(cfgetospeed(&tio));
  uint8_t flow = deviceLinkFlowNone;
  if (tio.c_cflag & CRTSCTS)
    flow = deviceLinkFlowRTSCTS;
  else if (tio.c_iflag & (IXON | IXOFF))
    flow = deviceLinkFlowXONXOFF;

  if (speed != 0 && speed != device->termSpeed) {
    device->speed = speed;
    mask |= 1;
  }
  if (flow != device->termFlow) {
    device->flow = flow;
    mask |= 4;
  }
  device->termSpeed = speed;
  device->termFlow = flow;

  if (mask)
    sendParams(device, mask);

  // Applications often clear all local flags; we need EXTPROC to hear
  // about the next change
  if (!(tio.c_lflag & EXTPROC)) {
    tio.c_lflag |= EXTPROC;
    tcsetattr(device->ptyFd, TCSANOW, &tio);
  }
}

void DeviceLinkHost::readConnection(Device* device)
{
  uint8_t buf[deviceLinkMaxData];
  size_t want = device->sendWindow;

  if (want > sizeof(buf))
    want = sizeof(buf);
  // A tail device only sends; we read to notice the end
  if (device->kind == deviceLinkTail)
    want = sizeof(buf);
  if (want == 0)
    return;

  ssize_t n = ::read(device->connFd, buf, want);
  if (n < 0 && (errno == EAGAIN || errno == EINTR))
    return;
  if (n <= 0) {
    watcher->unwatchFd(device->connFd);
    close(device->connFd);
    device->connFd = -1;
    return;
  }

  if (device->kind != deviceLinkTail)
    forwardFromApp(device, buf, n);
}

void DeviceLinkHost::forwardFromApp(Device* device, const uint8_t* data,
                                    size_t len)
{
  if (len == 0)
    return;
  send(deviceLinkData, device->channel, data, len);
  device->sendWindow -= len;
  updateWatches(device);
}

void DeviceLinkHost::writeToApp(Device* device)
{
  int fd = device->kind == deviceLinkSerial ? device->ptyFd :
                                              device->connFd;
  if (fd < 0 || device->toApp.empty())
    return;

  ssize_t n = ::write(fd, device->toApp.data(), device->toApp.size());
  if (n < 0 && errno != EAGAIN && errno != EINTR &&
      device->kind != deviceLinkSerial) {
    // The application went away; keep the data for the next one
    watcher->unwatchFd(device->connFd);
    close(device->connFd);
    device->connFd = -1;
    return;
  }

  if (n > 0) {
    device->toApp.erase(0, n);
    uint8_t credit[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16),
                          (uint8_t)(n >> 8), (uint8_t)n };
    send(deviceLinkCredit, device->channel, credit, sizeof(credit));
  }

  updateWatches(device);
}

// Tells the application that it no longer has the device: "lost NAME",
// "error NAME REASON"
void DeviceLinkHost::lose(Device* device, const std::string& event,
                          const std::string& detail)
{
  if (device->control != nullptr)
    reply(device->control, event + " " + device->name +
                           (detail.empty() ? "" : " " + detail));
  devices.erase(device->name);
  device->control = nullptr;
  closeEndpoint(device);
}

void DeviceLinkHost::finish(Device* device)
{
  if (device->releasedBy != nullptr)
    reply(device->releasedBy, "ok " + device->name);

  if (device->control != nullptr)
    devices.erase(device->name);
  closeEndpoint(device);
  channels.erase(device->channel);
  delete device;
}

// -=- Messages from the viewer

void DeviceLinkHost::ownerChanged(bool present)
{
  ownerPresent = present;

  if (!present) {
    while (!channels.empty()) {
      Device* device = channels.begin()->second;
      if (device->state == Device::Pending)
        lose(device, "error", "no-viewer");
      else if (device->state == Device::Open)
        lose(device, "lost", "");
      finish(device);
    }
  }

  broadcast(present ? "viewer present" : "viewer absent");
}

void DeviceLinkHost::handleMessage(uint8_t subtype, uint16_t channel,
                                   const uint8_t* data, size_t len)
{
  auto it = channels.find(channel);
  if (it == channels.end()) {
    // Late messages for a channel that is gone are normal
    vlog.debug("Ignoring device link message %d for channel %d",
               subtype, channel);
    return;
  }
  Device* device = it->second;

  switch (subtype) {
  case deviceLinkRequestResult:
    handleRequestResult(device, data, len);
    break;
  case deviceLinkData:
    handleData(device, data, len);
    break;
  case deviceLinkCredit:
    handleCredit(device, data, len);
    break;
  case deviceLinkLineState:
    handleLineState(device, data, len);
    break;
  case deviceLinkClose:
    handleClose(device);
    break;
  default:
    vlog.debug("Ignoring unknown device link message %d", subtype);
  }
}

void DeviceLinkHost::handleRequestResult(Device* device,
                                         const uint8_t* data, size_t len)
{
  static const char* reasons[] = {
    "open", "refused", "unavailable", "busy", "not-configured"
  };

  if (device->state != Device::Pending || len < 3)
    return;

  uint8_t status = data[0];
  size_t msgLen = (data[1] << 8) | data[2];
  std::string message;
  if (3 + msgLen <= len)
    message.assign((const char*)data + 3, msgLen);
  for (char& c : message) {
    if (c == '\n' || c == '\r')
      c = ' ';
  }

  if (status != deviceLinkOpen) {
    // A refused request ends the channel without Close
    std::string reason(status < sizeof(reasons) / sizeof(reasons[0]) ?
                       reasons[status] : "unavailable");
    if (!message.empty())
      reason += " " + message;
    lose(device, "error", reason);
    finish(device);
    return;
  }

  if (!openEndpoint(device)) {
    vlog.error("Cannot create the endpoint for %s: %s",
               device->name.c_str(), strerror(errno));
    reply(device->control, "error " + device->name + " local-failure");
    devices.erase(device->name);
    device->control = nullptr;
    closeEndpoint(device);
    uint8_t reason = deviceLinkCloseError;
    send(deviceLinkClose, device->channel, &reason, 1);
    device->state = Device::Closing;
    return;
  }

  device->state = Device::Open;
  reply(device->control, "ok " + device->name + " " + device->path);
  updateWatches(device);
}

void DeviceLinkHost::handleData(Device* device, const uint8_t* data,
                                size_t len)
{
  if (device->state != Device::Open)
    return;

  if (device->toApp.size() + len > deviceLinkWindow) {
    vlog.error("Viewer exceeded the window of %s", device->name.c_str());
    lose(device, "lost", "");
    uint8_t reason = deviceLinkCloseError;
    send(deviceLinkClose, device->channel, &reason, 1);
    device->state = Device::Closing;
    return;
  }

  device->toApp.append((const char*)data, len);
  writeToApp(device);
}

void DeviceLinkHost::handleCredit(Device* device, const uint8_t* data,
                                  size_t len)
{
  if (len < 4)
    return;
  uint32_t credit = ((uint32_t)data[0] << 24) | (data[1] << 16) |
                    (data[2] << 8) | data[3];
  if (device->sendWindow + (uint64_t)credit > deviceLinkWindow)
    device->sendWindow = deviceLinkWindow;
  else
    device->sendWindow += credit;
  updateWatches(device);
}

void DeviceLinkHost::handleLineState(Device* device, const uint8_t* data,
                                     size_t len)
{
  if (len < 2 || device->control == nullptr ||
      device->state != Device::Open)
    return;

  uint8_t lines = data[0], errors = data[1];
  std::string event("lines " + device->name);
  event += std::string(" cts=") + (lines & deviceLinkCTS ? "1" : "0");
  event += std::string(" dsr=") + (lines & deviceLinkDSR ? "1" : "0");
  event += std::string(" ri=") + (lines & deviceLinkRI ? "1" : "0");
  event += std::string(" dcd=") + (lines & deviceLinkDCD ? "1" : "0");
  reply(device->control, event);

  static const struct { uint8_t bit; const char* name; } errorNames[] = {
    { deviceLinkErrFraming, "framing" }, { deviceLinkErrParity, "parity" },
    { deviceLinkErrOverrun, "overrun" }, { deviceLinkErrBreak, "break" },
  };
  for (const auto& e : errorNames) {
    if ((errors & e.bit) && device->control != nullptr)
      reply(device->control,
            std::string("error ") + device->name + " " + e.name);
  }
}

void DeviceLinkHost::handleClose(Device* device)
{
  if (device->state != Device::Closing) {
    if (device->state == Device::Pending)
      lose(device, "error", "closed");
    else
      lose(device, "lost", "");
    uint8_t reason = deviceLinkCloseNormal;
    send(deviceLinkClose, device->channel, &reason, 1);
  }
  finish(device);
}

void DeviceLinkHost::send(uint8_t subtype, uint16_t channel,
                          const uint8_t* data, size_t len)
{
  server->sendDeviceLink(subtype, channel, data, len);
}

void DeviceLinkHost::sendParams(Device* device, uint8_t mask)
{
  uint8_t msg[9];

  msg[0] = mask;
  msg[1] = device->speed >> 24;
  msg[2] = device->speed >> 16;
  msg[3] = device->speed >> 8;
  msg[4] = device->speed;
  msg[5] = device->dataBits;
  msg[6] = device->parity;
  msg[7] = device->stopBits;
  msg[8] = device->flow;
  send(deviceLinkSetParams, device->channel, msg, sizeof(msg));
}
