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

// A side-channel service of the desktop, announced to clients with the
// DesktopEndpoint pseudo-encoding (see doc/file-drop.md)

#ifndef __RFB_DESKTOPENDPOINT_H__
#define __RFB_DESKTOPENDPOINT_H__

#include <stdint.h>

#include <string>
#include <vector>

namespace rfb {

  // The client replaces the host of the URL with the host it used for
  // the RFB connection
  const uint8_t desktopEndpointSameHost = 1 << 0;
  // The server carries connections to the endpoint through RFB
  // (EndpointTunnel messages)
  const uint8_t desktopEndpointTunnel = 1 << 1;

  const uint8_t desktopEndpointVersion = 1;

  // Limits of the announcement, also checked when reading it
  const size_t desktopEndpointMaxService = 64;
  const size_t desktopEndpointMaxURL = 2048;
  const size_t desktopEndpointMaxToken = 1024;

  // EndpointTunnel messages (both directions)
  enum EndpointTunnelSubtype {
    endpointTunnelOpen = 0,             // C->S
    endpointTunnelOpenResult = 1,       // S->C
    endpointTunnelData = 2,
    endpointTunnelCredit = 3,
    endpointTunnelClose = 4,
  };

  enum EndpointTunnelStatus {
    endpointTunnelConnected = 0,
    endpointTunnelRefused = 1,
    endpointTunnelUnreachable = 2,
    endpointTunnelTooMany = 3,
  };

  enum EndpointTunnelCloseReason {
    endpointTunnelNormal = 0,
    endpointTunnelError = 1,
  };

  // Largest Data payload, largest other payload
  const size_t endpointTunnelMaxData = 4096;
  const size_t endpointTunnelMaxPayload = 1024;
  // Bytes each side may send per channel before it waits for Credit
  const uint32_t endpointTunnelWindow = 65536;

  struct DesktopEndpoint {
    DesktopEndpoint() : flags(0) {}

    // An endpoint with an empty URL is withdrawn
    bool withdrawn() const { return url.empty(); }

    std::string service;
    uint8_t flags;
    std::string url;
    std::vector<uint8_t> token;
  };

}

#endif
