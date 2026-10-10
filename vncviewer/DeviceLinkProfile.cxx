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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <FL/fl_utf8.h>

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>
#include <core/xdgdirs.h>

#include "DeviceLinkProfile.h"
#include "parameters.h"

static core::LogWriter vlog("DeviceLink");

DeviceLinkProfile::DeviceLinkProfile()
  : ask(AskOnce), insecure(false)
{
}

// Shell-like matching of * and ?, ignoring case
static bool globMatch(const char* pattern, const char* text)
{
  while (*pattern) {
    if (*pattern == '*') {
      pattern++;
      do {
        if (globMatch(pattern, text))
          return true;
      } while (*text++);
      return false;
    }
    if (*text == '\0' ||
        (*pattern != '?' && tolower((unsigned char)*pattern) !=
                            tolower((unsigned char)*text)))
      return false;
    pattern++;
    text++;
  }
  return *text == '\0';
}

bool DeviceLinkProfile::matchesServer(const std::string& server) const
{
  if (servers.empty())
    return true;
  for (const std::string& pattern : servers) {
    if (globMatch(pattern.c_str(), server.c_str()))
      return true;
  }
  return false;
}

std::string DeviceLinkProfile::path()
{
  if (strlen(deviceLinksFile) > 0)
    return (const char*)deviceLinksFile;

  const char* dir = core::getvncconfigdir();
  if (dir == nullptr)
    return "";
  return std::string(dir) + "/device-links";
}

std::vector<std::string> DeviceLinkProfile::split(const std::string& line)
{
  std::vector<std::string> words;
  size_t i = 0;

  while (i < line.size()) {
    while (i < line.size() && isspace((unsigned char)line[i]))
      i++;
    if (i >= line.size() || line[i] == '#')
      break;

    std::string word;
    while (i < line.size() && !isspace((unsigned char)line[i])) {
      if (line[i] == '"') {
        size_t end = line.find('"', i + 1);
        if (end == std::string::npos)
          end = line.size();
        word += line.substr(i + 1, end - i - 1);
        i = end + 1;
      } else {
        word += line[i++];
      }
    }
    words.push_back(word);
  }

  return words;
}

std::string DeviceLinkProfile::quote(const std::string& word)
{
  if (!word.empty() &&
      word.find_first_of(" \t\"#") == std::string::npos)
    return word;
  return "\"" + word + "\"";
}

static bool readLines(const std::string& path, std::vector<std::string>* lines,
                      std::string* error)
{
  FILE* f = fl_fopen(path.c_str(), "r");
  if (f == nullptr) {
    *error = core::format(_("Could not open \"%s\": %s"), path.c_str(),
                          strerror(errno));
    return false;
  }

  std::string line;
  int c;
  while ((c = fgetc(f)) != EOF) {
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lines->push_back(line);
      line.clear();
    } else {
      line += (char)c;
    }
  }
  if (!line.empty())
    lines->push_back(line);
  fclose(f);

  // A byte order mark from Windows editors
  if (!lines->empty() && lines->front().compare(0, 3, "\xEF\xBB\xBF") == 0)
    lines->front().erase(0, 3);
  return true;
}

