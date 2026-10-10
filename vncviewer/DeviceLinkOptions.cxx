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

#include <algorithm>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Input_Choice.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <core/Timer.h>
#include <core/i18n.h>
#include <core/string.h>

#include <rfb/DeviceLink.h>

#include "DeviceLinkOptions.h"
#include "fltk/layout.h"
#include "fltk/util.h"
#include "parameters.h"

using namespace rfb;

static const char* kindLabel(uint8_t kind)
{
  switch (kind) {
  case deviceLinkSerial:
    return _("Serial");
  case deviceLinkTCP:
    return _("TCP");
  case deviceLinkTail:
    return _("File");
  }
  return "?";
}

static bool validName(const std::string& name)
{
  if (name.empty() || name.size() > deviceLinkMaxName)
    return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-'))
      return false;
  }
  return true;
}

// A modal dialog in which the connection goes on
static void runModal(Fl_Window* win)
{
  win->set_modal();
  win->show();
  while (win->shown()) {
    int next = core::Timer::checkTimeouts();
    if (next < 0 || next > 100)
      next = 100;
    Fl::wait(next / 1000.0);
  }
}

// -=- The dialog for one device

namespace {
  struct DeviceDialog {
    Fl_Window* win;
    Fl_Input* name;
    Fl_Choice* kind;
    Fl_Input_Choice* where;
    Fl_Button* browse;
    Fl_Group* serial;
    Fl_Input_Choice* speed;
    Fl_Choice* format;
    Fl_Choice* flow;
    Fl_Choice* dtr;
    Fl_Choice* rts;
    Fl_Check_Button* fixed;
    Fl_Check_Button* fromStart;
    Fl_Input* comment;
    bool ok;
  };
}

static const char* formats[] = { "8N1", "7E1", "7O1", "8E1", "8O1", "8N2",
                                 "7E2", "7N1" };

static void deviceKindChanged(Fl_Widget*, void* data)
{
  DeviceDialog* d = (DeviceDialog*)data;
  int kind = d->kind->value();

  d->where->clear();
  if (kind == 0) {
    for (const std::string& port : DeviceLinkPort::serialPorts())
      fltk_menu_add(d->where->menubutton(), port.c_str(), 0, nullptr);
    d->where->menubutton()->activate();
    d->serial->show();
  } else {
    d->where->menubutton()->deactivate();
    d->serial->hide();
  }
  if (kind == 2) {
    d->browse->show();
    d->fromStart->show();
  } else {
    d->browse->hide();
    d->fromStart->hide();
  }
  d->where->label(kind == 0 ? _("Port:") :
                  kind == 1 ? _("Host:port:") : _("File:"));
  d->win->redraw();
}

static void deviceBrowse(Fl_Widget*, void* data)
{
  DeviceDialog* d = (DeviceDialog*)data;
  Fl_Native_File_Chooser chooser;
  chooser.title(_("File to follow"));
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  if (chooser.show() == 0 && chooser.filename())
    d->where->value(chooser.filename());
}

static void deviceOK(Fl_Widget*, void* data)
{
  DeviceDialog* d = (DeviceDialog*)data;
  d->ok = true;
  d->win->hide();
}

static void deviceCancel(Fl_Widget*, void* data)
{
  ((DeviceDialog*)data)->win->hide();
}

