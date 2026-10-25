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

#ifdef WIN32
#include <winsock2.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#endif

#include <list>

#include <FL/Fl.H>

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <network/TcpSocket.h>
#include <rfb/CMsgWriter.h>
#include <rfb/DesktopEndpoint.h>

#include "CConn.h"
#include "FileDropTunnel.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static core::LogWriter vlog("FileDropTunnel");

// How often to look again while the RFB output is not drained
static const double retrySeconds = 0.02;

static void setNonBlocking(int fd)
{
#ifdef WIN32
  u_long on = 1;
  ioctlsocket(fd, FIONBIO, &on);
#else
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#endif
}

static bool wouldBlock()
{
#ifdef WIN32
  int err = WSAGetLastError();
  return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

FileDropTunnel::Channel::Channel()
  : sock(nullptr), open(false), sendCredit(rfb::endpointTunnelWindow),
    closeSent(false), closeReceived(false), watched(0)
{
}

FileDropTunnel::FileDropTunnel(CConn* cc_, const std::string& service_)
  : cc(cc_), service(service_), listener(nullptr), localPort(0), nextId(0),
    retrying(false)
{
  std::list<network::SocketListener*> listeners;

  try {
    network::createTcpListeners(&listeners, "127.0.0.1", 0);
  } catch (std::exception& e) {
    vlog.error(_("Cannot listen for the file drop tunnel: %s"), e.what());
    return;
  }
  if (listeners.empty())
    return;

  listener = listeners.front();
  listeners.pop_front();
  for (network::SocketListener* l : listeners)
    delete l;

  localPort = listener->getMyPort();
  Fl::add_fd(listener->getFd(), FL_READ, acceptEvent, this);
  vlog.debug("Tunnel for %s on 127.0.0.1:%d", service.c_str(), localPort);
}

FileDropTunnel::~FileDropTunnel()
{
  Fl::remove_timeout(retryTimeout, this);

  for (auto& c : channels) {
    if (c.second.watched)
      Fl::remove_fd(c.second.sock->getFd());
    delete c.second.sock;
  }

  if (listener) {
    Fl::remove_fd(listener->getFd());
    delete listener;
  }
}

void FileDropTunnel::acceptEvent(FL_SOCKET, void* data)
{
  ((FileDropTunnel*)data)->accept();
}

void FileDropTunnel::accept()
{
  network::Socket* sock = listener->accept();

  if (sock == nullptr)
    return;

  // A free channel number; the server bounds how many are open
  uint16_t id = nextId;
  while (channels.count(id))
    id++;
  nextId = id + 1;

  setNonBlocking(sock->getFd());
  Channel& ch = channels[id];
  ch.sock = sock;

  std::vector<uint8_t> payload(2 + service.size());
  payload[0] = service.size() >> 8;
  payload[1] = service.size();
  service.copy((char*)payload.data() + 2, service.size());
  send(rfb::endpointTunnelOpen, id, payload.data(), payload.size());
  // Nothing is read until the server has connected the channel
}

void FileDropTunnel::handleMessage(uint8_t subtype, uint16_t id,
                                   const uint8_t* data, size_t len)
{
  std::map<uint16_t, Channel>::iterator it = channels.find(id);

  // Messages for channels we dropped already (crossing a Close)
  if (it == channels.end())
    return;
  Channel* ch = &it->second;

  switch (subtype) {
  case rfb::endpointTunnelOpenResult:
    if (len != 1 || ch->open)
      return;
    if (data[0] != rfb::endpointTunnelConnected) {
      vlog.error(_("The server could not connect the file drop tunnel "
                   "(status %d)"), (int)data[0]);
      // The channel ends without Close
      drop(id);
      return;
    }
    ch->open = true;
    updateWatch(ch);
    return;

  case rfb::endpointTunnelData:
    if (!ch->open || ch->closeSent)
      return;
    if (ch->toLocal.size() + len > rfb::endpointTunnelWindow) {
      vlog.error(_("The server exceeded the file drop tunnel window"));
      sendClose(id, ch, rfb::endpointTunnelError);
      return;
    }
    ch->toLocal.insert(ch->toLocal.end(), data, data + len);
    writeLocal(id, ch);
    return;

  case rfb::endpointTunnelCredit:
    if (len != 4)
      return;
    ch->sendCredit += ((uint32_t)data[0] << 24) | (data[1] << 16) |
                      (data[2] << 8) | data[3];
    updateWatch(ch);
    return;

  case rfb::endpointTunnelClose:
    if (!ch->closeSent) {
      uint8_t reason = rfb::endpointTunnelNormal;
      ch->closeSent = true;
      send(rfb::endpointTunnelClose, id, &reason, 1);
    }
    ch->closeReceived = true;
    // What the endpoint sent before it closed still goes out
    if (ch->toLocal.empty())
      drop(id);
    else
      writeLocal(id, ch);
    return;

  default:
    return;
  }
}

void FileDropTunnel::channelEvent(FL_SOCKET fd, void* data)
{
  FileDropTunnel* self = (FileDropTunnel*)data;

  for (auto& c : self->channels) {
    if ((FL_SOCKET)c.second.sock->getFd() != fd)
      continue;
    uint16_t id = c.first;
    if (!c.second.toLocal.empty())
      self->writeLocal(id, &c.second);
    std::map<uint16_t, Channel>::iterator it = self->channels.find(id);
    if (it != self->channels.end())
      self->readLocal(id, &it->second);
    return;
  }
}

void FileDropTunnel::readLocal(uint16_t id, Channel* ch)
{
  uint8_t buf[rfb::endpointTunnelMaxData];

  if (!ch->open || ch->closeSent || ch->sendCredit == 0)
    return;

  // Framebuffer requests and input go first: wait while the RFB output
  // is not out yet
  if (cc->outputPending()) {
    updateAll();
    return;
  }

  size_t want = ch->sendCredit;
  if (want > sizeof(buf))
    want = sizeof(buf);

  long n = recv(ch->sock->getFd(), (char*)buf, want, 0);
  if (n > 0) {
    ch->sendCredit -= n;
    send(rfb::endpointTunnelData, id, buf, n);
    updateWatch(ch);
    return;
  }
  if (n < 0 && wouldBlock())
    return;

  // The HTTP client closed its connection (or it broke)
  sendClose(id, ch, n == 0 ? rfb::endpointTunnelNormal
                           : rfb::endpointTunnelError);
}

void FileDropTunnel::writeLocal(uint16_t id, Channel* ch)
{
  size_t sent = 0;

  while (sent < ch->toLocal.size()) {
    long n = ::send(ch->sock->getFd(), (const char*)ch->toLocal.data() + sent,
                    ch->toLocal.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += n;
      continue;
    }
    if (n < 0 && wouldBlock())
      break;
    ch->toLocal.clear();
    if (ch->closeReceived) {
      drop(id);
      return;
    }
    sendClose(id, ch, rfb::endpointTunnelError);
    return;
  }

  if (sent > 0) {
    ch->toLocal.erase(ch->toLocal.begin(), ch->toLocal.begin() + sent);
    if (!ch->closeSent) {
      uint8_t credit[4] = { (uint8_t)(sent >> 24), (uint8_t)(sent >> 16),
                            (uint8_t)(sent >> 8), (uint8_t)sent };
      send(rfb::endpointTunnelCredit, id, credit, 4);
    }
  }
  if (ch->closeReceived && ch->toLocal.empty()) {
    drop(id);
    return;
  }
  updateWatch(ch);
}

// We end the channel; it is dropped when the server's Close arrives
void FileDropTunnel::sendClose(uint16_t id, Channel* ch, uint8_t reason)
{
  if (ch->closeSent)
    return;
  ch->closeSent = true;
  ch->toLocal.clear();
  ch->sock->shutdownWrite();
  send(rfb::endpointTunnelClose, id, &reason, 1);
  updateWatch(ch);
}

void FileDropTunnel::drop(uint16_t id)
{
  std::map<uint16_t, Channel>::iterator it = channels.find(id);

  if (it == channels.end())
    return;
  if (it->second.watched)
    Fl::remove_fd(it->second.sock->getFd());
  delete it->second.sock;
  channels.erase(it);
}

void FileDropTunnel::updateWatch(Channel* ch)
{
  int when = 0;

  if (ch->open && !ch->closeSent) {
    if (ch->sendCredit > 0 && !cc->outputPending())
      when |= FL_READ;
    if (!ch->toLocal.empty())
      when |= FL_WRITE;
  } else if (ch->closeReceived && !ch->toLocal.empty()) {
    when |= FL_WRITE;
  }

  if (when == ch->watched)
    return;
  if (ch->watched)
    Fl::remove_fd(ch->sock->getFd());
  if (when)
    Fl::add_fd(ch->sock->getFd(), when, channelEvent, this);
  ch->watched = when;

  // Reading waits for the RFB output; look again shortly
  if (ch->open && !ch->closeSent && ch->sendCredit > 0 &&
      !(when & FL_READ) && !retrying) {
    retrying = true;
    Fl::add_timeout(retrySeconds, retryTimeout, this);
  }
}

void FileDropTunnel::updateAll()
{
  for (auto& c : channels)
    updateWatch(&c.second);
}

void FileDropTunnel::retryTimeout(void* data)
{
  FileDropTunnel* self = (FileDropTunnel*)data;

  self->retrying = false;
  self->cc->flushOutput();
  self->updateAll();
}

void FileDropTunnel::send(uint8_t subtype, uint16_t id,
                          const uint8_t* data, size_t len)
{
  try {
    cc->writer()->writeEndpointTunnel(subtype, id, data, len);
  } catch (std::exception& e) {
    // The connection itself reports a broken socket
    vlog.error("%s", e.what());
  }
}
