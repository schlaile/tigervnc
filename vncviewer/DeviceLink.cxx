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

#include <chrono>
#include <set>
#include <algorithm>
#include <sstream>
#include <vector>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input_Choice.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <core/LogWriter.h>
#include <core/Timer.h>
#include <core/i18n.h>
#include <core/string.h>

#include <rfb/DeviceLink.h>

#include "CConn.h"
#include "DeviceLink.h"
#include "parameters.h"

using namespace rfb;

static core::LogWriter vlog("DeviceLink");

static const double pollInterval = 0.02;

// Instances that still exist, for callbacks that run after a dialog
static std::set<DeviceLink*> instances;

// DeviceLinkOpen=Once: names the user allowed in this run
static std::set<std::string> confirmed;

struct DeviceLink::Channel {
  Channel() : kind(0), state(Asking), fixed(false), stopping(false),
              port(nullptr), openDone(false), gone(false),
              window(deviceLinkWindow), written(0), linesKnown(false),
              lines(0), errors(0), lineChanged(false) {}

  uint16_t id;
  uint8_t kind;
  std::string name;
  std::string label;
  enum { Asking, Opening, Open, Closing } state;
  bool fixed;
  DeviceLinkTarget target;

  std::thread reader, writer;
  std::mutex lock;
  std::condition_variable cond;
  std::atomic<bool> stopping;

  // The rest is shared with the threads, under lock
  DeviceLinkPort* port;
  bool openDone;
  std::string openError;
  bool gone;
  std::string goneError;

  // From the device, to be sent; and what the server lets us send
  std::string fromDevice;
  uint32_t window;
  // Written to the device since the last Credit
  uint32_t written;

  bool linesKnown;
  uint8_t lines, errors;
  bool lineChanged;

  struct Op {
    uint8_t subtype;
    std::string data;
  };
  std::deque<Op> ops;
};

static const char* kindName(uint8_t kind)
{
  switch (kind) {
  case deviceLinkSerial:
    return _("serial port");
  case deviceLinkTCP:
    return _("network device");
  case deviceLinkTail:
    return _("file");
  }
  return "?";
}

// "name=target;name=target"
static std::map<std::string, DeviceLinkTarget> configuredTargets()
{
  std::map<std::string, DeviceLinkTarget> targets;
  std::string value(deviceLinks);
  std::istringstream in(value);
  std::string entry;

  while (std::getline(in, entry, ';')) {
    size_t start = entry.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    entry = entry.substr(start);
    size_t eq = entry.find('=');
    if (eq == std::string::npos) {
      vlog.error(_("Ignoring device \"%s\": expected NAME=TARGET"),
                 entry.c_str());
      continue;
    }
    DeviceLinkTarget target;
    std::string error;
    if (!DeviceLinkTarget::parse(entry.substr(eq + 1), &target, &error)) {
      vlog.error(_("Ignoring device \"%s\": %s"),
                 entry.substr(0, eq).c_str(), error.c_str());
      continue;
    }
    targets[entry.substr(0, eq)] = target;
  }
  return targets;
}

DeviceLink::DeviceLink(CConn* cc_)
  : cc(cc_), helloReceived(false), maxChannels(0)
{
  instances.insert(this);
}

DeviceLink::~DeviceLink()
{
  closeDialog(0);
  instances.erase(this);
  Fl::remove_timeout(handlePoll, this);
  Fl::remove_timeout(handleQuestions, this);
  while (!channels.empty()) {
    Channel* channel = channels.begin()->second;
    stop(channel);
    erase(channel);
  }
}

bool DeviceLink::wanted()
{
  return !(::deviceLinkOpen == "Never" &&
           std::string(deviceLinks).find('=') == std::string::npos);
}