static bool editDevice(const char* title, std::string* name,
                       DeviceLinkTarget* target, std::string* comment,
                       const DeviceLinkProfile& profile)
{
  const int W = 440, lx = 120;
  DeviceDialog d;
  int y = OUTER_MARGIN;

  Fl_Window win(W, 360, title);
  d.win = &win;

  d.name = new Fl_Input(lx, y, 150, INPUT_HEIGHT, _("Name:"));
  d.name->tooltip(_("The name the remote desktop asks for: lower case "
                    "letters, digits, - and _"));
  y += INPUT_HEIGHT + TIGHT_MARGIN;

  d.kind = new Fl_Choice(lx, y, 150, CHOICE_HEIGHT, _("Kind:"));
  fltk_menu_add(d.kind, _("Serial port"), 0, nullptr);
  fltk_menu_add(d.kind, _("Network device (TCP)"), 0, nullptr);
  fltk_menu_add(d.kind, _("File that grows (tail)"), 0, nullptr);
  fltk_adjust_choice(d.kind);
  y += CHOICE_HEIGHT + TIGHT_MARGIN;

  d.where = new Fl_Input_Choice(lx, y, W - lx - OUTER_MARGIN - 45,
                                INPUT_HEIGHT, _("Port:"));
  d.browse = new Fl_Button(W - OUTER_MARGIN - 40, y, 40, INPUT_HEIGHT,
                           "...");
  y += INPUT_HEIGHT + INNER_MARGIN;

  int groupY = y;
  d.serial = new Fl_Group(0, y, W, 4 * (CHOICE_HEIGHT + TIGHT_MARGIN) +
                                   CHECK_HEIGHT);
  d.speed = new Fl_Input_Choice(lx, y, 150, INPUT_HEIGHT, _("Speed:"));
  fltk_menu_add(d.speed->menubutton(), _("as it is"), 0, nullptr);
  for (const char* s : { "1200", "2400", "4800", "9600", "19200", "38400",
                         "57600", "115200" })
    fltk_menu_add(d.speed->menubutton(), s, 0, nullptr);
  y += INPUT_HEIGHT + TIGHT_MARGIN;
  d.format = new Fl_Choice(lx, y, 150, CHOICE_HEIGHT, _("Format:"));
  fltk_menu_add(d.format, _("as it is"), 0, nullptr);
  for (const char* f : formats)
    fltk_menu_add(d.format, f, 0, nullptr);
  y += CHOICE_HEIGHT + TIGHT_MARGIN;
  d.flow = new Fl_Choice(lx, y, 150, CHOICE_HEIGHT, _("Flow control:"));
  fltk_menu_add(d.flow, _("as it is"), 0, nullptr);
  fltk_menu_add(d.flow, _("none"), 0, nullptr);
  fltk_menu_add(d.flow, "RTS/CTS", 0, nullptr);
  fltk_menu_add(d.flow, "XON/XOFF", 0, nullptr);
  y += CHOICE_HEIGHT + TIGHT_MARGIN;
  d.dtr = new Fl_Choice(lx, y, 100, CHOICE_HEIGHT, "DTR:");
  d.rts = new Fl_Choice(lx + 100 + 50, y, 100, CHOICE_HEIGHT, "RTS:");
  for (Fl_Choice* c : { d.dtr, d.rts }) {
    fltk_menu_add(c, _("as it is"), 0, nullptr);
    fltk_menu_add(c, _("on"), 0, nullptr);
    fltk_menu_add(c, _("off"), 0, nullptr);
  }
  y += CHOICE_HEIGHT + TIGHT_MARGIN;
  d.fixed = new Fl_Check_Button(LBLRIGHT(lx, y, CHECK_MIN_WIDTH,
                                         CHECK_HEIGHT,
                                         _("Keep these settings")));
  d.fixed->tooltip(_("The remote desktop may not change speed, format, "
                     "flow control, DTR and RTS"));
  d.serial->end();

  d.fromStart = new Fl_Check_Button(LBLRIGHT(lx, groupY, CHECK_MIN_WIDTH,
                                             CHECK_HEIGHT,
                                             _("Read the file from its "
                                               "beginning")));

  y = groupY + d.serial->h() + INNER_MARGIN;
  d.comment = new Fl_Input(lx, y, W - lx - OUTER_MARGIN, INPUT_HEIGHT,
                           _("Comment:"));
  y += INPUT_HEIGHT + INNER_MARGIN;

  Fl_Button cancel(W - OUTER_MARGIN - 2 * BUTTON_WIDTH - INNER_MARGIN, y,
                   BUTTON_WIDTH, BUTTON_HEIGHT, _("Cancel"));
  Fl_Return_Button ok(W - OUTER_MARGIN - BUTTON_WIDTH, y, BUTTON_WIDTH,
                      BUTTON_HEIGHT, _("OK"));
  win.size(W, y + BUTTON_HEIGHT + OUTER_MARGIN);
  win.end();

  d.ok = false;
  d.kind->callback(deviceKindChanged, &d);
  d.browse->callback(deviceBrowse, &d);
  ok.callback(deviceOK, &d);
  cancel.callback(deviceCancel, &d);
  win.callback(deviceCancel, &d);

  // Fill in the device
  d.name->value(name->c_str());
  d.kind->value(target->kind == deviceLinkTCP ? 1 :
                target->kind == deviceLinkTail ? 2 : 0);
  deviceKindChanged(nullptr, &d);
  d.where->value(target->where.c_str());
  d.speed->value(target->speed ? std::to_string(target->speed).c_str() :
                                 _("as it is"));
  d.format->value(0);
  if (target->dataBits) {
    std::string f(std::to_string(target->dataBits) + "NOEMS"[target->parity] +
                  (target->stopBits == deviceLinkStopBits2 ? "2" : "1"));
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
      if (f == formats[i])
        d.format->value(i + 1);
    }
  }
  d.flow->value(target->flow < 0 ? 0 : target->flow + 1);
  d.dtr->value(target->dtr < 0 ? 0 : target->dtr ? 1 : 2);
  d.rts->value(target->rts < 0 ? 0 : target->rts ? 1 : 2);
  d.fixed->value(target->fixed);
  d.fromStart->value(target->fromStart);
  d.comment->value(comment->c_str());

  std::string oldName(*name);
  while (true) {
    d.ok = false;
    runModal(&win);
    if (!d.ok)
      return false;

    std::string newName(d.name->value());
    if (!validName(newName)) {
      fl_alert(_("The name may only contain lower case letters, digits, "
                 "- and _ (at most %d characters)."), (int)deviceLinkMaxName);
      continue;
    }
    if (newName != oldName && profile.devices.count(newName)) {
      fl_alert(_("There is already a device \"%s\" in this profile."),
               newName.c_str());
      continue;
    }

    std::vector<std::string> words;
    int kind = d.kind->value();
    words.push_back(kind == 0 ? "serial" : kind == 1 ? "tcp" : "tail");
    words.push_back(d.where->value() ? d.where->value() : "");
    if (words.back().empty()) {
      fl_alert(_("Please give the device."));
      continue;
    }
    if (kind == 0) {
      std::string speed(d.speed->value() ? d.speed->value() : "");
      if (!speed.empty() && speed != _("as it is"))
        words.push_back(speed);
      if (d.format->value() > 0)
        words.push_back(formats[d.format->value() - 1]);
      static const char* flows[] = { "noflow", "rtscts", "xonxoff" };
      if (d.flow->value() > 0)
        words.push_back(flows[d.flow->value() - 1]);
      if (d.dtr->value() > 0)
        words.push_back(d.dtr->value() == 1 ? "dtr=1" : "dtr=0");
      if (d.rts->value() > 0)
        words.push_back(d.rts->value() == 1 ? "rts=1" : "rts=0");
      if (d.fixed->value())
        words.push_back("fixed");
    } else if (kind == 2 && d.fromStart->value()) {
      words.push_back("from-start");
    }

    DeviceLinkTarget parsed;
    std::string error;
    if (!DeviceLinkTarget::parse(words, &parsed, &error)) {
      fl_alert("%s", error.c_str());
      continue;
    }

    *name = newName;
    *target = parsed;
    *comment = d.comment->value();
    return true;
  }
}