bool DeviceLinkProfile::loadAll(std::vector<DeviceLinkProfile>* profiles,
                                std::string* error)
{
  std::string file(path());
  std::vector<std::string> lines;

  if (!readLines(file, &lines, error))
    return false;

  DeviceLinkProfile* profile = nullptr;
  for (size_t n = 0; n < lines.size(); n++) {
    std::string line(lines[n]);
    size_t start = line.find_first_not_of(" \t");

    if (start != std::string::npos && line[start] == '[') {
      size_t end = line.find(']', start);
      if (end == std::string::npos) {
        vlog.error(_("%s:%d: missing ]"), file.c_str(), (int)n + 1);
        profile = nullptr;
        continue;
      }
      profiles->push_back(DeviceLinkProfile());
      profile = &profiles->back();
      profile->name = line.substr(start + 1, end - start - 1);
      continue;
    }

    std::vector<std::string> words(split(line));
    if (words.empty())
      continue;
    if (profile == nullptr) {
      vlog.error(_("%s:%d: outside of a [profile]"), file.c_str(),
                 (int)n + 1);
      continue;
    }

    const std::string& key = words[0];
    if (key == "description") {
      profile->description.clear();
      for (size_t i = 1; i < words.size(); i++)
        profile->description += (i > 1 ? " " : "") + words[i];
    } else if (key == "server" && words.size() >= 2) {
      profile->servers.insert(profile->servers.end(), words.begin() + 1,
                              words.end());
    } else if (key == "ask" && words.size() == 2 &&
               (words[1] == "always" || words[1] == "once" ||
                words[1] == "never")) {
      profile->ask = words[1] == "always" ? AskAlways :
                     words[1] == "once" ? AskOnce : AskNever;
    } else if (key == "insecure" && words.size() == 2 &&
               (words[1] == "yes" || words[1] == "no")) {
      profile->insecure = words[1] == "yes";
    } else if (key == "description" || key == "server" || key == "ask" ||
               key == "insecure") {
      vlog.error(_("%s:%d: invalid \"%s\""), file.c_str(), (int)n + 1,
                 key.c_str());
    } else {
      DeviceLinkTarget target;
      std::string problem;
      if (!DeviceLinkTarget::parse(std::vector<std::string>(
                                     words.begin() + 1, words.end()),
                                   &target, &problem)) {
        vlog.error(_("%s:%d: device \"%s\": %s"), file.c_str(), (int)n + 1,
                   key.c_str(), problem.c_str());
        continue;
      }
      if (!profile->devices.count(key))
        profile->order.push_back(key);
      profile->devices[key] = target;
    }
  }

  return true;
}

bool DeviceLinkProfile::load(const std::string& name,
                             DeviceLinkProfile* profile, std::string* error)
{
  std::vector<DeviceLinkProfile> profiles;

  if (!loadAll(&profiles, error))
    return false;
  for (const DeviceLinkProfile& p : profiles) {
    if (p.name == name) {
      *profile = p;
      return true;
    }
  }
  *error = core::format(_("There is no device profile \"%s\" in \"%s\""),
                        name.c_str(), path().c_str());
  return false;
}

bool DeviceLinkProfile::addDevice(const std::string& profile,
                                  const std::string& name,
                                  const DeviceLinkTarget& target,
                                  std::string* error)
{
  std::string file(path());
  std::vector<std::string> lines;
  std::string ignored;

  if (file.empty()) {
    *error = _("Could not determine VNC config directory path");
    return false;
  }
  readLines(file, &lines, &ignored);

  std::string line(name);
  line.resize(std::max<size_t>(line.size() + 1, 13), ' ');
  for (const std::string& word : target.toWords())
    line += quote(word) + " ";
  line.pop_back();

  // After the last line of the profile that is not empty
  size_t insert = lines.size();
  bool found = false;
  for (size_t n = 0; n < lines.size(); n++) {
    std::string l(lines[n]);
    size_t start = l.find_first_not_of(" \t");
    bool section = start != std::string::npos && l[start] == '[';
    if (section && found)
      break;
    if (section && l.substr(start + 1, l.find(']') - start - 1) == profile)
      found = true;
    if (found && start != std::string::npos)
      insert = n + 1;
  }
  if (!found) {
    if (!lines.empty() && !lines.back().empty())
      lines.push_back("");
    lines.push_back("[" + profile + "]");
    insert = lines.size();
  }
  lines.insert(lines.begin() + insert, line);

  FILE* f = fl_fopen(file.c_str(), "w");
  if (f == nullptr) {
    *error = core::format(_("Could not write \"%s\": %s"), file.c_str(),
                          strerror(errno));
    return false;
  }
  for (const std::string& l : lines)
    fprintf(f, "%s\n", l.c_str());
  if (fclose(f) != 0) {
    *error = core::format(_("Could not write \"%s\": %s"), file.c_str(),
                          strerror(errno));
    return false;
  }
  return true;
}