void DeviceLink::handleMessage(uint8_t subtype, uint16_t id,
                               const uint8_t* data, size_t len)
{
  if (subtype == deviceLinkHello) {
    if (len < 4 || data[0] < 1)
      return;
    helloReceived = true;
    maxChannels = (data[2] << 8) | data[3];
    vlog.info(_("The server offers device links"));
    return;
  }

  if (!helloReceived)
    return;

  if (subtype == deviceLinkRequest) {
    handleRequest(id, data, len);
    return;
  }

  auto it = channels.find(id);
  if (it == channels.end())
    return;
  Channel* channel = it->second;

  switch (subtype) {
  case deviceLinkData:
  case deviceLinkSetParams:
  case deviceLinkSetLines:
  case deviceLinkPurge:
    if (channel->state == Channel::Open)
      queueOp(channel, subtype, data, len);
    break;
  case deviceLinkCredit:
    if (len >= 4) {
      uint32_t credit = ((uint32_t)data[0] << 24) | (data[1] << 16) |
                        (data[2] << 8) | data[3];
      std::lock_guard<std::mutex> l(channel->lock);
      channel->window = std::min<uint64_t>((uint64_t)channel->window + credit,
                                           deviceLinkWindow);
      channel->cond.notify_all();
    }
    break;
  case deviceLinkClose:
    handleClose(channel);
    break;
  }
}

void DeviceLink::handleRequest(uint16_t id, const uint8_t* data,
                               size_t len)
{
  if (len < 2 || 2 + (size_t)data[1] + 1 > len) {
    vlog.error(_("Invalid device link request"));
    return;
  }
  if (channels.count(id)) {
    vlog.error(_("Device link request for a channel in use"));
    return;
  }

  Channel* channel = new Channel;
  channel->id = id;
  channel->kind = data[0];
  channel->name.assign((const char*)data + 2, data[1]);
  size_t labelLen = data[2 + data[1]];
  if (3 + data[1] + labelLen <= len)
    channel->label.assign((const char*)data + 3 + data[1], labelLen);
  if (channel->label.empty())
    channel->label = channel->name;
  channels[id] = channel;

  // Asking the user runs a nested event loop, which must not happen
  // while the connection is in the middle of reading messages
  questions.push_back(id);
  Fl::add_timeout(0, handleQuestions, this);
}

void DeviceLink::handleClose(Channel* channel)
{
  if (channel->state == Channel::Asking)
    closeDialog(channel->id);
  if (channel->state != Channel::Closing) {
    uint8_t reason = deviceLinkCloseNormal;
    send(deviceLinkClose, channel->id, &reason, 1);
  }
  stop(channel);
  erase(channel);
}

void DeviceLink::queueOp(Channel* channel, uint8_t subtype,
                         const uint8_t* data, size_t len)
{
  std::lock_guard<std::mutex> l(channel->lock);
  channel->ops.push_back({subtype, std::string((const char*)data, len)});
  channel->cond.notify_all();
}

// -=- Asking the user

namespace {
  struct DialogState {
    Fl_Window* win;
    Fl_Input_Choice* input;
    bool yes;
  };
}

// The dialog that is open, so that it can be closed when the server
// withdraws the request
static Fl_Window* openDialog = nullptr;
static DeviceLink* openDialogOwner = nullptr;
static uint16_t openDialogChannel = 0;

static void dialogYes(Fl_Widget*, void* data)
{
  DialogState* state = (DialogState*)data;
  state->yes = true;
  state->win->hide();
}

static void dialogNo(Fl_Widget*, void* data)
{
  ((DialogState*)data)->win->hide();
}

static void dialogBrowse(Fl_Widget*, void* data)
{
  DialogState* state = (DialogState*)data;
  Fl_Native_File_Chooser chooser;
  chooser.title(_("File to follow"));
  chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
  if (chooser.show() == 0 && chooser.filename())
    state->input->value(chooser.filename());
}

static void runDialog(Fl_Window* win, DeviceLink* owner, uint16_t id)
{
  openDialog = win;
  openDialogOwner = owner;
  openDialogChannel = id;
  win->set_modal();
  win->show();
  // Like the main loop: the connection goes on while the user thinks,
  // and it may withdraw the request
  while (win->shown()) {
    int next = core::Timer::checkTimeouts();
    if (next < 0 || next > 100)
      next = 100;
    Fl::wait(next / 1000.0);
  }
  openDialog = nullptr;
  openDialogOwner = nullptr;
}

