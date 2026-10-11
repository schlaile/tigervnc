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

// Devices of this computer for applications of the remote desktop
// (doc/device-link.md). The server requests a device by name; the user
// decides which local serial port, network address or file answers it.
// Each open device has a thread that reads and one that writes; the
// FLTK main thread exchanges the DeviceLink messages with the server and
// takes what the threads have done with a timer.

#ifndef __DEVICELINK_H__
#define __DEVICELINK_H__

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "DeviceLinkPort.h"

class CConn;

class DeviceLink {
public:
  DeviceLink(CConn* cc);
  ~DeviceLink();

  // Whether device links are to be offered to server, as the user
  // typed it: only with a device profile (DeviceLinkProfile) for it
  static bool wanted(const char* server);

  // Main thread: a DeviceLink message from the server
  void handleMessage(uint8_t subtype, uint16_t channel,
                     const uint8_t* data, size_t len);

private:
  struct Channel;

  void handleRequest(uint16_t id, const uint8_t* data, size_t len);
  void handleClose(Channel* channel);
  void queueOp(Channel* channel, uint8_t subtype, const uint8_t* data,
               size_t len);

  // Main thread, from a timer: the user decides about requests
  enum Decision { Yes, No, NeedsUser };
  static void handleQuestions(void* data);
  Decision decide(Channel* channel, DeviceLinkTarget* target,
                  std::string* message, uint8_t* status);
  Decision askUser(Channel* channel, DeviceLinkTarget* target,
                   std::string* message, uint8_t* status);
  void closeDialog(uint16_t id);
  void acquire(Channel* channel, const DeviceLinkTarget& target);
  void start(Channel* channel, const DeviceLinkTarget& target);

  // Main thread, from a timer: what the threads did
  static void handlePoll(void* data);
  void poll();

  void stop(Channel* channel);
  void erase(Channel* channel);

  void send(uint8_t subtype, uint16_t channel, const uint8_t* data,
            size_t len);
  void result(uint16_t channel, uint8_t status, const std::string& message);

  static void readLoop(Channel* channel);
  static void writeLoop(Channel* channel);

  CConn* cc;
  bool helloReceived;
  unsigned maxChannels;
  std::map<uint16_t, Channel*> channels;
  std::list<uint16_t> questions;
};

#endif
