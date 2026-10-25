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

#include <string.h>

#ifndef WIN32
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <core/LogWriter.h>
#include <core/i18n.h>

#include <rfb/EndpointTunnel.h>
#include <rfb/Exception.h>
#include <rfb/SMsgWriter.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

using namespace rfb;

static core::LogWriter vlog("EndpointTunnel");

EndpointTunnel::Channel::Channel()
  : fd(-1), connecting(false), nextAddr(0), sendCredit(endpointTunnelWindow),
    endpointDone(false), closeReceived(false)
{
}

EndpointTunnel::EndpointTunnel(std::list<int>* closedFds_)
  : closedFds(closedFds_)
{
}

EndpointTunnel::~EndpointTunnel()
{
  for (auto& c : channels)
    closeFd(&c.second);
}

#ifndef WIN32
// Host and port of an http(s) URL; false if it is not one
static bool hostAndPort(const std::string& url, std::string* host,
                        std::string* port)
{
  size_t start, end;

  if (url.compare(0, 7, "http://") == 0) {
    start = 7;
    *port = "80";
  } else if (url.compare(0, 8, "https://") == 0) {
    start = 8;
    *port = "443";
  } else {
    return false;
  }

  end = url.find_first_of("/?#", start);
  if (end == std::string::npos)
    end = url.size();
  std::string authority(url, start, end - start);
  if (authority.find('@') != std::string::npos)
    return false;

  size_t colon;
  if (!authority.empty() && authority[0] == '[') {
    size_t close = authority.find(']');
    if (close == std::string::npos)
      return false;
    *host = authority.substr(1, close - 1);
    colon = close + 1;
    if (colon < authority.size() && authority[colon] != ':')
      return false;
  } else {
    colon = authority.find(':');
    *host = authority.substr(0, colon);
  }
  if (colon != std::string::npos && colon < authority.size()) {
    *port = authority.substr(colon + 1);
    if (port->empty() ||
        port->find_first_not_of("0123456789") != std::string::npos)
      return false;
  }

  return !host->empty();
}
#endif

void EndpointTunnel::handleMessage(uint8_t subtype, uint16_t id,
                                   const uint8_t* data, size_t len,
                                   const std::list<DesktopEndpoint>& endpoints,
                                   SMsgWriter* writer)
{
  std::map<uint16_t, Channel>::iterator it = channels.find(id);

  switch (subtype) {
  case endpointTunnelOpen: {
    if (it != channels.end())
      throw protocol_error(_("Endpoint tunnel channel already in use"));
    if (len < 2 || len != 2u + ((data[0] << 8) | data[1]))
      throw protocol_error(_("Invalid endpoint tunnel request"));
    std::string service((const char*)data + 2, len - 2);

    const DesktopEndpoint* endpoint = nullptr;
    for (const DesktopEndpoint& e : endpoints) {
      if (e.service == service && (e.flags & desktopEndpointTunnel))
        endpoint = &e;
    }

    uint8_t status = endpointTunnelRefused;
#ifndef WIN32
    std::string host, port;
    if (endpoint == nullptr) {
      vlog.info(_("No tunnel for service \"%s\""), service.c_str());
    } else if (channels.size() >= maxChannels) {
      status = endpointTunnelTooMany;
    } else if (!hostAndPort(endpoint->url, &host, &port)) {
      vlog.error(_("Cannot tunnel to %s"), endpoint->url.c_str());
    } else {
      struct addrinfo hints, *results;
      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      // Endpoints are on the desktop's own host or nearby, so the name
      // is looked up synchronously
      status = endpointTunnelUnreachable;
      if (getaddrinfo(host.c_str(), port.c_str(), &hints, &results) == 0) {
        Channel ch;
        for (struct addrinfo* r = results; r; r = r->ai_next) {
          const uint8_t* a = (const uint8_t*)r->ai_addr;
          ch.addrs.push_back(std::vector<uint8_t>(a, a + r->ai_addrlen));
        }
        freeaddrinfo(results);

        if (connectNext(&ch)) {
          bool connecting = ch.connecting;
          channels[id] = ch;
          if (connecting)
            return;
          status = endpointTunnelConnected;
        }
      }
    }
#else
    (void)endpoint;
#endif
    writer->writeEndpointTunnel(endpointTunnelOpenResult, id, &status, 1);
    return;
  }

  case endpointTunnelData:
    if (it == channels.end() || it->second.connecting ||
        it->second.closeReceived)
      throw protocol_error(_("Endpoint tunnel data for a channel not open"));
    if (it->second.toEndpoint.size() + len > endpointTunnelWindow)
      throw protocol_error(_("Endpoint tunnel window exceeded"));
    it->second.toEndpoint.insert(it->second.toEndpoint.end(),
                                 data, data + len);
    writeEndpoint(id, &it->second, writer);
    return;

  case endpointTunnelCredit: {
    if (len != 4)
      throw protocol_error(_("Invalid endpoint tunnel credit"));
    // Credit for a channel we ended already crossed our Close
    if (it == channels.end())
      return;
    uint32_t credit = ((uint32_t)data[0] << 24) | (data[1] << 16) |
                      (data[2] << 8) | data[3];
    if (credit > endpointTunnelWindow - it->second.sendCredit)
      throw protocol_error(_("Endpoint tunnel window exceeded"));
    it->second.sendCredit += credit;
    return;
  }

  case endpointTunnelClose:
    // The client answers our Close, or both closed at the same time
    if (it == channels.end())
      return;
    it->second.closeReceived = true;
    if (it->second.toEndpoint.empty())
      finish(id, endpointTunnelNormal, writer);
    return;

  default:
    // Receivers skip subtypes they do not know
    return;
  }
}

