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

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <chrono>
#include <map>
#include <stdexcept>

#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>
#include <FL/fl_utf8.h>

#include <core/LogWriter.h>
#include <core/i18n.h>
#include <core/string.h>

#include "CConn.h"
#include "FileDrop.h"

static core::LogWriter vlog("FileDrop");

// Paths from FLTK are UTF-8, also on Windows
static FILE* openFile(const std::string& path)
{
  return fl_fopen(path.c_str(), "rb");
}

static bool statFile(const std::string& path, int64_t* size)
{
  struct stat st;
  if (fl_stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return false;
  if (size)
    *size = st.st_size;
  return true;
}

static int seekFile(FILE* f, int64_t offset)
{
#ifdef WIN32
  return _fseeki64(f, offset, SEEK_SET);
#else
  return fseeko(f, offset, SEEK_SET);
#endif
}

static const size_t chunkSize = 4 * 1024 * 1024;
static const int maxAttempts = 5;


// ----------------------------------------------------------------------
// A minimal JSON reader for the endpoint's answers

namespace {

struct JValue {
  enum Type { Null, Bool, Number, String, Array, Object };

  JValue() : type(Null), number(0), boolean(false) {}

  const JValue& operator[](const std::string& key) const {
    static const JValue none;
    std::map<std::string, JValue>::const_iterator it = object.find(key);
    return it == object.end() ? none : it->second;
  }

  Type type;
  double number;
  bool boolean;
  std::string string;
  std::vector<JValue> array;
  std::map<std::string, JValue> object;
};

class JParser {
public:
  JParser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()),
                                  depth(0) {}

  JValue parse() {
    JValue v = value();
    ws();
    if (p != end)
      fail();
    return v;
  }

private:
  [[noreturn]] void fail() {
    throw std::runtime_error(_("Invalid answer from the desktop"));
  }

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
  }

  bool lit(const char* s) {
    size_t n = strlen(s);
    if ((size_t)(end - p) < n || strncmp(p, s, n) != 0)
      return false;
    p += n;
    return true;
  }

  void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += (char)cp;
    } else if (cp < 0x800) {
      out += (char)(0xc0 | (cp >> 6));
      out += (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += (char)(0xe0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3f));
      out += (char)(0x80 | (cp & 0x3f));
    } else {
      out += (char)(0xf0 | (cp >> 18));
      out += (char)(0x80 | ((cp >> 12) & 0x3f));
      out += (char)(0x80 | ((cp >> 6) & 0x3f));
      out += (char)(0x80 | (cp & 0x3f));
    }
  }

  unsigned hex4() {
    if (end - p < 4)
      fail();
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else fail();
    }
    return v;
  }

  std::string str() {
    std::string out;
    if (p >= end || *p != '"')
      fail();
    p++;
    while (p < end && *p != '"') {
      if ((unsigned char)*p < 0x20)
        fail();
      if (*p != '\\') {
        out += *p++;
        continue;
      }
      if (++p >= end)
        fail();
      switch (*p++) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        unsigned cp = hex4();
        if (cp >= 0xd800 && cp < 0xdc00 && lit("\\u")) {
          unsigned lo = hex4();
          if (lo < 0xdc00 || lo >= 0xe000)
            fail();
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        }
        utf8(out, cp);
        break;
      }
      default:
        fail();
      }
    }
    if (p >= end)
      fail();
    p++;
    return out;
  }

  JValue value() {
    JValue v;
    ws();
    if (p >= end || ++depth > 32)
      fail();
    if (*p == '{') {
      v.type = JValue::Object;
      p++;
      ws();
      if (p < end && *p == '}') {
        p++;
      } else {
        for (;;) {
          ws();
          std::string key = str();
          ws();
          if (p >= end || *p++ != ':')
            fail();
          v.object[key] = value();
          ws();
          if (p < end && *p == ',') { p++; continue; }
          if (p < end && *p == '}') { p++; break; }
          fail();
        }
      }
    } else if (*p == '[') {
      v.type = JValue::Array;
      p++;
      ws();
      if (p < end && *p == ']') {
        p++;
      } else {
        for (;;) {
          v.array.push_back(value());
          ws();
          if (p < end && *p == ',') { p++; continue; }
          if (p < end && *p == ']') { p++; break; }
          fail();
        }
      }
    } else if (*p == '"') {
      v.type = JValue::String;
      v.string = str();
    } else if (lit("true")) {
      v.type = JValue::Bool;
      v.boolean = true;
    } else if (lit("false")) {
      v.type = JValue::Bool;
    } else if (lit("null")) {
      v.type = JValue::Null;
    } else {
      char* e;
      v.number = strtod(p, &e);
      if (e == p || e > end)
        fail();
      v.type = JValue::Number;
      p = e;
    }
    depth--;
    return v;
  }

  const char* p;
  const char* end;
  int depth;
};