// -=- The page

DeviceLinkOptions::DeviceLinkOptions(int x, int y, int w, int h)
  : Fl_Group(x, y, w, h, _("Devices")), loaded(false), dirty(false),
    filling(false)
{
  int tx = x + OUTER_MARGIN, ty = y + OUTER_MARGIN;
  int width = w - 2 * OUTER_MARGIN;

  connectionChoice = new Fl_Choice(LBLLEFT(tx, ty, width, CHOICE_HEIGHT,
                                           _("Profile for this connection:")));
  connectionChoice->tooltip(_("Which devices of this computer the remote "
                              "desktop may use. Takes effect when "
                              "connecting."));
  ty += CHOICE_HEIGHT + INNER_MARGIN + TIGHT_MARGIN;

  // Profiles on the left
  const int listWidth = 120;
  int bottom = y + h - OUTER_MARGIN;
  int listTop = ty + INPUT_LABEL_OFFSET;
  profileBrowser = new Fl_Hold_Browser(tx, listTop, listWidth,
                                       bottom - BUTTON_HEIGHT - TIGHT_MARGIN -
                                       listTop,
                                       _("Profiles"));
  profileBrowser->align(FL_ALIGN_TOP_LEFT);
  profileBrowser->format_char(0);
  profileBrowser->callback(handleProfileSelect, this);

  int half = (listWidth - TIGHT_MARGIN) / 2;
  Fl_Button* newButton = new Fl_Button(tx, bottom - BUTTON_HEIGHT, half,
                                       BUTTON_HEIGHT, _("New"));
  newButton->callback(handleNewProfile, this);
  removeProfileButton = new Fl_Button(tx + half + TIGHT_MARGIN,
                                      bottom - BUTTON_HEIGHT, half,
                                      BUTTON_HEIGHT, _("Remove"));
  removeProfileButton->callback(handleRemoveProfile, this);

  // The selected profile on the right
  int px = tx + listWidth + INNER_MARGIN;
  int pw = tx + width - px;
  profileGroup = new Fl_Group(px, ty, pw, bottom - ty);

  const char* labels[] = { _("Description:"), _("Servers:"),
                           _("Confirm:") };
  int labelWidth = 0;
  for (const char* l : labels)
    labelWidth = std::max(labelWidth, gui_str_len(l) + TEXT_PADDING);

  int py = listTop;
  descriptionInput = new Fl_Input(px + labelWidth, py, pw - labelWidth,
                                  INPUT_HEIGHT, labels[0]);
  descriptionInput->when(FL_WHEN_CHANGED);
  descriptionInput->callback(handleProfileEdit, this);
  py += INPUT_HEIGHT + TIGHT_MARGIN;

  serversInput = new Fl_Input(px + labelWidth, py, pw - labelWidth,
                              INPUT_HEIGHT, labels[1]);
  serversInput->tooltip(_("Only for these servers, as given to the viewer; "
                          "patterns with * and ?, separated by blanks. "
                          "Empty: any server."));
  serversInput->when(FL_WHEN_CHANGED);
  serversInput->callback(handleProfileEdit, this);
  py += INPUT_HEIGHT + TIGHT_MARGIN;

  askChoice = new Fl_Choice(px + labelWidth, py, pw - labelWidth,
                            CHOICE_HEIGHT, labels[2]);
  fltk_menu_add(askChoice, _("every time"), 0, nullptr);
  fltk_menu_add(askChoice, _("once per start"), 0, nullptr);
  fltk_menu_add(askChoice, _("never"), 0, nullptr);
  askChoice->callback(handleProfileEdit, this);
  py += CHOICE_HEIGHT + TIGHT_MARGIN;

  insecureCheckbox = new Fl_Check_Button(LBLRIGHT(px, py, CHECK_MIN_WIDTH,
                                                  CHECK_HEIGHT,
                                                  _("Also without "
                                                    "encryption (VPN)")));
  insecureCheckbox->callback(handleProfileEdit, this);
  py += CHECK_HEIGHT + INNER_MARGIN + INPUT_LABEL_OFFSET;

  deviceBrowser = new Fl_Hold_Browser(px, py, pw,
                                      bottom - BUTTON_HEIGHT - TIGHT_MARGIN -
                                      py, _("Devices"));
  deviceBrowser->align(FL_ALIGN_TOP_LEFT);
  deviceBrowser->format_char(0);
  static int columns[] = { 80, 50, 0 };
  deviceBrowser->column_widths(columns);
  deviceBrowser->column_char('\t');
  deviceBrowser->callback(handleDeviceSelect, this);

  int third = (pw - 2 * TIGHT_MARGIN) / 3;
  Fl_Button* addButton = new Fl_Button(px, bottom - BUTTON_HEIGHT, third,
                                       BUTTON_HEIGHT, _("Add..."));
  addButton->callback(handleAddDevice, this);
  editDeviceButton = new Fl_Button(px + third + TIGHT_MARGIN,
                                   bottom - BUTTON_HEIGHT, third,
                                   BUTTON_HEIGHT, _("Edit..."));
  editDeviceButton->callback(handleEditDevice, this);
  removeDeviceButton = new Fl_Button(px + 2 * (third + TIGHT_MARGIN),
                                     bottom - BUTTON_HEIGHT, third,
                                     BUTTON_HEIGHT, _("Remove"));
  removeDeviceButton->callback(handleRemoveDevice, this);

  profileGroup->end();

  end();
}

