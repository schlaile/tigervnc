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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#ifdef WIN32
#include <windows.h>
#include <winhttp.h>
#elif !defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#include <nettle/eddsa.h>
#include <nettle/sha2.h>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>

#include <core/Configuration.h>
#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/xdgdirs.h>

#include "UpdateCheck.h"

static core::LogWriter vlog("UpdateCheck");

core::BoolParameter
  updateCheck("UpdateCheck",
              _("Look for a new version of the viewer at start (at most "
                "once a day)"),
              true);

// ---- parts for the tests

bool decodeBase64(const std::string& in, std::string* out)
{
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned bits = 0;
  int nbits = 0, pad = 0;
  out->clear();
  for (char c : in) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    if (c == '=') {
      pad++;
      continue;
    }
    if (pad > 0)
      return false;               // data after padding
    const char* p = strchr(alphabet, c);
    if (p == nullptr || c == '\0')
      return false;
    bits = (bits << 6) | (unsigned)(p - alphabet);
    nbits += 6;
    if (nbits >= 8) {
      nbits -= 8;
      *out += (char)((bits >> nbits) & 0xff);
    }
  }
  return pad <= 2;
}

std::string sha256Hex(const std::string& data)
{
  struct sha256_ctx ctx;
  uint8_t digest[SHA256_DIGEST_SIZE];
  sha256_init(&ctx);
  sha256_update(&ctx, data.size(), (const uint8_t*)data.data());
  sha256_digest(&ctx, sizeof(digest), digest);
  std::string hex;
  for (uint8_t b : digest) {
    static const char digits[] = "0123456789abcdef";
    hex += digits[b >> 4];
    hex += digits[b & 15];
  }
  return hex;
}

bool parseSignedUpdate(const std::string& text, const std::string& sig,
                       const std::string& key, UpdateInfo* info)
{
  std::string rawKey, rawSig;
  if (!decodeBase64(key, &rawKey) || rawKey.size() != ED25519_KEY_SIZE)
    return false;
  if (!decodeBase64(sig, &rawSig) ||
      rawSig.size() != ED25519_SIGNATURE_SIZE)
    return false;
  if (!ed25519_sha512_verify((const uint8_t*)rawKey.data(), text.size(),
                             (const uint8_t*)text.data(),
                             (const uint8_t*)rawSig.data()))
    return false;

  *info = UpdateInfo();
  size_t pos = 0;
  bool first = true;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos)
      eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (first) {
      if (line != "TigerVNC update 1")
        return false;
      first = false;
      continue;
    }
    size_t sp = line.find(' ');
    std::string name = line.substr(0, sp);
    std::string value = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (name == "release") {
      char* end;
      info->release = strtol(value.c_str(), &end, 10);
      if (value.empty() || *end != '\0' || info->release <= 0)
        return false;
    } else if (name == "version") {
      info->version = value;
    } else if (name == "page") {
      info->page = value;
    } else if (name == "windows") {
      char url[1024], hash[65];
      unsigned long size;
      if (sscanf(value.c_str(), "%1023s %lu %64s", url, &size, hash) != 3)
        return false;
      info->windowsURL = url;
      info->windowsSize = size;
      info->windowsSHA256 = hash;
    }
  }
  if (first || info->release == 0)
    return false;
  for (const std::string* u : {&info->page, &info->windowsURL}) {
    if (!u->empty() && u->compare(0, 8, "https://") != 0)
      return false;
  }
  return true;
}

// ---- fetching

#ifdef WIN32

namespace {

std::wstring widen(const std::string& s)
{
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0)
    return L"";
  std::vector<wchar_t> buf(n);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), n);
  return buf.data();
}

}

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