std::string jsonString(const std::string& s)
{
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

std::string baseName(const std::string& path)
{
  size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

// The client's guess; the desktop checks the content itself
std::string guessType(const std::string& name)
{
  static const char* const types[][2] = {
    { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
    { "png", "image/png" }, { "gif", "image/gif" },
    { "webp", "image/webp" }, { "svg", "image/svg+xml" },
    { "tif", "image/tiff" }, { "tiff", "image/tiff" },
    { "heic", "image/heic" }, { "pdf", "application/pdf" },
    { "txt", "text/plain" }, { "csv", "text/csv" },
    { "html", "text/html" }, { "xml", "application/xml" },
    { "zip", "application/zip" }, { "mp4", "video/mp4" },
    { "mov", "video/quicktime" }, { "webm", "video/webm" },
    { "mp3", "audio/mpeg" }, { "wav", "audio/wav" },
    { "ogg", "audio/ogg" },
    { "odt", "application/vnd.oasis.opendocument.text" },
    { "ods", "application/vnd.oasis.opendocument.spreadsheet" },
    { "doc", "application/msword" },
    { "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
    { "xls", "application/vnd.ms-excel" },
    { "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
  };
  size_t dot = name.rfind('.');
  if (dot == std::string::npos)
    return "application/octet-stream";
  std::string ext = name.substr(dot + 1);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z')
      c = c - 'A' + 'a';
  }
  for (const auto& t : types) {
    if (ext == t[0])
      return t[1];
  }
  return "application/octet-stream";
}

bool sha256File(const std::string& path, std::string* hex)
{
  FILE* f = openFile(path);
  if (!f)
    return false;

  gnutls_hash_hd_t hd;
  if (gnutls_hash_init(&hd, GNUTLS_DIG_SHA256) < 0) {
    fclose(f);
    return false;
  }
  std::vector<uint8_t> buf(1 << 20);
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
    gnutls_hash(hd, buf.data(), n);
  bool ok = !ferror(f);
  fclose(f);

  uint8_t digest[32];
  gnutls_hash_deinit(hd, digest);
  hex->clear();
  for (uint8_t b : digest) {
    char h[3];
    snprintf(h, sizeof(h), "%02x", b);
    *hex += h;
  }
  return ok;
}

bool isHttpURL(const std::string& url)
{
  return url.compare(0, 7, "http://") == 0 ||
         url.compare(0, 8, "https://") == 0;
}

}

// ----------------------------------------------------------------------

FileDrop::FileDrop(CConn* cc_)
  : cc(cc_), usable(false), generation(0), stopping(false)
{
  Fl::add_timeout(0.2, handleQueue, this);
}

FileDrop::~FileDrop()
{
  Fl::remove_timeout(handleQueue, this);

  stopping = true;
  {
    std::lock_guard<std::mutex> guard(lock);
    for (FileDropHttp* http : running)
      http->abort();
  }
  if (eventsThread.joinable())
    eventsThread.join();
  for (Job& job : jobs)
    job.thread.join();
}

void FileDrop::setEndpoint(const rfb::DesktopEndpoint& endpoint,
                           const std::string& rfbHost)
{
  if (endpoint.service != "file-drop")
    return;

  stopEvents();

  std::lock_guard<std::mutex> guard(lock);
  generation++;
  usable = false;
  token.clear();

  if (endpoint.withdrawn()) {
    vlog.info(_("The desktop no longer accepts dropped files"));
    return;
  }

  FileDropURL url;
  if (!url.parse(endpoint.url)) {
    vlog.error(_("Invalid file drop endpoint: %s"), endpoint.url.c_str());
    return;
  }
  if (endpoint.flags & rfb::desktopEndpointSameHost) {
    // Not for connections over a local socket
    if (rfbHost.empty() || rfbHost[0] == '/') {
      vlog.error(_("The file drop endpoint needs the server's host name"));
      return;
    }
    url.host = rfbHost;
  }
  if (url.path.empty() || url.path[url.path.size() - 1] != '/')
    url.path += "/";

  base = url;
  token.assign(endpoint.token.begin(), endpoint.token.end());
  usable = true;
  vlog.info(_("The desktop accepts dropped files at %s"), base.str().c_str());

  uint64_t gen = generation;
  FileDropURL b = base;
  std::string t = token;
  eventsThread = std::thread(&FileDrop::eventsLoop, this, gen, b, t);
}

void FileDrop::stopEvents()
{
  {
    std::lock_guard<std::mutex> guard(lock);
    generation++;
    for (FileDropHttp* http : running)
      http->abort();
  }
  if (eventsThread.joinable())
    eventsThread.join();
}

bool FileDrop::active()
{
  std::lock_guard<std::mutex> guard(lock);
  return usable;
}

void FileDrop::drop(int x, int y, const std::vector<std::string>& paths)
{
  std::lock_guard<std::mutex> guard(lock);
  if (!usable || paths.empty())
    return;

  // Finished jobs can go
  for (std::list<Job>::iterator it = jobs.begin(); it != jobs.end(); ) {
    if (!*it->done) {
      ++it;
      continue;
    }
    it->thread.join();
    it = jobs.erase(it);
  }

  Job job;
  job.done = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> done = job.done;
  FileDropURL b = base;
  std::string t = token;
  job.thread = std::thread([this, x, y, paths, b, t, done]() {
    dropJob(x, y, paths, b, t);
    *done = true;
  });
  jobs.push_back(std::move(job));
}

std::vector<std::string> FileDrop::parseDrop(const char* text, int len)
{
  std::vector<std::string> paths;
  std::string all(text, len);
  size_t pos = 0;

  while (pos < all.size()) {
    size_t eol = all.find('\n', pos);
    if (eol == std::string::npos)
      eol = all.size();
    std::string line = all.substr(pos, eol - pos);
    pos = eol + 1;

    while (!line.empty() && (line.back() == '\r' || line.back() == '\0'))
      line.pop_back();
    if (line.empty())
      continue;

    if (line.compare(0, 7, "file://") == 0) {
      // file:///path or file://host/path; percent-decoded
      size_t start = line.find('/', 7);
      if (start == std::string::npos)
        return std::vector<std::string>();
      std::string path;
      for (size_t i = start; i < line.size(); i++) {
        if (line[i] == '%' && i + 2 < line.size() &&
            isxdigit((unsigned char)line[i + 1]) &&
            isxdigit((unsigned char)line[i + 2])) {
          path += (char)strtol(line.substr(i + 1, 2).c_str(), nullptr, 16);
          i += 2;
        } else {
          path += line[i];
        }
      }
#ifdef WIN32
      // file:///C:/dir/file
      if (path.size() > 2 && path[0] == '/' && path[2] == ':')
        path.erase(0, 1);
#endif
      line = path;
    }

    if (!statFile(line, nullptr))
      return std::vector<std::string>();
    paths.push_back(line);
  }
  return paths;
}

FileDropHttp* FileDrop::newHttp()
{
  FileDropHttp* http = new FileDropHttp;
  std::lock_guard<std::mutex> guard(lock);
  running.insert(http);
  if (stopping)
    http->abort();
  return http;
}

void FileDrop::releaseHttp(FileDropHttp* http)
{
  {
    std::lock_guard<std::mutex> guard(lock);
    running.erase(http);
  }
  delete http;
}

bool FileDrop::sleepFor(int seconds, uint64_t gen)
{
  for (int i = 0; i < seconds * 10; i++) {
    if (stopping)
      return false;
    if (gen != 0) {
      std::lock_guard<std::mutex> guard(lock);
      if (gen != generation)
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !stopping;
}

void FileDrop::dropJob(int x, int y, std::vector<std::string> paths,
                       FileDropURL endpoint, std::string endpointToken)
{
  std::vector<File> files;

  for (const std::string& path : paths) {
    File f;
    f.path = path;
    f.name = baseName(path);
    if (!statFile(path, &f.size) || !sha256File(path, &f.sha256)) {
      message(core::format(_("Could not read \"%s\""), f.name.c_str()), true);
      return;
    }
    f.type = guessType(f.name);
    files.push_back(f);
  }

  std::string offer = "{\"x\": " + std::to_string(x) +
                      ", \"y\": " + std::to_string(y) + ", \"files\": [";
  for (size_t i = 0; i < files.size(); i++) {
    if (i)
      offer += ", ";
    offer += "{\"name\": " + jsonString(files[i].name) +
             ", \"size\": " + std::to_string(files[i].size) +
             ", \"sha256\": " + jsonString(files[i].sha256) +
             ", \"type\": " + jsonString(files[i].type) + "}";
  }
  offer += "]}";

  JValue answer;
  FileDropHttp* http = newHttp();
  try {
    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + endpointToken;
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "application/json";
    FileDropResponse resp = http->request("POST",
                                          endpoint.resolve("offers"),
                                          headers, offer);
    if (resp.status != 200)
      throw std::runtime_error(core::format(_("HTTP status %d"),
                                            resp.status));
    answer = JParser(resp.body).parse();
  } catch (std::exception& e) {
    releaseHttp(http);
    if (!stopping)
      message(core::format(_("The desktop could not take the files: %s"),
                           e.what()), true);
    return;
  }
  releaseHttp(http);

  const JValue& results = answer["files"];
  if (results.type != JValue::Array ||
      results.array.size() != files.size()) {
    message(_("Invalid answer from the desktop"), true);
    return;
  }

  for (size_t i = 0; i < files.size(); i++) {
    const JValue& r = results.array[i];
    if (r["result"].string != "accept") {
      std::string reason = r["reason"].string;
      if (reason.empty())
        reason = _("not accepted here");
      message(core::format(_("\"%s\": %s"), files[i].name.c_str(),
                           reason.c_str()), true);
      continue;
    }

    std::string error;
    try {
      FileDropURL url = endpoint.resolve(r["upload"]["url"].string);
      message(core::format(_("Uploading \"%s\"..."),
                           files[i].name.c_str()), false);
      if (upload(files[i], url, r["upload"]["token"].string, &error))
        message(core::format(_("\"%s\" uploaded"), files[i].name.c_str()),
                false);
      else if (!stopping)
        message(core::format(_("Upload of \"%s\" failed: %s"),
                             files[i].name.c_str(), error.c_str()), true);
    } catch (std::exception& e) {
      message(core::format(_("Upload of \"%s\" failed: %s"),
                           files[i].name.c_str(), e.what()), true);
    }
  }
}

// tus 1.0.0 core protocol: HEAD for the offset, PATCH from there
bool FileDrop::upload(const File& file, const FileDropURL& url,
                      const std::string& uploadToken, std::string* error)
{
  std::map<std::string, std::string> headers;
  headers["Tus-Resumable"] = "1.0.0";
  headers["Authorization"] = "Bearer " + uploadToken;

  FILE* f = openFile(file.path);
  if (!f) {
    *error = _("Could not read the file");
    return false;
  }

  int failures = 0;
  int64_t offset = -1;
  bool ok = false;

  while (!stopping) {
    FileDropHttp* http = newHttp();
    try {
      if (offset < 0) {
        FileDropResponse resp = http->request("HEAD", url, headers);
        if (resp.status == 404 || resp.status == 410 || resp.status == 401) {
          *error = _("The upload is no longer known to the desktop");
          releaseHttp(http);
          break;
        }
        if (resp.status != 200 || !resp.headers.count("upload-offset"))
          throw std::runtime_error(core::format(_("HTTP status %d"),
                                                resp.status));
        offset = strtoll(resp.headers["upload-offset"].c_str(), nullptr, 10);
        if (offset < 0 || offset > file.size) {
          *error = _("Invalid answer from the desktop");
          releaseHttp(http);
          break;
        }
      }
      if (offset == file.size) {
        ok = true;
        releaseHttp(http);
        break;
      }

      int64_t len = file.size - offset;
      if (len > (int64_t)chunkSize)
        len = chunkSize;
      if (seekFile(f, offset) != 0)
        throw std::runtime_error(_("Could not read the file"));

      std::map<std::string, std::string> patchHeaders(headers);
      patchHeaders["Upload-Offset"] = std::to_string(offset);
      patchHeaders["Content-Type"] = "application/offset+octet-stream";
      FileDropResponse resp = http->request(
        "PATCH", url, patchHeaders, "",
        [f](uint8_t* buf, size_t n) -> long {
          return fread(buf, 1, n, f);
        }, len);
      releaseHttp(http);
      http = nullptr;

      if (resp.status == 204) {
        offset = resp.headers.count("upload-offset") ?
                 strtoll(resp.headers["upload-offset"].c_str(), nullptr, 10) :
                 offset + len;
        failures = 0;
        continue;
      }
      if (resp.status == 460) {
        *error = _("The file changed while it was uploaded");
        break;
      }
      if (resp.status == 404 || resp.status == 410) {
        *error = _("The upload is no longer known to the desktop");
        break;
      }
      // The desktop has all bytes and stores the file later
      if (resp.status == 503 && resp.headers.count("upload-offset") &&
          strtoll(resp.headers["upload-offset"].c_str(), nullptr, 10) ==
          file.size) {
        ok = true;
        break;
      }
      throw std::runtime_error(core::format(_("HTTP status %d"), resp.status));
    } catch (std::exception& e) {
      if (http)
        releaseHttp(http);
      if (stopping)
        break;
      // Ask for the offset again and continue there
      offset = -1;
      if (++failures >= maxAttempts) {
        *error = e.what();
        break;
      }
      vlog.info(_("Upload of \"%s\" interrupted: %s"), file.name.c_str(),
                e.what());
      if (!sleepFor(failures * 2))
        break;
    }
  }

  fclose(f);
  return ok;
}

// Server-Sent Events: "open" asks to show a document locally
void FileDrop::eventsLoop(uint64_t gen, FileDropURL endpoint,
                          std::string endpointToken)
{
  int delay = 1;

  for (;;) {
    {
      std::lock_guard<std::mutex> guard(lock);
      if (stopping || gen != generation)
        return;
    }

    std::string buffer, event, data;
    bool unauthorized = false;
    FileDropHttp* http = newHttp();
    try {
      std::map<std::string, std::string> headers;
      headers["Authorization"] = "Bearer " + endpointToken;
      headers["Accept"] = "text/event-stream";
      FileDropResponse resp = http->request(
        "GET", endpoint.resolve("events"), headers, "", nullptr, -1,
        [&](const char* p, size_t n) -> bool {
          buffer.append(p, n);
          size_t eol;
          while ((eol = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, eol);
            buffer.erase(0, eol + 1);
            if (!line.empty() && line.back() == '\r')
              line.pop_back();
            if (line.empty()) {
              if (event == "open" && !data.empty()) {
                try {
                  JValue v = JParser(data).parse();
                  openRequest(v["url"].string, v["name"].string);
                } catch (std::exception& e) {
                  vlog.error("%s", e.what());
                }
              }
              event.clear();
              data.clear();
            } else if (line.compare(0, 6, "event:") == 0) {
              event = line.substr(line[6] == ' ' ? 7 : 6);
            } else if (line.compare(0, 5, "data:") == 0) {
              if (!data.empty())
                data += "\n";
              data += line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
            }
            delay = 1;
          }
          return buffer.size() < 65536;
        });
      unauthorized = resp.status == 401;
      if (resp.status != 200 && !unauthorized)
        vlog.info(_("File drop events: HTTP status %d"), resp.status);
    } catch (std::exception& e) {
      vlog.debug("File drop events: %s", e.what());
    }
    releaseHttp(http);

    if (unauthorized) {
      vlog.error(_("The desktop no longer knows the file drop token"));
      std::lock_guard<std::mutex> guard(lock);
      if (gen == generation)
        usable = false;
      return;
    }
    if (!sleepFor(delay, gen))
      return;
    if (delay < 30)
      delay *= 2;
  }
}

void FileDrop::message(const std::string& text, bool error)
{
  QueueItem item;
  item.kind = error ? QueueItem::Error : QueueItem::Message;
  item.text = text;
  std::lock_guard<std::mutex> guard(queueLock);
  queue.push_back(item);
}

void FileDrop::openRequest(const std::string& url, const std::string& name)
{
  if (!isHttpURL(url)) {
    vlog.error(_("Ignoring request to open a non-HTTP URL"));
    return;
  }
  QueueItem item;
  item.kind = QueueItem::Open;
  item.text = url;
  item.name = name;
  std::lock_guard<std::mutex> guard(queueLock);
  queue.push_back(item);
}

void FileDrop::handleQueue(void* data)
{
  FileDrop* self = (FileDrop*)data;
  std::list<QueueItem> items;

  {
    std::lock_guard<std::mutex> guard(self->queueLock);
    items.swap(self->queue);
  }
  // Before the items: a dialog below runs a nested event loop
  Fl::repeat_timeout(0.2, handleQueue, self);

  // Messages first; then at most one dialog, as the connection (and with
  // it this object) may be gone when its nested event loop returns
  std::list<QueueItem> opens;
  for (const QueueItem& item : items) {
    if (item.kind == QueueItem::Open)
      opens.push_back(item);
    else
      self->cc->fileDropMessage(item.text.c_str(),
                                item.kind == QueueItem::Error);
  }
  if (opens.empty())
    return;

  QueueItem open = opens.front();
  opens.pop_front();
  {
    std::lock_guard<std::mutex> guard(self->queueLock);
    self->queue.splice(self->queue.begin(), opens);
  }
  openDocument(open.text, open.name);
}

void FileDrop::openDocument(const std::string& url, const std::string& name)
{
  std::string what = name.empty() ? url : name;

  // Never open without asking: the URL comes from the remote side.
  // "Open" is not the default button, so that Return (typed into the
  // session a moment ago) does not open anything.
  int choice = fl_choice(_("The remote desktop wants to open \"%s\" on "
                           "this computer.\n\n%s"),
                         _("Cancel"), nullptr, _("Open"),
                         what.c_str(), url.c_str());
  if (choice != 2)
    return;

  char msg[256];
  if (!fl_open_uri(url.c_str(), msg, sizeof(msg)))
    vlog.error(_("Could not open %s: %s"), url.c_str(), msg);
}
