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

// The "Devices" page of the options dialog: the device profile of the
// connection, and the profiles themselves (DeviceLinkProfile.h)

#ifndef __DEVICELINKOPTIONS_H__
#define __DEVICELINKOPTIONS_H__

#include <string>
#include <vector>

#include <FL/Fl_Group.H>

#include "DeviceLinkProfile.h"

class Fl_Button;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Hold_Browser;
class Fl_Input;
class Fl_Widget;

class DeviceLinkOptions : public Fl_Group {
public:
  DeviceLinkOptions(int x, int y, int w, int h);

  // From the parameters and the profile file
  void load();
  // To the parameters and, if anything changed, the profile file
  void store();

private:
  DeviceLinkProfile* current();
  void fillChoice();
  void fillProfiles();
  void showProfile();
  void fillDevices();
  void changed();

  static void handleProfileSelect(Fl_Widget*, void* data);
  static void handleProfileEdit(Fl_Widget*, void* data);
  static void handleNewProfile(Fl_Widget*, void* data);
  static void handleRemoveProfile(Fl_Widget*, void* data);
  static void handleDeviceSelect(Fl_Widget*, void* data);
  static void handleAddDevice(Fl_Widget*, void* data);
  static void handleEditDevice(Fl_Widget*, void* data);
  static void handleRemoveDevice(Fl_Widget*, void* data);

  Fl_Choice* connectionChoice;
  Fl_Hold_Browser* profileBrowser;
  Fl_Button* removeProfileButton;
  Fl_Group* profileGroup;
  Fl_Input* descriptionInput;
  Fl_Input* serversInput;
  Fl_Choice* askChoice;
  Fl_Check_Button* insecureCheckbox;
  Fl_Hold_Browser* deviceBrowser;
  Fl_Button* editDeviceButton;
  Fl_Button* removeDeviceButton;

  std::vector<DeviceLinkProfile> profiles;
  std::vector<std::string> header;
  // The profile the connection uses, as the parameter says
  std::string selected;
  bool loaded;
  bool dirty;
  // While showProfile() fills the fields
  bool filling;
};

#endif