void EndpointTunnel::getFds(std::list<Fd>* fds, bool clientDrained) const
{
  for (const auto& c : channels) {
    const Channel& ch = c.second;
    Fd f;

    if (ch.fd < 0)
      continue;

    f.fd = ch.fd;
    if (ch.connecting) {
      f.read = false;
      f.write = true;
    } else {
      f.read = clientDrained && ch.sendCredit > 0 && !ch.endpointDone &&
               !ch.closeReceived;
      f.write = !ch.toEndpoint.empty();
    }
    fds->push_back(f);
  }
}

bool EndpointTunnel::handleEvent(int fd, bool read, bool write,
                                 SMsgWriter* writer, bool clientDrained)
{
  std::map<uint16_t, Channel>::iterator it;

  for (it = channels.begin(); it != channels.end(); ++it) {
    if (it->second.fd == fd)
      break;
  }
  if (it == channels.end())
    return false;

#ifndef WIN32
  uint16_t id = it->first;
  Channel* ch = &it->second;

  if (ch->connecting) {
    if (!write)
      return true;

    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0)
      err = errno;
    if (err == 0) {
      uint8_t status = endpointTunnelConnected;
      ch->connecting = false;
      writer->writeEndpointTunnel(endpointTunnelOpenResult, id, &status, 1);
      return true;
    }

    closeFd(ch);
    if (!connectNext(ch)) {
      uint8_t status = endpointTunnelUnreachable;
      channels.erase(it);
      writer->writeEndpointTunnel(endpointTunnelOpenResult, id, &status, 1);
    } else if (!ch->connecting) {
      uint8_t status = endpointTunnelConnected;
      writer->writeEndpointTunnel(endpointTunnelOpenResult, id, &status, 1);
    }
    return true;
  }

  if (write)
    writeEndpoint(id, ch, writer);

  // writeEndpoint() may have ended the channel
  it = channels.find(id);
  if (read && clientDrained && it != channels.end())
    readEndpoint(id, &it->second, writer);
#else
  (void)read;
  (void)write;
  (void)writer;
  (void)clientDrained;
#endif

  return true;
}

// Starts a connection to the next address; false if none is left. On
// success, ch->connecting tells whether it is still in progress.
bool EndpointTunnel::connectNext(Channel* ch)
{
#ifndef WIN32
  while (ch->nextAddr < ch->addrs.size()) {
    const std::vector<uint8_t>& a = ch->addrs[ch->nextAddr++];
    const struct sockaddr* sa = (const struct sockaddr*)a.data();

    int fd = socket(sa->sa_family, SOCK_STREAM, 0);
    if (fd < 0)
      continue;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    if (connect(fd, sa, a.size()) == 0) {
      ch->fd = fd;
      ch->connecting = false;
      return true;
    }
    if (errno == EINPROGRESS) {
      ch->fd = fd;
      ch->connecting = true;
      return true;
    }
    ::close(fd);
  }
#else
  (void)ch;
#endif
  return false;
}

void EndpointTunnel::readEndpoint(uint16_t id, Channel* ch,
                                  SMsgWriter* writer)
{
#ifndef WIN32
  uint8_t buf[endpointTunnelMaxData];

  // A few chunks per event, so that one channel does not hold up the
  // others
  for (int i = 0; i < 4 && ch->sendCredit > 0; i++) {
    size_t want = ch->sendCredit;
    if (want > sizeof(buf))
      want = sizeof(buf);

    ssize_t n = recv(ch->fd, buf, want, 0);
    if (n > 0) {
      ch->sendCredit -= n;
      writer->writeEndpointTunnel(endpointTunnelData, id, buf, n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      return;

    ch->endpointDone = true;
    finish(id, n == 0 ? endpointTunnelNormal : endpointTunnelError, writer);
    return;
  }
#else
  (void)id;
  (void)ch;
  (void)writer;
#endif
}

void EndpointTunnel::writeEndpoint(uint16_t id, Channel* ch,
                                   SMsgWriter* writer)
{
#ifndef WIN32
  if (ch->connecting)
    return;

  while (!ch->toEndpoint.empty()) {
    ssize_t n = send(ch->fd, ch->toEndpoint.data(), ch->toEndpoint.size(),
                     MSG_NOSIGNAL);
    if (n > 0) {
      uint8_t credit[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16),
                            (uint8_t)(n >> 8), (uint8_t)n };
      ch->toEndpoint.erase(ch->toEndpoint.begin(),
                           ch->toEndpoint.begin() + n);
      if (!ch->closeReceived)
        writer->writeEndpointTunnel(endpointTunnelCredit, id, credit, 4);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      return;

    finish(id, endpointTunnelError, writer);
    return;
  }

  if (ch->closeReceived)
    finish(id, endpointTunnelNormal, writer);
#else
  (void)id;
  (void)ch;
  (void)writer;
#endif
}

// Ends a channel and tells the client: we start the close, or answer
// its Close
void EndpointTunnel::finish(uint16_t id, uint8_t reason, SMsgWriter* writer)
{
  std::map<uint16_t, Channel>::iterator it = channels.find(id);

  if (it == channels.end())
    return;

  closeFd(&it->second);
  channels.erase(it);

  writer->writeEndpointTunnel(endpointTunnelClose, id, &reason, 1);
}

void EndpointTunnel::closeFd(Channel* ch)
{
  if (ch->fd < 0)
    return;
  // The desktop may still watch it; it stops and closes it
  closedFds->push_back(ch->fd);
  ch->fd = -1;
}
