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

#include <core/LogWriter.h>

#include "FileDropApp.h"

#ifdef WIN32
#include <windows.h>
#include <shlwapi.h>
#elif !defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

static core::LogWriter vlog("FileDropApp");

#ifndef __APPLE__
namespace {

// Characters that need no quoting anywhere: what a URL of the endpoint
// consists of (it is percent-encoded)
bool plainURL(const std::string& url)
{
  if (url.compare(0, 7, "http://") != 0 &&
      url.compare(0, 8, "https://") != 0)
    return false;
  for (unsigned char c : url) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          strchr("-._~:/?#[]@!&'()*+,;=%", c) != nullptr))
      return false;
  }
  return true;
}

}
#endif

bool desktopExecArgs(const std::string& exec, const std::string& url,
                     std::vector<std::string>* argv)
{
  // The string value's escapes first, then the Exec quoting
  std::string value;
  for (size_t i = 0; i < exec.size(); i++) {
    if (exec[i] == '\\' && i + 1 < exec.size()) {
      char c = exec[++i];
      switch (c) {
      case 's': value += ' '; break;
      case 'n': value += '\n'; break;
      case 't': value += '\t'; break;
      case 'r': value += '\r'; break;
      case '\\': value += '\\'; break;
      default: value += '\\'; value += c;
      }
    } else {
      value += exec[i];
    }
  }

  std::vector<std::string> words;
  std::vector<bool> quoted;
  std::string word;
  bool inWord = false, inQuotes = false, wasQuoted = false;
  for (size_t i = 0; i < value.size(); i++) {
    char c = value[i];
    if (inQuotes) {
      if (c == '\\' && i + 1 < value.size() &&
          strchr("\"`$\\", value[i + 1]) != nullptr)
        word += value[++i];
      else if (c == '"')
        inQuotes = false;
      else
        word += c;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      if (inWord) {
        words.push_back(word);
        quoted.push_back(wasQuoted);
      }
      word.clear();
      inWord = wasQuoted = false;
    } else if (c == '"') {
      inQuotes = inWord = wasQuoted = true;
    } else {
      word += c;
      inWord = true;
    }
  }
  if (inQuotes)
    return false;
  if (inWord) {
    words.push_back(word);
    quoted.push_back(wasQuoted);
  }

  // Field codes: %u and %U take the URL; %f and %F mean files only
  bool takesURL = false;
  argv->clear();
  for (size_t w = 0; w < words.size(); w++) {
    const std::string& in = words[w];
    if (quoted[w]) {            // no field codes in quoted arguments
      argv->push_back(in);
      continue;
    }
    std::string out;
    bool code = false;
    for (size_t i = 0; i < in.size(); i++) {
      if (in[i] != '%' || i + 1 == in.size()) {
        out += in[i];
        continue;
      }
      char c = in[++i];
      if (c == '%') {
        out += '%';
      } else if (c == 'u' || c == 'U') {
        out += url;
        takesURL = true;
      } else if (c == 'f' || c == 'F') {
        return false;
      } else {
        code = true;            // %i, %c, %k and deprecated ones: dropped
      }
    }
    if (!(code && out.empty()))
      argv->push_back(out);
  }
  return takesURL && !argv->empty() && !(*argv)[0].empty();
}

#if !defined(WIN32) && !defined(__APPLE__)

namespace {

// Standard output of a program, without the trailing newline
bool runCapture(const std::vector<std::string>& args, std::string* out)
{
  int fds[2];
  if (pipe(fds) == -1)
    return false;
  pid_t pid = fork();
  if (pid == -1) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    std::vector<char*> argv;
    for (const std::string& a : args)
      argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    dup2(fds[1], 1);
    int null = open("/dev/null", O_RDWR);
    if (null != -1) {
      dup2(null, 0);
      dup2(null, 2);
    }
    for (int fd = 3; fd < 1024; fd++)
      close(fd);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  char buf[512];
  ssize_t n;
  out->clear();
  while ((n = read(fds[0], buf, sizeof(buf))) > 0 ||
         (n == -1 && errno == EINTR)) {
    if (n > 0 && out->size() < 4096)
      out->append(buf, n);
  }
  close(fds[0]);
  int status;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
    ;
  while (!out->empty() && (out->back() == '\n' || out->back() == '\r'))
    out->pop_back();
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// The Exec value of a desktop entry, empty if not an application to run
std::string desktopExec(const std::string& path)
{
  FILE* f = fopen(path.c_str(), "r");
  if (f == nullptr)
    return "";
  std::string exec, type;
  bool entry = false, hidden = false;
  char line[4096];
  while (fgets(line, sizeof(line), f) != nullptr) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
      l.pop_back();
    if (!l.empty() && l[0] == '[') {
      entry = l == "[Desktop Entry]";
      continue;
    }
    if (!entry)
      continue;
    size_t eq = l.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = l.substr(0, eq), value = l.substr(eq + 1);
    while (!key.empty() && key.back() == ' ')
      key.pop_back();
    while (!value.empty() && value[0] == ' ')
      value.erase(0, 1);
    if (key == "Exec")
      exec = value;
    else if (key == "Type")
      type = value;
    else if (key == "Hidden")
      hidden = value == "true";
  }
  fclose(f);
  if (hidden || type != "Application")
    return "";
  return exec;
}

// Where the desktop entry with this id is (Desktop Entry Specification:
// the data directories' "applications", a '-' may stand for a '/')
std::string findDesktopEntry(const std::string& id)
{
  std::vector<std::string> dirs;
  const char* home = getenv("XDG_DATA_HOME");
  if (home != nullptr && home[0] == '/')
    dirs.push_back(home);
  else if (getenv("HOME") != nullptr)
    dirs.push_back(std::string(getenv("HOME")) + "/.local/share");
  const char* data = getenv("XDG_DATA_DIRS");
  std::string list = data != nullptr && data[0] != '\0' ? data
                     : "/usr/local/share:/usr/share";
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(':', start);
    if (end == std::string::npos)
      end = list.size();
    if (end > start && list[start] == '/')
      dirs.push_back(list.substr(start, end - start));
    start = end + 1;
  }