// Asks whether the remote desktop may use a configured device. "Allow"
// is not the default button, so that Return typed into the session a
// moment ago does not allow anything.
static bool confirmDevice(DeviceLink* owner, uint16_t id,
                          const std::string& text)
{
  DialogState state;

  Fl_Window win(460, 150, _("Device for the remote desktop"));
  Fl_Box box(10, 10, 440, 90);
  box.copy_label(text.c_str());
  box.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP);
  Fl_Button allow(250, 110, 95, 28, _("Allow"));
  Fl_Button refuse(355, 110, 95, 28, _("Refuse"));
  win.end();

  state.win = &win;
  state.input = nullptr;
  state.yes = false;
  allow.callback(dialogYes, &state);
  refuse.callback(dialogNo, &state);
  win.callback(dialogNo, &state);

  runDialog(&win, owner, id);
  return state.yes;
}

// Asks which local device answers a name that is not configured
static bool chooseTarget(DeviceLink* owner, uint16_t id,
                         const std::string& text, uint8_t kind,
                         std::string* value, bool* remember)
{
  DialogState state;

  Fl_Window win(480, 200, _("Device for the remote desktop"));
  Fl_Box box(10, 10, 460, 70);
  box.copy_label(text.c_str());
  box.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP);

  const char* what = kind == deviceLinkSerial ? _("Serial port:") :
                     kind == deviceLinkTCP ? _("Host:port:") : _("File:");
  int inputWidth = kind == deviceLinkTail ? 265 : 300;
  Fl_Input_Choice input(160, 85, inputWidth, 25, what);
  if (kind == deviceLinkSerial) {
    std::vector<std::string> ports(DeviceLinkPort::serialPorts());
    for (const std::string& port : ports) {
      // Menu paths: a slash would start a submenu
      std::string item;
      for (char c : port) {
        if (c == '/' || c == '\\' || c == '&' || c == '_')
          item += '\\';
        item += c;
      }
      input.add(item.c_str());
    }
    if (!ports.empty())
      input.value(ports[0].c_str());
  }

  Fl_Button browse(430, 85, 40, 25, "...");
  if (kind != deviceLinkTail)
    browse.hide();

  Fl_Check_Button rememberButton(160, 120, 310, 25,
                                 _("Remember this choice"));
  rememberButton.value(1);

  // No default button: the session may have had keyboard focus a moment
  // ago
  Fl_Button use(270, 160, 95, 28, _("Use"));
  Fl_Button refuse(375, 160, 95, 28, _("Refuse"));
  win.end();

  state.win = &win;
  state.input = &input;
  state.yes = false;
  use.callback(dialogYes, &state);
  refuse.callback(dialogNo, &state);
  browse.callback(dialogBrowse, &state);
  win.callback(dialogNo, &state);

  runDialog(&win, owner, id);

  if (!state.yes || input.value() == nullptr || input.value()[0] == '\0')
    return false;

  *value = input.value();
  *remember = rememberButton.value() != 0;
  return true;
}

void DeviceLink::closeDialog(uint16_t id)
{
  if (openDialog != nullptr && openDialogOwner == this &&
      (openDialogChannel == id || id == 0))
    openDialog->hide();
}

void DeviceLink::handleQuestions(void* data)
{
  // Only one dialog at a time; requests that need the user wait for the
  // dialog that is open, the others go on
  static bool asking = false;
  DeviceLink* self = (DeviceLink*)data;
  std::list<uint16_t> later;

  while (instances.count(self) && !self->questions.empty()) {
    uint16_t id = self->questions.front();
    self->questions.pop_front();

    auto it = self->channels.find(id);
    if (it == self->channels.end() ||
        it->second->state != Channel::Asking)
      continue;

    DeviceLinkTarget target;
    std::string message;
    uint8_t status;
    Decision decision = self->decide(it->second, &target, &message,
                                     &status);

    if (decision == NeedsUser) {
      if (asking) {
        later.push_back(id);
        continue;
      }
      asking = true;
      decision = self->askUser(it->second, &target, &message, &status);
      asking = false;

      // The connection may be gone, or the server may have closed the
      // channel, while the user was thinking
      if (!instances.count(self))
        return;
      it = self->channels.find(id);
      if (it == self->channels.end() ||
          it->second->state != Channel::Asking)
        continue;
    }

    if (decision == Yes) {
      self->start(it->second, target);
    } else {
      vlog.info(_("Device \"%s\" not given to the remote desktop: %s"),
                it->second->name.c_str(), message.c_str());
      self->result(id, status, message);
      self->erase(it->second);
    }
  }

  if (instances.count(self))
    self->questions.splice(self->questions.begin(), later);
}