void DeviceLinkOptions::load()
{
  std::string error;

  profiles.clear();
  header.clear();
  // A missing file simply means no profiles yet
  DeviceLinkProfile::loadAll(&profiles, &error, &header);
  selected = (const char*)deviceLinkProfile;
  loaded = true;
  dirty = false;

  fillProfiles();
  fillChoice();
  profileBrowser->value(profiles.empty() ? 0 : 1);
  for (size_t i = 0; i < profiles.size(); i++) {
    if (profiles[i].name == selected)
      profileBrowser->value(i + 1);
  }
  showProfile();
}

void DeviceLinkOptions::store()
{
  if (!loaded)
    return;

  int choice = connectionChoice->value();
  if (choice <= 0)
    selected = "";
  else if ((size_t)choice <= profiles.size())
    selected = profiles[choice - 1].name;
  deviceLinkProfile.setParam(selected.c_str());

  if (dirty) {
    std::string error;
    if (!DeviceLinkProfile::saveAll(profiles, header, &error))
      fl_alert(_("The device profiles could not be saved:\n\n%s"),
               error.c_str());
    dirty = false;
  }
}

DeviceLinkProfile* DeviceLinkOptions::current()
{
  int i = profileBrowser->value();
  if (i <= 0 || (size_t)i > profiles.size())
    return nullptr;
  return &profiles[i - 1];
}

