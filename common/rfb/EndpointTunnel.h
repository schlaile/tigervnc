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

// The server side of tunnels through RFB to the endpoint of a
// side-channel service (doc/file-drop.md), for one client. Each channel
// is one TCP connection to the host and port of the announced endpoint
// URL; the desktop watches the descriptors and reports their events.

#ifndef __RFB_ENDPOINTTUNNEL_H__
#define __RFB_ENDPOINTTUNNEL_H__

#include <stdint.h>

#include <list>
#include <map>
#include <string>
#include <vector>

#include <rfb/DesktopEndpoint.h>

namespace rfb {

  class SMsgWriter;

  class EndpointTunnel {
  public:
    // A descriptor to watch, and for what
    struct Fd {
      int fd;
      bool read;
      bool write;
    };

    // closedFds: where descriptors of ended channels go; the desktop
    // stops watching and closes them (see VNCServerST)
    EndpointTunnel(std::list<int>* closedFds);
    ~EndpointTunnel();

    // A message from the client. Throws protocol_error on violations.
    void handleMessage(uint8_t subtype, uint16_t channel,
                       const uint8_t* data, size_t len,
                       const std::list<DesktopEndpoint>& endpoints,
                       SMsgWriter* writer);

    // The descriptors to watch. clientDrained: whether the output to
    // the client is empty (data from endpoints is read only then, so
    // that framebuffer updates and input go first).
    void getFds(std::list<Fd>* fds, bool clientDrained) const;

    // An event on one of our descriptors; false if it is not ours
    bool handleEvent(int fd, bool read, bool write, SMsgWriter* writer,
                     bool clientDrained);

    // Bound on open channels per client
    static const size_t maxChannels = 8;

  private:
    struct Channel {
      Channel();

      int fd;
      bool connecting;
      // addresses of the endpoint (struct sockaddr) still to try while
      // connecting
      std::vector<std::vector<uint8_t>> addrs;
      size_t nextAddr;
      // bytes from the client, not yet written to the endpoint
      std::vector<uint8_t> toEndpoint;
      // bytes we may still send to the client
      uint32_t sendCredit;
      // the endpoint closed its side
      bool endpointDone;
      // the client sent Close; the channel ends when toEndpoint is out
      bool closeReceived;
    };

    bool connectNext(Channel* ch);
    void readEndpoint(uint16_t id, Channel* ch, SMsgWriter* writer);
    void writeEndpoint(uint16_t id, Channel* ch, SMsgWriter* writer);
    void finish(uint16_t id, uint8_t reason, SMsgWriter* writer);
    void closeFd(Channel* ch);

    std::map<uint16_t, Channel> channels;
    std::list<int>* closedFds;
  };

}

#endif
