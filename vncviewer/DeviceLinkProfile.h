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

// Device profiles (doc/device-link.md): which devices of this computer
// a remote desktop may use. A connection uses the profile named by
// DeviceLinkProfile; without one, it offers no devices at all.
//
// The profiles are kept in the file "device-links" in the configuration
// directory (DeviceLinksFile), for example:
//
//   [kasse]
//   description  Kasse Laden
//   server       wws.schlaile.de:*
//   ask          once
//   insecure     yes
//   display      serial  COM3   9600 8N1
//   cts          tail    "C:\Program Files\CTS\dummykasse.log"
//
// Words are separated by blanks; double quotes keep a word with blanks
// together (backslashes have no special meaning); # starts a comment.

#ifndef __DEVICELINKPROFILE_H__
#define __DEVICELINKPROFILE_H__

#include <map>
#include <string>
#include <vector>

#include "DeviceLinkPort.h"

struct DeviceLinkProfile {
  DeviceLinkProfile();

  enum Ask { AskAlways, AskOnce, AskNever };

  std::string name;
  std::string description;
  // Patterns with * and ?; empty: any server
  std::vector<std::string> servers;
  Ask ask;
  bool insecure;
  std::map<std::string, DeviceLinkTarget> devices;
  // Device names in the order of the file
  std::vector<std::string> order;

  bool matchesServer(const std::string& server) const;

  // The file with the profiles
  static std::string path();

  // All profiles of the file; problems in single lines are logged and
  // the lines skipped. False only if the file cannot be read.
  static bool loadAll(std::vector<DeviceLinkProfile>* profiles,
                      std::string* error);
  // The profile called name
  static bool load(const std::string& name, DeviceLinkProfile* profile,
                   std::string* error);

  // Adds a device to a profile in the file, creating the file and the
  // profile if needed
  static bool addDevice(const std::string& profile, const std::string& name,
                        const DeviceLinkTarget& target, std::string* error);

  // Words of a line, without the comment
  static std::vector<std::string> split(const std::string& line);
  static std::string quote(const std::string& word);
};

#endif