  for (const std::string& dir : dirs) {
    std::string name = id;
    for (;;) {
      std::string path = dir + "/applications/" + name;
      struct stat st;
      if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
        return path;
      size_t dash = name.find('-');
      if (dash == std::string::npos)
        break;
      name[dash] = '/';
    }
  }
  return "";
}

// Starts a program in its own session, not waiting for it; false if it
// could not be started
bool spawnDetached(const std::vector<std::string>& args)
{
  int fds[2];
  if (pipe(fds) == -1)
    return false;
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  pid_t pid = fork();
  if (pid == -1) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    // The grandchild runs the program; the child exits at once, so that
    // nothing is left for the viewer to wait for
    if (fork() != 0)
      _exit(0);
    setsid();
    std::vector<char*> argv;
    for (const std::string& a : args)
      argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int null = open("/dev/null", O_RDWR);
    if (null != -1)
      dup2(null, 0);
    for (int fd = 3; fd < 1024; fd++) {
      if (fd != fds[1])
        close(fd);
    }
    execvp(argv[0], argv.data());
    int err = errno;
    ssize_t ignored = write(fds[1], &err, sizeof(err));
    (void)ignored;
    _exit(127);
  }
  close(fds[1]);
  int status;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
    ;
  // Closed without a word once the program runs
  int err = 0;
  ssize_t n;
  while ((n = read(fds[0], &err, sizeof(err))) == -1 && errno == EINTR)
    ;
  close(fds[0]);
  if (n > 0) {
    vlog.error("%s: %s", args[0].c_str(), strerror(err));
    return false;
  }
  return true;
}

}

bool openWithDefaultApp(const std::string& url, const std::string& type,
                        const std::string& /*extension*/)
{
  if (!plainURL(url))
    return false;
  // A MIME type, nothing a program could take for an option
  if (type.empty() || type[0] == '-' ||
      type.find('/') == std::string::npos)
    return false;
  for (unsigned char c : type) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          strchr("/.+-", c) != nullptr))
      return false;
  }

  std::string id;
  if (!runCapture({"xdg-mime", "query", "default", type}, &id) ||
      id.empty() || id.find('/') != std::string::npos) {
    vlog.debug("No default application for %s", type.c_str());
    return false;
  }
  std::string path = findDesktopEntry(id);
  if (path.empty()) {
    vlog.debug("Desktop entry %s not found", id.c_str());
    return false;
  }
  std::vector<std::string> argv;
  if (!desktopExecArgs(desktopExec(path), url, &argv)) {
    vlog.debug("%s does not take URLs", id.c_str());
    return false;
  }
  vlog.info("Opening %s with %s", url.c_str(), id.c_str());
  return spawnDetached(argv);
}

#elif defined(WIN32)

bool openWithDefaultApp(const std::string& url, const std::string& /*type*/,
                        const std::string& extension)
{
  if (!plainURL(url) || extension.empty())
    return false;

  // The command the system runs to open a file of this type, e.g.
  // "C:\...\vlc.exe" --started-from-file "%1".  Players registered as
  // apps have none (they are started differently); they get the file.
  std::wstring ext = L"." + std::wstring(extension.begin(), extension.end());
  wchar_t command[2048];
  DWORD len = sizeof(command) / sizeof(command[0]);
  if (AssocQueryStringW(ASSOCF_NOTRUNCATE | ASSOCF_INIT_IGNOREUNKNOWN,
                        ASSOCSTR_COMMAND, ext.c_str(), L"open", command,
                        &len) != S_OK)
    return false;
  wchar_t expanded[2048];
  if (ExpandEnvironmentStringsW(command, expanded,
                                sizeof(expanded) / sizeof(expanded[0])) == 0)
    return false;
  std::wstring cmd(expanded);
  if (cmd.find(L"DelegateExecute") != std::wstring::npos)
    return false;

  std::wstring wurl(url.begin(), url.end());     // plain ASCII
  bool takes = false;
  std::wstring line;
  for (size_t i = 0; i < cmd.size(); i++) {
    if (cmd[i] == L'%' && i + 1 < cmd.size()) {
      wchar_t c = cmd[i + 1];
      if (c == L'1' || c == L'L' || c == L'l') {
        bool quoted = i > 0 && cmd[i - 1] == L'"';
        line += quoted ? wurl : L"\"" + wurl + L"\"";
        takes = true;
        i++;
        continue;
      }
      if (c == L'*') {
        i++;
        continue;
      }
    }
    line += cmd[i];
  }
  if (!takes)
    return false;

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  std::vector<wchar_t> buf(line.begin(), line.end());
  buf.push_back(0);
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &si, &pi)) {
    vlog.error("Could not start the player: error %lu", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  vlog.info("Opening %s with the player for .%s", url.c_str(),
            extension.c_str());
  return true;
}

#endif
