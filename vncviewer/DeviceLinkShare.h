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

// A local device shared by the sessions of the viewers on this computer
// (doc/device-link.md, "Sharing a device between sessions").
//
// The first viewer that needs a serial port or network device opens it
// and keeps it open; it is the host. Other viewers on this computer are
// its guests: they connect to the host on 127.0.0.1 (address and secret
// in the lock file of the device) instead of opening the device again,
// so that the device never sees the change between sessions. The host
// sends what the device says to the session that has locked the device,
// or else to the one whose viewer window has the keyboard focus, and
// passes on what the sessions write, unless another session has the
// lock.
//
// Files that are followed are not shared: every viewer reads them itself.

#ifndef __DEVICELINKSHARE_H__
#define __DEVICELINKSHARE_H__

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "DeviceLinkPort.h"

class DeviceLinkShare {
public:
  // Starts using the device as one participant; the outcome comes with
  // take(). session names this viewer's session for messages.
  static DeviceLinkShare* attach(const DeviceLinkTarget& target,
                                 const std::string& session);
  virtual ~DeviceLinkShare() {}

  // What the device and the other participants did since the last call
  struct Events {
    Events() : opened(false), failed(false), gone(false), written(0),
               lineChanged(false), lines(0), errors(0), lockResult(-1) {}
    bool opened;
    bool failed;        // could not be opened; error says why
    bool gone;          // went away; error says why
    std::string error;
    std::string data;   // from the device, for this participant
    uint32_t written;   // bytes of ours written to the device
    bool lineChanged;
    uint8_t lines, errors;
    int lockResult;     // -1 none, else a RequestResult status
    std::string lockMessage;
  };
  virtual Events take() = 0;

  // To the device; dropped while another session has the lock
  virtual void write(const std::string& data) = 0;
  virtual void setParams(const std::string& payload) = 0;
  virtual void setLines(const std::string& payload) = 0;
  virtual void purge(uint8_t which) = 0;

  // wait: -1 no, 0 until free, else seconds
  virtual void lock(int wait) = 0;
  virtual void unlock() = 0;

  // Whether a window of this viewer has the keyboard focus
  static void setFocus(bool focused);

  // Main thread, often: moves data between the device, the guests and
  // the participants
  static void pollAll();
  static bool active();
};

#endif