bool updateHttpGet(const std::string& url, int timeoutSeconds,
                   size_t maxSize, std::string* body, std::string* error)
{
  std::wstring wurl = widen(url);
  URL_COMPONENTSW parts;
  wchar_t host[256], path[2048];
  memset(&parts, 0, sizeof(parts));
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host;
  parts.dwHostNameLength = sizeof(host) / sizeof(host[0]);
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = sizeof(path) / sizeof(path[0]);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS) {
    *error = "not an https URL";
    return false;
  }

  bool ok = false;
  HINTERNET session = WinHttpOpen(L"TigerVNC", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr)        // before Windows 8.1
    session = WinHttpOpen(L"TigerVNC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET connect = nullptr, request = nullptr;
  if (session != nullptr) {
    int ms = timeoutSeconds * 1000;
    WinHttpSetTimeouts(session, ms, ms, ms, ms);
    connect = WinHttpConnect(session, host, parts.nPort, 0);
  }
  if (connect != nullptr)
    request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
  if (request != nullptr &&
      WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      *error = "HTTP status " + std::to_string(status);
    } else {
      body->clear();
      ok = true;
      for (;;) {
        char buf[16384];
        DWORD got = 0;
        if (!WinHttpReadData(request, buf, sizeof(buf), &got)) {
          ok = false;
          break;
        }
        if (got == 0)
          break;
        if (body->size() + got > maxSize) {
          *error = "too large";
          ok = false;
          break;
        }
        body->append(buf, got);
      }
    }
  }
  if (!ok && error->empty())
    *error = "Windows error " + std::to_string(GetLastError());
  if (request)
    WinHttpCloseHandle(request);
  if (connect)
    WinHttpCloseHandle(connect);
  if (session)
    WinHttpCloseHandle(session);
  return ok;
}

#elif !defined(__APPLE__)

// Elsewhere through curl, if it is there (the update is only a notice)
bool updateHttpGet(const std::string& url, int timeoutSeconds,
                   size_t maxSize, std::string* body, std::string* error)
{
  if (url.compare(0, 8, "https://") != 0) {
    *error = "not an https URL";
    return false;
  }
  int fds[2];
  if (pipe(fds) == -1) {
    *error = strerror(errno);
    return false;
  }
  std::string timeout = std::to_string(timeoutSeconds);
  pid_t pid = fork();
  if (pid == -1) {
    *error = strerror(errno);
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    dup2(fds[1], 1);
    int null = open("/dev/null", O_RDWR);
    if (null != -1) {
      dup2(null, 0);
      dup2(null, 2);
    }
    for (int fd = 3; fd < 1024; fd++)
      close(fd);
    execlp("curl", "curl", "-s", "-f", "-L", "--proto", "=https",
           "--max-time", timeout.c_str(), "--", url.c_str(), (char*)nullptr);
    _exit(127);
  }
  close(fds[1]);
  body->clear();
  bool big = false;
  char buf[16384];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0 ||
         (n == -1 && errno == EINTR)) {
    if (n <= 0)
      continue;
    if (body->size() + n > maxSize) {
      big = true;
      break;
    }
    body->append(buf, n);
  }
  close(fds[0]);
  if (big)
    kill(pid, SIGTERM);
  int status;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
    ;
  if (big) {
    *error = "too large";
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    *error = "curl failed";
    return false;
  }
  return true;
}

#endif

// ---- the check

namespace {

const char* stampPath()
{
  static std::string path;
  const char* dir = core::getvncstatedir();
  if (dir == nullptr)
    return nullptr;
  path = std::string(dir) + "/update-check";
  return path.c_str();
}

bool checkedToday()
{
  const char* path = stampPath();
  if (path == nullptr)
    return false;
  FILE* f = fopen(path, "r");
  if (f == nullptr)
    return false;
  long long then = 0;
  bool ok = fscanf(f, "%lld", &then) == 1;
  fclose(f);
  long long now = time(nullptr);
  return ok && then <= now && now - then < 24 * 60 * 60;
}

void markChecked()
{
  const char* path = stampPath();
  if (path == nullptr)
    return;
  FILE* f = fopen(path, "w");
  if (f == nullptr)
    return;
  fprintf(f, "%lld\n", (long long)time(nullptr));
  fclose(f);
}

#ifdef WIN32

// Runs work in a thread while a small window says what happens
void withProgress(const char* text, const std::function<void()>& work)
{
  std::atomic<bool> done(false);
  std::thread t([&]() {
    work();
    done = true;
  });
  Fl_Window win(360, 80, _("TigerVNC"));
  Fl_Box box(10, 10, 340, 60, text);
  box.align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  win.end();
  win.set_modal();
  win.show();
  while (!done)
    Fl::wait(0.1);
  win.hide();
  t.join();
}

// Only an installed viewer (not one from the zip) is replaced
bool installedViewer(std::wstring* dir)
{
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return false;
  std::wstring path(exe);
  size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos)
    return false;
  *dir = path.substr(0, slash);
  return GetFileAttributesW((*dir + L"\\unins000.exe").c_str()) !=
         INVALID_FILE_ATTRIBUTES;
}

