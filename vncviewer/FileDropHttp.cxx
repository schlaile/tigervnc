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

#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <errno.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <stdexcept>
#include <vector>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include <core/i18n.h>
#include <network/TcpSocket.h>

#include "FileDropHttp.h"

// Wakes up a thread blocked on the socket
static void shutdownSocket(network::TcpSocket* sock)
{
#ifdef WIN32
  ::shutdown(sock->getFd(), SD_BOTH);
#else
  ::shutdown(sock->getFd(), SHUT_RDWR);
#endif
}

static std::string lower(std::string s)
{
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z')
      c = c - 'A' + 'a';
  }
  return s;
}

static std::string trim(const std::string& s)
{
  size_t b = s.find_first_not_of(" \t");
  if (b == std::string::npos)
    return "";
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool FileDropURL::parse(const std::string& url)
{
  size_t sep = url.find("://");
  if (sep == std::string::npos)
    return false;

  scheme = lower(url.substr(0, sep));
  if (scheme == "http")
    port = 80;
  else if (scheme == "https")
    port = 443;
  else
    return false;

  size_t start = sep + 3;
  size_t slash = url.find('/', start);
  std::string authority = url.substr(start, slash == std::string::npos ?
                                     std::string::npos : slash - start);
  path = slash == std::string::npos ? "/" : url.substr(slash);

  if (authority.find('@') != std::string::npos)
    return false;

  std::string portStr;
  if (!authority.empty() && authority[0] == '[') {
    size_t end = authority.find(']');
    if (end == std::string::npos)
      return false;
    host = authority.substr(1, end - 1);
    if (end + 1 < authority.size()) {
      if (authority[end + 1] != ':')
        return false;
      portStr = authority.substr(end + 2);
    }
  } else {
    size_t colon = authority.find(':');
    host = authority.substr(0, colon);
    if (colon != std::string::npos)
      portStr = authority.substr(colon + 1);
  }

  if (host.empty())
    return false;
  if (!portStr.empty()) {
    char* end;
    long p = strtol(portStr.c_str(), &end, 10);
    if (*end != '\0' || p <= 0 || p > 65535)
      return false;
    port = p;
  }

  for (char c : path) {
    if ((unsigned char)c <= 0x20 || (unsigned char)c >= 0x7f)
      return false;
  }
  return true;
}

FileDropURL FileDropURL::resolve(const std::string& ref) const
{
  FileDropURL u;

  if (ref.find("://") != std::string::npos) {
    if (!u.parse(ref))
      throw std::runtime_error(_("Invalid URL"));
    return u;
  }

  u = *this;
  if (!ref.empty() && ref[0] == '/') {
    u.path = ref;
  } else {
    std::string base = path.substr(0, path.find('?'));
    u.path = base.substr(0, base.rfind('/') + 1) + ref;
  }
  return u;
}

std::string FileDropURL::str() const
{
  std::string s = scheme + "://";
  if (host.find(':') != std::string::npos)
    s += "[" + host + "]";
  else
    s += host;
  if (!((scheme == "http" && port == 80) ||
        (scheme == "https" && port == 443)))
    s += ":" + std::to_string(port);
  return s + path;
}

FileDropHttp::FileDropHttp()
  : sock(nullptr), session(nullptr), creds(nullptr), aborted(false)
{
}

FileDropHttp::~FileDropHttp()
{
  disconnect();
}

void FileDropHttp::abort()
{
  std::lock_guard<std::mutex> guard(lock);
  aborted = true;
  if (sock)
    shutdownSocket(sock);
}

void FileDropHttp::connect(const FileDropURL& url)
{
  network::TcpSocket* s = new network::TcpSocket(url.host.c_str(),
                                                 url.port);
  {
    std::lock_guard<std::mutex> guard(lock);
    sock = s;
    if (aborted) {
      shutdownSocket(sock);
      throw std::runtime_error(_("Aborted"));
    }
  }

  if (url.scheme != "https")
    return;

  gnutls_certificate_credentials_t cred;
  gnutls_session_t sess;
  int ret;

  if (gnutls_certificate_allocate_credentials(&cred) != GNUTLS_E_SUCCESS)
    throw std::runtime_error(_("TLS initialisation failed"));
  creds = cred;
  gnutls_certificate_set_x509_system_trust(cred);

  if (gnutls_init(&sess, GNUTLS_CLIENT) != GNUTLS_E_SUCCESS)
    throw std::runtime_error(_("TLS initialisation failed"));
  session = sess;
  gnutls_set_default_priority(sess);
  gnutls_credentials_set(sess, GNUTLS_CRD_CERTIFICATE, cred);

  // Server name indication is only for names, not for addresses
  if (url.host.find_first_not_of("0123456789.") != std::string::npos &&
      url.host.find(':') == std::string::npos)
    gnutls_server_name_set(sess, GNUTLS_NAME_DNS, url.host.data(),
                           url.host.size());
  gnutls_session_set_verify_cert(sess, url.host.c_str(), 0);
  gnutls_transport_set_int(sess, sock->getFd());

  do {
    ret = gnutls_handshake(sess);
  } while (ret < 0 && !gnutls_error_is_fatal(ret));
  if (ret < 0)
    throw std::runtime_error(std::string(_("TLS handshake failed: ")) +
                             gnutls_strerror(ret));
}

void FileDropHttp::disconnect()
{
  if (session) {
    gnutls_deinit((gnutls_session_t)session);
    session = nullptr;
  }
  if (creds) {
    gnutls_certificate_free_credentials(
      (gnutls_certificate_credentials_t)creds);
    creds = nullptr;
  }
  std::lock_guard<std::mutex> guard(lock);
  delete sock;
  sock = nullptr;
}

void FileDropHttp::writeAll(const void* data, size_t len)
{
  const char* p = (const char*)data;

  while (len > 0) {
    long n;
    if (session) {
      n = gnutls_record_send((gnutls_session_t)session, p, len);
      if (n == GNUTLS_E_AGAIN || n == GNUTLS_E_INTERRUPTED)
        continue;
    } else {
      n = send(sock->getFd(), p, len, 0);
#ifndef WIN32
      if (n < 0 && errno == EINTR)
        continue;
#endif
    }
    if (n <= 0 || aborted)
      throw std::runtime_error(_("Connection lost"));
    p += n;
    len -= n;
  }
}

long FileDropHttp::readSome(void* data, size_t len)
{
  for (;;) {
    long n;
    if (session) {
      n = gnutls_record_recv((gnutls_session_t)session, data, len);
      if (n == GNUTLS_E_AGAIN || n == GNUTLS_E_INTERRUPTED)
        continue;
      if (n == GNUTLS_E_PREMATURE_TERMINATION)
        n = 0;
    } else {
      n = recv(sock->getFd(), (char*)data, len, 0);
#ifndef WIN32
      if (n < 0 && errno == EINTR)
        continue;
#endif
    }
    if (aborted)
      throw std::runtime_error(_("Aborted"));
    if (n < 0)
      throw std::runtime_error(_("Connection lost"));
    return n;
  }
}

FileDropResponse FileDropHttp::request(
  const std::string& method, const FileDropURL& url,
  const std::map<std::string, std::string>& headers,
  const std::string& body, BodySource source, int64_t sourceLength,
  BodySink sink, size_t maxBody)
{
  FileDropResponse resp;

  disconnect();
  connect(url);

  std::string head = method + " " + url.path + " HTTP/1.1\r\n";
  std::string hostHeader = url.str();
  hostHeader = hostHeader.substr(url.scheme.size() + 3);
  hostHeader = hostHeader.substr(0, hostHeader.find('/'));
  head += "Host: " + hostHeader + "\r\n";
  head += "Connection: close\r\n";
  head += "User-Agent: TigerVNC\r\n";
  for (const auto& h : headers)
    head += h.first + ": " + h.second + "\r\n";
  if (source)
    head += "Content-Length: " + std::to_string(sourceLength) + "\r\n";
  else if (!body.empty() || method == "POST" || method == "PATCH")
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  head += "\r\n";

  writeAll(head.data(), head.size());
  if (source) {
    std::vector<uint8_t> buf(64 * 1024);
    int64_t sent = 0;
    while (sent < sourceLength) {
      size_t want = buf.size();
      if ((int64_t)want > sourceLength - sent)
        want = sourceLength - sent;
      long n = source(buf.data(), want);
      if (n <= 0)
        throw std::runtime_error(_("Failed to read the file"));
      writeAll(buf.data(), n);
      sent += n;
    }
  } else if (!body.empty()) {
    writeAll(body.data(), body.size());
  }

  // Response head
  std::string data;
  size_t end;
  char buf[16384];
  while ((end = data.find("\r\n\r\n")) == std::string::npos) {
    if (data.size() > 65536)
      throw std::runtime_error(_("Invalid HTTP response"));
    long n = readSome(buf, sizeof(buf));
    if (n == 0)
      throw std::runtime_error(_("Connection closed by the server"));
    data.append(buf, n);
  }
  std::string respHead = data.substr(0, end);
  data.erase(0, end + 4);

  size_t lineEnd = respHead.find("\r\n");
  std::string statusLine = respHead.substr(0, lineEnd);
  if (statusLine.compare(0, 5, "HTTP/") != 0 ||
      statusLine.find(' ') == std::string::npos)
    throw std::runtime_error(_("Invalid HTTP response"));
  resp.status = atoi(statusLine.c_str() + statusLine.find(' ') + 1);

  size_t pos = lineEnd == std::string::npos ? respHead.size() : lineEnd + 2;
  while (pos < respHead.size()) {
    size_t next = respHead.find("\r\n", pos);
    if (next == std::string::npos)
      next = respHead.size();
    std::string line = respHead.substr(pos, next - pos);
    size_t colon = line.find(':');
    if (colon != std::string::npos)
      resp.headers[lower(trim(line.substr(0, colon)))] =
        trim(line.substr(colon + 1));
    pos = next + 2;
  }

  if (method == "HEAD" || resp.status == 204 || resp.status == 304 ||
      resp.status < 200) {
    disconnect();
    return resp;
  }

  // Body: chunked, by length, or until the connection closes
  bool chunked = lower(resp.headers["transfer-encoding"]).find("chunked") !=
                 std::string::npos;
  int64_t length = -1;
  if (!chunked && resp.headers.count("content-length"))
    length = strtoll(resp.headers["content-length"].c_str(), nullptr, 10);

  auto deliver = [&](const char* p, size_t n) -> bool {
    if (sink)
      return sink(p, n);
    if (resp.body.size() + n > maxBody)
      throw std::runtime_error(_("HTTP response too large"));
    resp.body.append(p, n);
    return true;
  };
  auto fill = [&]() -> bool {
    long n = readSome(buf, sizeof(buf));
    if (n == 0)
      return false;
    data.append(buf, n);
    return true;
  };

  if (chunked) {
    for (;;) {
      size_t le;
      while ((le = data.find("\r\n")) == std::string::npos) {
        if (!fill())
          throw std::runtime_error(_("Connection closed by the server"));
      }
      long size = strtol(data.c_str(), nullptr, 16);
      data.erase(0, le + 2);
      if (size <= 0)
        break;
      while ((long)data.size() < size + 2) {
        if (!fill())
          throw std::runtime_error(_("Connection closed by the server"));
      }
      if (!deliver(data.data(), size))
        break;
      data.erase(0, size + 2);
    }
  } else {
    int64_t got = 0;
    for (;;) {
      if (!data.empty()) {
        size_t n = data.size();
        if (length >= 0 && (int64_t)n > length - got)
          n = length - got;
        if (!deliver(data.data(), n))
          break;
        got += n;
        data.clear();
      }
      if (length >= 0 && got >= length)
        break;
      if (!fill()) {
        if (length >= 0)
          throw std::runtime_error(_("Connection closed by the server"));
        break;
      }
    }
  }

  disconnect();
  return resp;
}