void DeviceLinkOptions::fillChoice()
{
  std::string keep(selected);
  int value = 0;

  connectionChoice->clear();
  fltk_menu_add(connectionChoice, _("(none)"), 0, nullptr);
  for (size_t i = 0; i < profiles.size(); i++) {
    std::string label(profiles[i].name);
    if (!profiles[i].description.empty())
      label += " - " + profiles[i].description;
    fltk_menu_add(connectionChoice, label.c_str(), 0, nullptr);
    if (profiles[i].name == keep)
      value = i + 1;
  }
  connectionChoice->value(value);
}

void DeviceLinkOptions::fillProfiles()
{
  int keep = profileBrowser->value();

  profileBrowser->clear();
  for (const DeviceLinkProfile& p : profiles)
    profileBrowser->add(p.name.c_str());
  if (keep > profileBrowser->size())
    keep = profileBrowser->size();
  profileBrowser->value(keep);
}

void DeviceLinkOptions::showProfile()
{
  DeviceLinkProfile* p = current();

  filling = true;
  if (p == nullptr) {
    descriptionInput->value("");
    serversInput->value("");
    askChoice->value(1);
    insecureCheckbox->value(0);
    profileGroup->deactivate();
    removeProfileButton->deactivate();
  } else {
    descriptionInput->value(p->description.c_str());
    std::string servers;
    for (const std::string& s : p->servers)
      servers += (servers.empty() ? "" : " ") + s;
    serversInput->value(servers.c_str());
    askChoice->value(p->ask);
    insecureCheckbox->value(p->insecure);
    profileGroup->activate();
    removeProfileButton->activate();
  }
  filling = false;
  fillDevices();
}

void DeviceLinkOptions::fillDevices()
{
  DeviceLinkProfile* p = current();
  int keep = deviceBrowser->value();

  deviceBrowser->clear();
  if (p != nullptr) {
    for (const std::string& name : p->order) {
      const DeviceLinkTarget& t = p->devices[name];
      std::vector<std::string> words(t.toWords());
      std::string what;
      for (size_t i = 1; i < words.size(); i++)
        what += (i > 1 ? " " : "") + words[i];
      deviceBrowser->add((name + "\t" + kindLabel(t.kind) + "\t" +
                          what).c_str());
    }
  }
  if (keep > deviceBrowser->size())
    keep = deviceBrowser->size();
  deviceBrowser->value(keep);

  if (deviceBrowser->value() > 0) {
    editDeviceButton->activate();
    removeDeviceButton->activate();
  } else {
    editDeviceButton->deactivate();
    removeDeviceButton->deactivate();
  }
}

void DeviceLinkOptions::changed()
{
  DeviceLinkProfile* p = current();
  if (p != nullptr)
    p->modified = true;
  dirty = true;
}

void DeviceLinkOptions::handleProfileSelect(Fl_Widget*, void* data)
{
  ((DeviceLinkOptions*)data)->showProfile();
}

void DeviceLinkOptions::handleProfileEdit(Fl_Widget*, void* data)
{
  DeviceLinkOptions* self = (DeviceLinkOptions*)data;
  DeviceLinkProfile* p = self->current();

  if (self->filling || p == nullptr)
    return;

  p->description = self->descriptionInput->value();
  p->servers = DeviceLinkProfile::split(self->serversInput->value());
  p->ask = (DeviceLinkProfile::Ask)self->askChoice->value();
  p->insecure = self->insecureCheckbox->value();
  self->changed();

  // The description shows in the choice of the connection
  int choice = self->connectionChoice->value();
  if (choice > 0 && (size_t)choice <= self->profiles.size())
    self->selected = self->profiles[choice - 1].name;
  self->fillChoice();
}