// The installer, silent, with the viewer started again afterwards with
// the same arguments, as far as they pass through its command line
bool runInstaller(const UpdateInfo& info, int argc, char** argv)
{
  std::string data, error;
  bool fetched = false;
  withProgress(_("Downloading the new version of the viewer..."), [&]() {
    fetched = updateHttpGet(info.windowsURL, 60, 200 * 1024 * 1024, &data,
                            &error);
  });
  if (!fetched) {
    fl_alert(_("Could not download the new version: %s"), error.c_str());
    return false;
  }
  if (data.size() != info.windowsSize ||
      sha256Hex(data) != info.windowsSHA256) {
    vlog.error("The downloaded installer does not match the update "
               "description");
    fl_alert(_("The downloaded installer does not match the signed "
               "update description; it is not run."));
    return false;
  }

  wchar_t tmp[MAX_PATH];
  if (GetTempPathW(MAX_PATH, tmp) == 0)
    return false;
  std::wstring setup = std::wstring(tmp) + L"TigerVNC-Update-" +
                       std::to_wstring(info.release) + L".exe";
  FILE* f = _wfopen(setup.c_str(), L"wb");
  if (f == nullptr)
    return false;
  bool written = fwrite(data.data(), 1, data.size(), f) == data.size();
  if (fclose(f) != 0 || !written) {
    DeleteFileW(setup.c_str());
    return false;
  }

  std::string args;
  for (int i = 1; i < argc; i++) {
    if (strpbrk(argv[i], " \t\"") != nullptr) {
      args.clear();
      break;
    }
    args += (args.empty() ? "" : " ") + std::string(argv[i]);
  }
  std::wstring cmd = L"\"" + setup + L"\" /SILENT /SUPPRESSMSGBOXES "
                     L"/NORESTART /RELAUNCH=1";
  if (!args.empty())
    cmd += L" \"/RELAUNCHARGS=" + widen(args) + L"\"";

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &si, &pi)) {
    vlog.error("Could not run the installer: error %lu", GetLastError());
    DeleteFileW(setup.c_str());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  vlog.info("Installing %s", info.version.c_str());
  return true;
}

#endif

}

bool checkForUpdate(int argc, char** argv)
{
#if defined(UPDATE_URL) && defined(UPDATE_KEY) && defined(UPDATE_RELEASE)
  if (!updateCheck || checkedToday())
    return false;

  // Short timeouts: it holds up the start
  std::string text, sig, error;
  if (!updateHttpGet(UPDATE_URL, 5, 64 * 1024, &text, &error) ||
      !updateHttpGet(std::string(UPDATE_URL) + ".sig", 5, 4096, &sig,
                     &error)) {
    vlog.info("No update description: %s", error.c_str());
    return false;               // again at the next start
  }
  markChecked();

  UpdateInfo info;
  if (!parseSignedUpdate(text, sig, UPDATE_KEY, &info)) {
    vlog.error("The update description is not correctly signed");
    return false;
  }
  if (info.release <= UPDATE_RELEASE) {
    vlog.debug("Up to date (release %ld)", (long)UPDATE_RELEASE);
    return false;
  }
  vlog.info("New version %s (release %ld)", info.version.c_str(),
            info.release);

#ifdef WIN32
  std::wstring dir;
  if (installedViewer(&dir) && !info.windowsURL.empty()) {
    int choice = fl_choice(_("A new version of the viewer is available "
                             "(%s).\n\nInstall it now? The viewer starts "
                             "again afterwards."),
                           _("Later"), nullptr, _("Install"),
                           info.version.c_str());
    if (choice != 2)
      return false;
    return runInstaller(info, argc, argv);
  }
#else
  (void)argc;
  (void)argv;
#endif

  if (info.page.empty())
    return false;
  int choice = fl_choice(_("A new version of the viewer is available "
                           "(%s)."),
                         _("Later"), nullptr, _("Open download page"),
                         info.version.c_str());
  if (choice == 2) {
    char msg[256];
    if (!fl_open_uri(info.page.c_str(), msg, sizeof(msg)))
      vlog.error("Could not open %s: %s", info.page.c_str(), msg);
  }
  return false;
#else
  (void)argc;
  (void)argv;
  return false;
#endif
}