DeviceLink::Decision DeviceLink::decide(Channel* channel,
                                        DeviceLinkTarget* target,
                                        std::string* message,
                                        uint8_t* status)
{
  std::map<std::string, DeviceLinkTarget> targets(configuredTargets());
  auto it = targets.find(channel->name);
  uint8_t kind = channel->kind;

  *status = deviceLinkRefused;

  if (kind != deviceLinkSerial && kind != deviceLinkTCP &&
      kind != deviceLinkTail) {
    *status = deviceLinkUnavailable;
    *message = _("Unknown kind of device");
    return No;
  }

  if (!cc->isSecure() && !deviceLinkInsecure) {
    *message = _("The connection is not encrypted (DeviceLinkInsecure)");
    return No;
  }

  if (::deviceLinkOpen == "Never" && it == targets.end()) {
    *status = deviceLinkNotConfigured;
    *message = _("Not set up on this computer");
    return No;
  }
  if (::deviceLinkOpen == "Never") {
    *message = _("Not allowed (DeviceLinkOpen=Never)");
    return No;
  }

  if (it == targets.end())
    return NeedsUser;

  if (it->second.kind != kind) {
    *status = deviceLinkUnavailable;
    *message = core::format(_("Set up as %s on this computer"),
                            kindName(it->second.kind));
    return No;
  }

  *target = it->second;
  if (::deviceLinkOpen == "Ask" ||
      (::deviceLinkOpen == "Once" && !confirmed.count(channel->name)))
    return NeedsUser;
  return Yes;
}

DeviceLink::Decision DeviceLink::askUser(Channel* channel,
                                         DeviceLinkTarget* target,
                                         std::string* message,
                                         uint8_t* status)
{
  std::string name(channel->name), label(channel->label);
  uint8_t kind = channel->kind;
  uint16_t id = channel->id;

  *status = deviceLinkRefused;
  *message = _("Refused by the user");

  // A configured device, to be confirmed
  if (target->kind != 0) {
    std::string text(core::format(_("The remote desktop wants to use "
                                    "\"%s\" (%s, \"%s\") on this "
                                    "computer:\n\n%s"),
                                  label.c_str(), kindName(kind),
                                  name.c_str(), target->where.c_str()));
    if (!confirmDevice(this, id, text))
      return No;
    confirmed.insert(name);
    return Yes;
  }

  std::string text(core::format(_("The remote desktop wants to use \"%s\" "
                                  "(%s, \"%s\"), which is not set up on "
                                  "this computer. Which one should it "
                                  "use?"),
                                label.c_str(), kindName(kind),
                                name.c_str()));
  std::string value;
  bool remember;
  if (!chooseTarget(this, id, text, kind, &value, &remember))
    return No;

  std::string prefix(kind == deviceLinkSerial ? "serial:" :
                     kind == deviceLinkTCP ? "tcp:" : "tail:");
  std::string error;
  if (!DeviceLinkTarget::parse(prefix + value, target, &error)) {
    fl_alert("%s", error.c_str());
    *status = deviceLinkUnavailable;
    *message = error;
    return No;
  }
  confirmed.insert(name);

  if (remember) {
    std::string links(deviceLinks);
    while (!links.empty() && (links.back() == ';' || links.back() == ' '))
      links.pop_back();
    if (!links.empty())
      links += ";";
    links += name + "=" + target->toString();
    deviceLinks.setParam(links.c_str());
    try {
      saveViewerParameters(nullptr, cc->getServerName());
    } catch (std::exception& e) {
      vlog.error(_("Could not save the device: %s"), e.what());
    }
  }

  return Yes;
}

