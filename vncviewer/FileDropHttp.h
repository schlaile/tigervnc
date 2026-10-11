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

// A small blocking HTTP/1.1 client for the file drop side channel (see
// doc/file-drop.md): one request per connection, plain or TLS (GnuTLS),
// for use from worker threads.

#ifndef __FILEDROPHTTP_H__
#define __FILEDROPHTTP_H__

#include <stdint.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace network { class TcpSocket; }

struct FileDropURL {
  FileDropURL() : port(0) {}

  bool parse(const std::string& url);
  // A reference relative to this URL (absolute, absolute path, or
  // relative path)
  FileDropURL resolve(const std::string& ref) const;
  std::string str() const;

  std::string scheme;           // "http" or "https"
  std::string host;             // without brackets
  int port;
  std::string path;             // with query, starts with "/"
};

struct FileDropResponse {
  FileDropResponse() : status(0) {}

  int status;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
};

class FileDropHttp {
public:
  // Request body: called with a buffer, returns the number of bytes
  // written into it (0 at the end, <0 on errors)
  typedef std::function<long(uint8_t* buf, size_t len)> BodySource;
  // Streamed response body (event streams): called per chunk of data,
  // returns false to stop
  typedef std::function<bool(const char* data, size_t len)> BodySink;

  FileDropHttp();
  ~FileDropHttp();

  // Performs a request. Throws std::exception on connection and protocol
  // errors. Without a sink, the body is collected in the response (at
  // most maxBody bytes).
  FileDropResponse request(const std::string& method,
                           const FileDropURL& url,
                           const std::map<std::string, std::string>& headers,
                           const std::string& body = "",
                           BodySource source = nullptr,
                           int64_t sourceLength = -1,
                           BodySink sink = nullptr,
                           size_t maxBody = 1 << 20);

  // Aborts a running request from another thread
  void abort();

private:
  void connect(const FileDropURL& url);
  void disconnect();
  void writeAll(const void* data, size_t len);
  long readSome(void* data, size_t len);

  network::TcpSocket* sock;
  void* session;                // gnutls_session_t
  void* creds;                  // gnutls_certificate_credentials_t
  std::mutex lock;
  std::atomic<bool> aborted;
};

#endif
