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

//
// DeviceLinkHost - the desktop side of device links (doc/device-link.md)
//
// Applications of the desktop request devices on a control socket. A
// serial device appears as a pseudo terminal, a tcp or tail device as
// a Unix socket. The bytes and settings go to the viewer that owns the
// device links as DeviceLink messages.
//

#ifndef __DEVICELINKHOST_H__
#define __DEVICELINKHOST_H__

#include <stddef.h>
#include <stdint.h>

#include <list>
#include <map>
#include <string>

namespace rfb { class VNCServer; }

class DeviceLinkHost {
public:
  // The event loop of the server, which watches file descriptors
  class Watcher {
  public:
    virtual ~Watcher() {}
    virtual void watchFd(int fd, bool read, bool write) = 0;
    virtual void unwatchFd(int fd) = 0;
  };

  // Creates the directory dir with the control socket in it
  DeviceLinkHost(Watcher* watcher, rfb::VNCServer* server,
                 const char* dir, unsigned maxChannels);
  ~DeviceLinkHost();

  // handleFdEvent() returns false if fd is not one of ours
  bool handleFdEvent(int fd, bool read, bool write);

  // From the VNC server
  void ownerChanged(bool present);
  void handleMessage(uint8_t subtype, uint16_t channel,
                     const uint8_t* data, size_t len);

  // The default directory for display, under $XDG_RUNTIME_DIR
  static std::string defaultDir(const char* display);

private:
  struct Control;
  struct Device;

  // Control socket
  void acceptControl();
  void handleControl(Control* control, bool read, bool write);
  void handleCommand(Control* control, const std::string& line);
  void reply(Control* control, const std::string& line);
  void broadcast(const std::string& line);
  void closeControl(Control* control);

  // Commands
  // wait: -1 no, 0 until released, else seconds
  void request(Control* control, const std::string& name,
               const std::string& kind, const std::string& label,
               int wait);
  // ack: answer "ok NAME" on control once the viewer has closed it
  void release(Control* control, const std::string& name, bool ack);
  Device* serialFor(Control* control, const std::string& name);

  // Devices
  bool openEndpoint(Device* device);
  void closeEndpoint(Device* device);
  void handleDevice(Device* device, int fd, bool read, bool write);
  void readPty(Device* device);
  void readConnection(Device* device);
  void forwardFromApp(Device* device, const uint8_t* data, size_t len);
  void writeToApp(Device* device);
  void termiosChanged(Device* device);
  void updateWatches(Device* device);
  void lose(Device* device, const std::string& event,
            const std::string& detail);
  void finish(Device* device);

  // Messages from the viewer
  void handleRequestResult(Device* device, const uint8_t* data,
                           size_t len);
  void handleData(Device* device, const uint8_t* data, size_t len);
  void handleCredit(Device* device, const uint8_t* data, size_t len);
  void handleLineState(Device* device, const uint8_t* data, size_t len);
  void handleClose(Device* device);

  void send(uint8_t subtype, uint16_t channel,
            const uint8_t* data, size_t len);
  void sendParams(Device* device, uint8_t mask);

  Watcher* watcher;
  rfb::VNCServer* server;
  std::string dir;
  unsigned maxChannels;
  bool ownerPresent;

  int listenFd;
  std::list<Control*> controls;

  // Devices by name, and by channel including those being closed
  std::map<std::string, Device*> devices;
  std::map<uint16_t, Device*> channels;
  uint16_t nextChannel;
};

#endif
