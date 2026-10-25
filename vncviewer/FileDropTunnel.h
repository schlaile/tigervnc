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

// The client side of tunnels through RFB to a side-channel endpoint
// (doc/file-drop.md). It listens on a loopback port; every connection
// there becomes a channel, which the server connects to the endpoint.
// The HTTP client connects to that port instead of the endpoint's host
// and speaks to the endpoint as usual (TLS end to end). Main thread only.

#ifndef __FILEDROPTUNNEL_H__
#define __FILEDROPTUNNEL_H__

#include <stdint.h>

#include <FL/Fl.H>

#include <map>
#include <string>
#include <vector>

namespace network { class Socket; class SocketListener; }

class CConn;

class FileDropTunnel {
public:
  FileDropTunnel(CConn* cc, const std::string& service);
  ~FileDropTunnel();

  // The loopback port, 0 if listening failed
  int port() const { return localPort; }

  // An EndpointTunnel message from the server
  void handleMessage(uint8_t subtype, uint16_t channel,
                     const uint8_t* data, size_t len);

private:
  struct Channel {
    Channel();

    network::Socket* sock;
    bool open;
    // bytes from the server, not yet written to the local connection
    std::vector<uint8_t> toLocal;
    // bytes we may still send to the server
    uint32_t sendCredit;
    // we sent Close (started it, or answered the server's)
    bool closeSent;
    // the server sent Close; dropped once toLocal is out
    bool closeReceived;
    int watched;                // FLTK event mask we registered
  };

  static void acceptEvent(FL_SOCKET fd, void* data);
  static void channelEvent(FL_SOCKET fd, void* data);
  static void retryTimeout(void* data);

  void accept();
  void readLocal(uint16_t id, Channel* ch);
  void writeLocal(uint16_t id, Channel* ch);
  void sendClose(uint16_t id, Channel* ch, uint8_t reason);
  void drop(uint16_t id);
  void updateWatch(Channel* ch);
  void updateAll();
  void send(uint8_t subtype, uint16_t id, const uint8_t* data, size_t len);

  CConn* cc;
  std::string service;
  network::SocketListener* listener;
  int localPort;
  std::map<uint16_t, Channel> channels;
  uint16_t nextId;
  bool retrying;
};

#endif