// -=- The threads

void DeviceLink::start(Channel* channel, const DeviceLinkTarget& target)
{
  channel->target = target;
  channel->fixed = target.fixed;
  channel->state = Channel::Opening;
  channel->reader = std::thread(readLoop, channel);
  channel->writer = std::thread(writeLoop, channel);

  if (!Fl::has_timeout(handlePoll, this))
    Fl::add_timeout(pollInterval, handlePoll, this);
}

void DeviceLink::readLoop(Channel* channel)
{
  std::string error;
  DeviceLinkPort* port = DeviceLinkPort::open(channel->target, &error);

  {
    std::lock_guard<std::mutex> l(channel->lock);
    channel->port = port;
    channel->openDone = true;
    channel->openError = error;
    channel->cond.notify_all();
  }
  if (port == nullptr)
    return;

  uint8_t buf[deviceLinkMaxData];
  while (!channel->stopping) {
    size_t want;
    {
      std::unique_lock<std::mutex> l(channel->lock);
      want = std::min<size_t>(channel->window, sizeof(buf));
    }

    int n = 0;
    if (want > 0) {
      n = port->read(buf, want, &error);
    } else {
      std::unique_lock<std::mutex> l(channel->lock);
      channel->cond.wait_for(l, std::chrono::milliseconds(100), [&] {
        return channel->stopping || channel->window > 0;
      });
    }

    uint8_t lines, errors;
    bool haveLines = port->lineState(&lines, &errors);

    std::lock_guard<std::mutex> l(channel->lock);
    if (n < 0) {
      channel->gone = true;
      channel->goneError = error;
      return;
    }
    if (n > 0) {
      channel->fromDevice.append((const char*)buf, n);
      channel->window -= n;
    }
    if (haveLines && (!channel->linesKnown || lines != channel->lines ||
                      errors)) {
      channel->linesKnown = true;
      channel->lines = lines;
      channel->errors |= errors;
      channel->lineChanged = true;
    }
  }
}

void DeviceLink::writeLoop(Channel* channel)
{
  DeviceLinkPort* port;

  {
    std::unique_lock<std::mutex> l(channel->lock);
    channel->cond.wait(l, [&] {
      return channel->stopping || channel->openDone;
    });
    port = channel->port;
  }
  if (port == nullptr)
    return;

  while (true) {
    Channel::Op op;
    {
      std::unique_lock<std::mutex> l(channel->lock);
      channel->cond.wait(l, [&] {
        return channel->stopping || !channel->ops.empty();
      });
      if (channel->stopping)
        return;
      op = channel->ops.front();
      channel->ops.pop_front();
    }

    const uint8_t* d = (const uint8_t*)op.data.data();
    size_t len = op.data.size();
    std::string error;

    switch (op.subtype) {
    case deviceLinkData:
      if (!port->write(d, len, channel->stopping, &error)) {
        std::lock_guard<std::mutex> l(channel->lock);
        channel->gone = true;
        channel->goneError = error;
        return;
      } else {
        std::lock_guard<std::mutex> l(channel->lock);
        channel->written += len;
      }
      break;
    case deviceLinkSetParams:
      if (len >= 9 && !channel->fixed) {
        uint32_t speed = ((uint32_t)d[1] << 24) | (d[2] << 16) |
                         (d[3] << 8) | d[4];
        port->setParams(d[0], speed, d[5], d[6], d[7], d[8]);
      }
      break;
    case deviceLinkSetLines:
      if (len >= 4) {
        if (d[0] && !channel->fixed)
          port->setLines(d[0], d[1]);
        unsigned ms = (d[2] << 8) | d[3];
        if (ms)
          port->sendBreak(ms);
      }
      break;
    case deviceLinkPurge:
      if (len >= 1)
        port->purge(d[0]);
      break;
    }
  }
}

void DeviceLink::handlePoll(void* data)
{
  DeviceLink* self = (DeviceLink*)data;
  self->poll();

  bool active = false;
  for (auto& entry : self->channels) {
    if (entry.second->state == Channel::Opening ||
        entry.second->state == Channel::Open)
      active = true;
  }
  if (active)
    Fl::repeat_timeout(pollInterval, handlePoll, data);
}

