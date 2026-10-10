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

// The local end of a device link (doc/device-link.md): a serial port,
// a TCP connection or a file that is followed as it grows. A port is
// used by two worker threads, one reading and one writing; read() and
// write() return within about 100 ms so that the threads can stop.

#ifndef __DEVICELINKPORT_H__
#define __DEVICELINKPORT_H__

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

// What a name stands for, as written in a profile (see
// DeviceLinkProfile.h), split into words:
//
//   serial PORT [SPEED] [FORMAT] [rtscts|xonxoff|noflow] [dtr=0|1]
//               [rts=0|1] [fixed]
//   tcp HOST:PORT
//   tail PATH [from-start]
struct DeviceLinkTarget {
  DeviceLinkTarget();

  // Parses one target; false with *error set if it is not valid
  static bool parse(const std::vector<std::string>& words,
                    DeviceLinkTarget* target, std::string* error);
  std::vector<std::string> toWords() const;
  // For messages
  std::string toString() const;

  uint8_t kind;
  std::string where;

  // Serial: initialisation, 0 or -1 for "leave as it is"
  uint32_t speed;
  uint8_t dataBits, parity, stopBits;
  int flow;
  int dtr, rts;
  bool fixed;

  // Tail: start at the beginning instead of the end
  bool fromStart;
};

class DeviceLinkPort {
public:
  // Opens the target, nullptr with *error set on failure. May block
  // (TCP connect), so it is called from a worker thread.
  static DeviceLinkPort* open(const DeviceLinkTarget& target,
                              std::string* error);
  virtual ~DeviceLinkPort() {}

  // Reads what is there, waiting at most about 100 ms. Returns the
  // number of bytes, 0 if there was nothing, -1 if the device is gone.
  virtual int read(uint8_t* buf, size_t len, std::string* error) = 0;

  // Writes all of buf, unless stop becomes true. Returns false if the
  // device is gone.
  virtual bool write(const uint8_t* buf, size_t len,
                     const std::atomic<bool>& stop,
                     std::string* error) = 0;

  // Serial ports only; the others ignore these. mask: bit 0 speed,
  // bit 1 format, bit 2 flow.
  virtual void setParams(uint8_t /*mask*/, uint32_t /*speed*/,
                         uint8_t /*dataBits*/, uint8_t /*parity*/,
                         uint8_t /*stopBits*/, uint8_t /*flow*/) {}
  virtual void setLines(uint8_t /*mask*/, uint8_t /*values*/) {}
  virtual void sendBreak(unsigned /*ms*/) {}
  virtual void purge(uint8_t /*which*/) {}
  // Modem lines, and errors since the last call; false if the device
  // has no such thing
  virtual bool lineState(uint8_t* /*lines*/, uint8_t* /*errors*/) {
    return false;
  }

  // The serial ports of this computer, for the user to choose from
  static std::vector<std::string> serialPorts();
};

#endif