void DeviceLinkOptions::handleNewProfile(Fl_Widget*, void* data)
{
  DeviceLinkOptions* self = (DeviceLinkOptions*)data;
  const char* answer = fl_input(_("Name of the new device profile:"), "");

  if (answer == nullptr)
    return;
  std::string name(answer);
  name.erase(0, name.find_first_not_of(" \t"));
  name.erase(name.find_last_not_of(" \t") + 1);
  if (name.empty() || name.find_first_of("[]#\"") != std::string::npos) {
    fl_alert(_("Please give a name without [ ] # and \"."));
    return;
  }
  for (const DeviceLinkProfile& p : self->profiles) {
    if (p.name == name) {
      fl_alert(_("There is already a profile \"%s\"."), name.c_str());
      return;
    }
  }

  // The connection choice keeps its selection by name
  int choice = self->connectionChoice->value();
  if (choice > 0 && (size_t)choice <= self->profiles.size())
    self->selected = self->profiles[choice - 1].name;
  else
    self->selected = "";

  DeviceLinkProfile profile;
  profile.name = name;
  profile.modified = true;
  self->profiles.push_back(profile);
  self->dirty = true;
  self->fillProfiles();
  self->profileBrowser->value(self->profiles.size());
  self->fillChoice();
  self->showProfile();
}

void DeviceLinkOptions::handleRemoveProfile(Fl_Widget*, void* data)
{
  DeviceLinkOptions* self = (DeviceLinkOptions*)data;
  DeviceLinkProfile* p = self->current();

  if (p == nullptr)
    return;
  if (fl_choice(_("Remove the device profile \"%s\" with all its "
                  "devices?"), _("Cancel"), _("Remove"), nullptr,
                p->name.c_str()) != 1)
    return;

  int choice = self->connectionChoice->value();
  if (choice > 0 && (size_t)choice <= self->profiles.size())
    self->selected = self->profiles[choice - 1].name;
  if (self->selected == p->name)
    self->selected = "";

  self->profiles.erase(self->profiles.begin() +
                       (self->profileBrowser->value() - 1));
  self->dirty = true;
  self->fillProfiles();
  self->fillChoice();
  self->showProfile();
}

void DeviceLinkOptions::handleDeviceSelect(Fl_Widget*, void* data)
{
  DeviceLinkOptions* self = (DeviceLinkOptions*)data;

  self->fillDevices();
  // Double click edits
  if (Fl::event_clicks() && self->deviceBrowser->value() > 0)
    handleEditDevice(nullptr, data);
}

void DeviceLinkOptions::handleAddDevice(Fl_Widget*, void* data)
{
  DeviceLinkOptions* self = (DeviceLinkOptions*)data;
  DeviceLinkProfile* p = self->current();

  if (p == nullptr)
    return;

  std::string name, comment;
  DeviceLinkTarget target;
  target.kind = deviceLinkSerial;
  if (!editDevice(_("New device"), &name, &target, &comment, *p))
    return;

  p->order.push_back(name);
  p->devices[name] = target;
  p->comments[name] = comment;
  self->changed();
  self->fillDevices();
  self->deviceBrowser->value(p->order.size());
  self->fillDevices();
}

void DeviceLinkOptions::handleEditDevice(Fl_Widget*, void* data)
{
  DeviceLinkOptions* self = (DeviceLinkOptions*)data;
  DeviceLinkProfile* p = self->current();
  int i = self->deviceBrowser->value();

  if (p == nullptr || i <= 0 || (size_t)i > p->order.size())
    return;

  std::string oldName(p->order[i - 1]), name(oldName);
  DeviceLinkTarget target(p->devices[name]);
  std::string comment(p->comments[name]);
  if (!editDevice(_("Device"), &name, &target, &comment, *p))
    return;

  p->devices.erase(oldName);
  p->comments.erase(oldName);
  p->order[i - 1] = name;
  p->devices[name] = target;
  p->comments[name] = comment;
  self->changed();
  self->fillDevices();
}

void DeviceLinkOptions::handleRemoveDevice(Fl_Widget*, void* data)
{
  DeviceLinkOptions* self = (DeviceLinkOptions*)data;
  DeviceLinkProfile* p = self->current();
  int i = self->deviceBrowser->value();

  if (p == nullptr || i <= 0 || (size_t)i > p->order.size())
    return;

  std::string name(p->order[i - 1]);
  p->devices.erase(name);
  p->comments.erase(name);
  p->order.erase(p->order.begin() + (i - 1));
  self->changed();
  self->fillDevices();
}