void DeviceLink::poll()
{
  std::list<Channel*> all;
  for (auto& entry : channels)
    all.push_back(entry.second);

  for (Channel* channel : all) {
    std::string fromDevice;
    uint32_t written;
    bool lineChanged, gone, opened = false, failed = false;
    uint8_t lines, errors;
    std::string error;

    {
      std::lock_guard<std::mutex> l(channel->lock);
      if (channel->state == Channel::Opening && channel->openDone) {
        opened = channel->port != nullptr;
        failed = !opened;
        error = channel->openError;
      }
      fromDevice.swap(channel->fromDevice);
      written = channel->written;
      channel->written = 0;
      lineChanged = channel->lineChanged;
      channel->lineChanged = false;
      lines = channel->lines;
      errors = channel->errors;
      channel->errors = 0;
      gone = channel->gone;
      if (gone)
        error = channel->goneError;
    }

    if (failed) {
      vlog.error(_("Could not open %s for \"%s\": %s"),
                 channel->target.where.c_str(), channel->name.c_str(),
                 error.c_str());
      cc->deviceLinkMessage(core::format(_("Could not open %s: %s"),
                                         channel->target.where.c_str(),
                                         error.c_str()).c_str());
      result(channel->id, deviceLinkUnavailable, error);
      stop(channel);
      erase(channel);
      continue;
    }
    if (opened) {
      vlog.info(_("Device \"%s\" is %s"), channel->name.c_str(),
                channel->target.toString().c_str());
      result(channel->id, rfb::deviceLinkOpen, "");
      channel->state = Channel::Open;
    }
    if (channel->state != Channel::Open)
      continue;

    for (size_t pos = 0; pos < fromDevice.size(); pos += deviceLinkMaxData) {
      size_t n = std::min(fromDevice.size() - pos, deviceLinkMaxData);
      send(deviceLinkData, channel->id,
           (const uint8_t*)fromDevice.data() + pos, n);
    }
    if (written) {
      uint8_t credit[4] = { (uint8_t)(written >> 24),
                            (uint8_t)(written >> 16),
                            (uint8_t)(written >> 8), (uint8_t)written };
      send(deviceLinkCredit, channel->id, credit, sizeof(credit));
    }
    if (lineChanged) {
      uint8_t state[2] = { lines, errors };
      send(deviceLinkLineState, channel->id, state, sizeof(state));
    }
    if (gone) {
      vlog.error(_("Device \"%s\" is gone: %s"), channel->name.c_str(),
                 error.c_str());
      cc->deviceLinkMessage(core::format(_("%s is gone: %s"),
                                         channel->target.where.c_str(),
                                         error.c_str()).c_str());
      uint8_t reason = deviceLinkCloseError;
      send(deviceLinkClose, channel->id, &reason, 1);
      channel->state = Channel::Closing;
      stop(channel);
    }
  }
}

void DeviceLink::stop(Channel* channel)
{
  {
    std::lock_guard<std::mutex> l(channel->lock);
    channel->stopping = true;
    channel->cond.notify_all();
  }
  if (channel->reader.joinable())
    channel->reader.join();
  if (channel->writer.joinable())
    channel->writer.join();
  delete channel->port;
  channel->port = nullptr;
}

void DeviceLink::erase(Channel* channel)
{
  channels.erase(channel->id);
  delete channel;
}

void DeviceLink::send(uint8_t subtype, uint16_t channel,
                      const uint8_t* data, size_t len)
{
  cc->sendDeviceLink(subtype, channel, data, len);
}

void DeviceLink::result(uint16_t channel, uint8_t status,
                        const std::string& message)
{
  std::string m(message.substr(0, deviceLinkMaxPayload - 3));
  std::vector<uint8_t> msg;
  msg.push_back(status);
  msg.push_back(m.size() >> 8);
  msg.push_back(m.size());
  msg.insert(msg.end(), m.begin(), m.end());
  send(deviceLinkRequestResult, channel, msg.data(), msg.size());
}
