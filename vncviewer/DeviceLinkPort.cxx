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
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <netdb.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/serial.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include <core/i18n.h>
#include <core/string.h>

#include <rfb/DeviceLink.h>

#include "DeviceLinkPort.h"

using namespace rfb;

static const int pollMs = 100;

// -=- DeviceLinkTarget

DeviceLinkTarget::DeviceLinkTarget()
  : kind(0), speed(0), dataBits(0), parity(deviceLinkParityNone),
    stopBits(0), flow(-1), dtr(-1), rts(-1), fixed(false),
    fromStart(false)
{
}

static bool parseFormat(const std::string& word, DeviceLinkTarget* t)
{
  static const char parities[] = "NOEMS";
  const char* p;

  if (word.size() < 3 || word[0] < '5' || word[0] > '8' || word[1] == '\0' ||
      (p = strchr(parities, toupper(word[1]))) == nullptr)
    return false;
  std::string stop(word.substr(2));
  if (stop != "1" && stop != "2" && stop != "1.5")
    return false;

  t->dataBits = word[0] - '0';
  t->parity = p - parities;
  t->stopBits = stop == "1" ? deviceLinkStopBits1 :
                stop == "2" ? deviceLinkStopBits2 : deviceLinkStopBits15;
  return true;
}

bool DeviceLinkTarget::parse(const std::vector<std::string>& words,
                             DeviceLinkTarget* t, std::string* error)
{
  *t = DeviceLinkTarget();

  if (words.size() < 2) {
    *error = _("Expected a kind (serial, tcp, tail) and a device");
    return false;
  }
  const std::string& kind = words[0];
  t->where = words[1];

  if (kind == "tcp") {
    size_t port = t->where.rfind(':');
    if (port == std::string::npos || port == 0 ||
        port + 1 == t->where.size()) {
      *error = core::format(_("\"%s\" is not HOST:PORT"), t->where.c_str());
      return false;
    }
    if (words.size() > 2) {
      *error = core::format(_("Unknown option \"%s\""), words[2].c_str());
      return false;
    }
    t->kind = deviceLinkTCP;
    return true;
  }

  if (kind == "tail") {
    t->kind = deviceLinkTail;
    for (size_t i = 2; i < words.size(); i++) {
      if (words[i] == "from-start") {
        t->fromStart = true;
      } else {
        *error = core::format(_("Unknown option \"%s\""), words[i].c_str());
        return false;
      }
    }
    return true;
  }

  if (kind != "serial") {
    *error = core::format(_("Unknown kind \"%s\""), kind.c_str());
    return false;
  }

  t->kind = deviceLinkSerial;
  for (size_t i = 2; i < words.size(); i++) {
    const std::string& word = words[i];
    if (!word.empty() &&
        word.find_first_not_of("0123456789") == std::string::npos) {
      t->speed = strtoul(word.c_str(), nullptr, 10);
    } else if (parseFormat(word, t)) {
    } else if (word == "rtscts") {
      t->flow = deviceLinkFlowRTSCTS;
    } else if (word == "xonxoff") {
      t->flow = deviceLinkFlowXONXOFF;
    } else if (word == "noflow") {
      t->flow = deviceLinkFlowNone;
    } else if (word == "dtr=0" || word == "dtr=1") {
      t->dtr = word[4] - '0';
    } else if (word == "rts=0" || word == "rts=1") {
      t->rts = word[4] - '0';
    } else if (word == "fixed") {
      t->fixed = true;
    } else {
      *error = core::format(_("Unknown option \"%s\""), word.c_str());
      return false;
    }
  }
  return true;
}

std::vector<std::string> DeviceLinkTarget::toWords() const
{
  std::vector<std::string> words;

  switch (kind) {
  case deviceLinkTCP:
    return { "tcp", where };
  case deviceLinkTail:
    words = { "tail", where };
    if (fromStart)
      words.push_back("from-start");
    return words;
  }

  words = { "serial", where };
  if (speed)
    words.push_back(std::to_string(speed));
  if (dataBits) {
    std::string format(std::to_string(dataBits) + "NOEMS"[parity]);
    format += stopBits == deviceLinkStopBits2 ? "2" :
              stopBits == deviceLinkStopBits15 ? "1.5" : "1";
    words.push_back(format);
  }
  if (flow == deviceLinkFlowRTSCTS)
    words.push_back("rtscts");
  else if (flow == deviceLinkFlowXONXOFF)
    words.push_back("xonxoff");
  else if (flow == deviceLinkFlowNone)
    words.push_back("noflow");
  if (dtr >= 0)
    words.push_back("dtr=" + std::to_string(dtr));
  if (rts >= 0)
    words.push_back("rts=" + std::to_string(rts));
  if (fixed)
    words.push_back("fixed");
  return words;
}

std::string DeviceLinkTarget::toString() const
{
  std::string s;
  for (const std::string& word : toWords()) {
    if (!s.empty())
      s += " ";
    s += word;
  }
  return s;
}

#ifdef WIN32

static std::string lastError()
{
  char buf[256];
  DWORD err = GetLastError();
  if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                      buf, sizeof(buf), nullptr))
    return core::format("error %lu", (unsigned long)err);
  std::string s(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                        s.back() == ' ' || s.back() == '.'))
    s.pop_back();
  return s;
}

static std::wstring widen(const std::string& s)
{
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n : 1, L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(wcslen(w.c_str()));
  return w;
}

// -=- Serial port, Windows

class SerialPort : public DeviceLinkPort {
public:
  SerialPort(HANDLE h_) : h(h_) {
    memset(&readOv, 0, sizeof(readOv));
    memset(&writeOv, 0, sizeof(writeOv));
    readOv.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    writeOv.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  }
  ~SerialPort() {
    CancelIo(h);
    CloseHandle(h);
    CloseHandle(readOv.hEvent);
    CloseHandle(writeOv.hEvent);
  }

  bool init(const DeviceLinkTarget& t, std::string* error) {
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
      *error = lastError();
      return false;
    }
    dcb.fBinary = TRUE;
    dcb.fAbortOnError = FALSE;
    dcb.fNull = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = t.dtr == 0 ? DTR_CONTROL_DISABLE : DTR_CONTROL_ENABLE;
    dcb.fRtsControl = t.rts == 0 ? RTS_CONTROL_DISABLE : RTS_CONTROL_ENABLE;
    dcbParams(&dcb, (t.speed ? 1 : 0) | (t.dataBits ? 2 : 0) |
                    (t.flow >= 0 ? 4 : 0),
              t.speed, t.dataBits, t.parity, t.stopBits,
              t.flow >= 0 ? t.flow : deviceLinkFlowNone);
    if (!SetCommState(h, &dcb)) {
      *error = lastError();
      return false;
    }

    // ReadFile returns as soon as there is a byte, or after pollMs
    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = pollMs;
    SetCommTimeouts(h, &timeouts);
    return true;
  }

  int read(uint8_t* buf, size_t len, std::string* error) override {
    DWORD n = 0;
    ResetEvent(readOv.hEvent);
    if (!ReadFile(h, buf, len, &n, &readOv)) {
      if (GetLastError() != ERROR_IO_PENDING ||
          !GetOverlappedResult(h, &readOv, &n, TRUE)) {
        *error = lastError();
        return -1;
      }
    }
    return n;
  }

  bool write(const uint8_t* buf, size_t len, const std::atomic<bool>& stop,
             std::string* error) override {
    while (len > 0) {
      DWORD n = 0;
      ResetEvent(writeOv.hEvent);
      if (!WriteFile(h, buf, len, &n, &writeOv)) {
        if (GetLastError() != ERROR_IO_PENDING) {
          *error = lastError();
          return false;
        }
        // Flow control may hold the data back for a long time
        while (WaitForSingleObject(writeOv.hEvent, pollMs) == WAIT_TIMEOUT) {
          if (stop) {
            CancelIoEx(h, &writeOv);
            GetOverlappedResult(h, &writeOv, &n, TRUE);
            return true;
          }
        }
        if (!GetOverlappedResult(h, &writeOv, &n, FALSE)) {
          *error = lastError();
          return false;
        }
      }
      buf += n;
      len -= n;
    }
    return true;
  }

  void setParams(uint8_t mask, uint32_t speed, uint8_t dataBits,
                 uint8_t parity, uint8_t stopBits, uint8_t flow) override {
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb))
      return;
    dcbParams(&dcb, mask, speed, dataBits, parity, stopBits, flow);
    SetCommState(h, &dcb);
  }

  void setLines(uint8_t mask, uint8_t values) override {
    if (mask & deviceLinkDTR)
      EscapeCommFunction(h, values & deviceLinkDTR ? SETDTR : CLRDTR);
    if (mask & deviceLinkRTS)
      EscapeCommFunction(h, values & deviceLinkRTS ? SETRTS : CLRRTS);
  }

  void sendBreak(unsigned ms) override {
    SetCommBreak(h);
    Sleep(ms);
    ClearCommBreak(h);
  }

  void purge(uint8_t which) override {
    DWORD flags = 0;
    if (which & deviceLinkPurgeReceive)
      flags |= PURGE_RXCLEAR;
    if (which & deviceLinkPurgeTransmit)
      flags |= PURGE_TXCLEAR;
    PurgeComm(h, flags);
  }

  bool lineState(uint8_t* lines, uint8_t* errors) override {
    DWORD status = 0, errs = 0;
    if (!GetCommModemStatus(h, &status))
      return false;
    ClearCommError(h, &errs, nullptr);
    *lines = (status & MS_CTS_ON ? deviceLinkCTS : 0) |
             (status & MS_DSR_ON ? deviceLinkDSR : 0) |
             (status & MS_RING_ON ? deviceLinkRI : 0) |
             (status & MS_RLSD_ON ? deviceLinkDCD : 0);
    *errors = (errs & CE_FRAME ? deviceLinkErrFraming : 0) |
              (errs & CE_RXPARITY ? deviceLinkErrParity : 0) |
              (errs & (CE_OVERRUN | CE_RXOVER) ? deviceLinkErrOverrun : 0) |
              (errs & CE_BREAK ? deviceLinkErrBreak : 0);
    return true;
  }

private:
  static void dcbParams(DCB* dcb, uint8_t mask, uint32_t speed,
                        uint8_t dataBits, uint8_t parity, uint8_t stopBits,
                        uint8_t flow) {
    if ((mask & 1) && speed)
      dcb->BaudRate = speed;
    if ((mask & 2) && dataBits) {
      static const BYTE parities[] = { NOPARITY, ODDPARITY, EVENPARITY,
                                       MARKPARITY, SPACEPARITY };
      dcb->ByteSize = dataBits;
      dcb->Parity = parity < 5 ? parities[parity] : NOPARITY;
      dcb->fParity = dcb->Parity != NOPARITY;
      dcb->StopBits = stopBits == deviceLinkStopBits2 ? TWOSTOPBITS :
                      stopBits == deviceLinkStopBits15 ? ONE5STOPBITS :
                                                         ONESTOPBIT;
    }
    if (mask & 4) {
      dcb->fOutxCtsFlow = flow == deviceLinkFlowRTSCTS;
      if (flow == deviceLinkFlowRTSCTS)
        dcb->fRtsControl = RTS_CONTROL_HANDSHAKE;
      else if (dcb->fRtsControl == RTS_CONTROL_HANDSHAKE)
        dcb->fRtsControl = RTS_CONTROL_ENABLE;
      dcb->fOutX = dcb->fInX = flow == deviceLinkFlowXONXOFF;
      dcb->XonChar = 0x11;
      dcb->XoffChar = 0x13;
      dcb->XonLim = 512;
      dcb->XoffLim = 512;
    }
  }

  HANDLE h;
  OVERLAPPED readOv, writeOv;
};

static DeviceLinkPort* openSerial(const DeviceLinkTarget& t,
                                  std::string* error)
{
  std::string name(t.where);
  if (name.compare(0, 4, "\\\\.\\") != 0)
    name = "\\\\.\\" + name;

  HANDLE h = CreateFileW(widen(name).c_str(), GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *error = lastError();
    return nullptr;
  }

  SerialPort* port = new SerialPort(h);
  if (!port->init(t, error)) {
    delete port;
    return nullptr;
  }
  return port;
}

std::vector<std::string> DeviceLinkPort::serialPorts()
{
  std::vector<std::string> ports;
  HKEY key;

  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                    0, KEY_READ, &key) != ERROR_SUCCESS)
    return ports;

  for (DWORD i = 0; ; i++) {
    char name[256], value[256];
    DWORD nameLen = sizeof(name), valueLen = sizeof(value), type;
    if (RegEnumValueA(key, i, name, &nameLen, nullptr, &type,
                      (BYTE*)value, &valueLen) != ERROR_SUCCESS)
      break;
    if (type == REG_SZ && valueLen > 0) {
      value[std::min<DWORD>(valueLen, sizeof(value) - 1)] = '\0';
      ports.push_back(value);
    }
  }
  RegCloseKey(key);

  std::sort(ports.begin(), ports.end(), [](const std::string& a,
                                           const std::string& b) {
    if (a.size() != b.size())
      return a.size() < b.size();
    return a < b;
  });
  return ports;
}

// -=- Tail, Windows

class TailPort : public DeviceLinkPort {
public:
  TailPort(const std::string& path_, bool fromStart)
    : path(widen(path_)), h(INVALID_HANDLE_VALUE), offset(0) {
    reopen(!fromStart);
  }
  ~TailPort() {
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }

  int read(uint8_t* buf, size_t len, std::string* /*error*/) override {
    // A file that was replaced, or cut short, starts over
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs)) {
      uint64_t size = ((uint64_t)attrs.nFileSizeHigh << 32) |
                      attrs.nFileSizeLow;
      if (h == INVALID_HANDLE_VALUE || size < offset)
        reopen(false);
    }

    if (h != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER pos;
      DWORD n = 0;
      pos.QuadPart = offset;
      if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
          ReadFile(h, buf, len, &n, nullptr) && n > 0) {
        offset += n;
        return n;
      }
    }

    Sleep(pollMs);
    return 0;
  }

  bool write(const uint8_t*, size_t, const std::atomic<bool>&,
             std::string*) override {
    return true;
  }

private:
  void reopen(bool atEnd) {
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
    // Share everything, so that the program writing the file can go on
    // writing, rotating and deleting it
    h = CreateFileW(path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    offset = 0;
    LARGE_INTEGER size;
    if (atEnd && h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size))
      offset = size.QuadPart;
  }

  std::wstring path;
  HANDLE h;
  uint64_t offset;
};

#else // WIN32

static std::string lastError()
{
  return strerror(errno);
}

// -=- Serial port, POSIX

static const struct {
  uint32_t speed;
  speed_t code;
} speeds[] = {
  { 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 }, { 150, B150 },
  { 200, B200 }, { 300, B300 }, { 600, B600 }, { 1200, B1200 },
  { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
  { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 },
  { 115200, B115200 }, { 230400, B230400 },
#ifdef B460800
  { 460800, B460800 },
#endif
#ifdef B921600
  { 921600, B921600 },
#endif
};

class SerialPort : public DeviceLinkPort {
public:
  SerialPort(int fd_) : fd(fd_) {
#ifdef TIOCGICOUNT
    memset(&counts, 0, sizeof(counts));
    ioctl(fd, TIOCGICOUNT, &counts);
#endif
  }
  ~SerialPort() {
    // The exclusive mode would outlive us on some ports
    ioctl(fd, TIOCNXCL);
    close(fd);
  }

  bool init(const DeviceLinkTarget& t, std::string* error) {
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
      *error = lastError();
      return false;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    termiosParams(&tio, (t.speed ? 1 : 0) | (t.dataBits ? 2 : 0) |
                        (t.flow >= 0 ? 4 : 0),
                  t.speed, t.dataBits, t.parity, t.stopBits,
                  t.flow >= 0 ? t.flow : deviceLinkFlowNone);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
      *error = lastError();
      return false;
    }
    uint8_t mask = (t.dtr >= 0 ? deviceLinkDTR : 0) |
                   (t.rts >= 0 ? deviceLinkRTS : 0);
    uint8_t values = (t.dtr == 1 ? deviceLinkDTR : 0) |
                     (t.rts == 1 ? deviceLinkRTS : 0);
    setLines(mask, values);
    return true;
  }

  int read(uint8_t* buf, size_t len, std::string* error) override {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int r = poll(&pfd, 1, pollMs);
    if (r < 0)
      return errno == EINTR ? 0 : -1;
    if (r == 0)
      return 0;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
      *error = _("The device was removed");
      return -1;
    }
    ssize_t n = ::read(fd, buf, len);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR)
        return 0;
      *error = lastError();
      return -1;
    }
    return n;
  }

  bool write(const uint8_t* buf, size_t len, const std::atomic<bool>& stop,
             std::string* error) override {
    while (len > 0 && !stop) {
      struct pollfd pfd = { fd, POLLOUT, 0 };
      if (poll(&pfd, 1, pollMs) <= 0)
        continue;
      ssize_t n = ::write(fd, buf, len);
      if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
          continue;
        *error = lastError();
        return false;
      }
      buf += n;
      len -= n;
    }
    return true;
  }

  void setParams(uint8_t mask, uint32_t speed, uint8_t dataBits,
                 uint8_t parity, uint8_t stopBits, uint8_t flow) override {
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0)
      return;
    termiosParams(&tio, mask, speed, dataBits, parity, stopBits, flow);
    tcsetattr(fd, TCSADRAIN, &tio);
  }

  void setLines(uint8_t mask, uint8_t values) override {
    int set = 0, clear = 0;
    if (mask & deviceLinkDTR)
      (values & deviceLinkDTR ? set : clear) |= TIOCM_DTR;
    if (mask & deviceLinkRTS)
      (values & deviceLinkRTS ? set : clear) |= TIOCM_RTS;
    if (set)
      ioctl(fd, TIOCMBIS, &set);
    if (clear)
      ioctl(fd, TIOCMBIC, &clear);
  }

  void sendBreak(unsigned ms) override {
    tcdrain(fd);
    ioctl(fd, TIOCSBRK, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    ioctl(fd, TIOCCBRK, 0);
  }

  void purge(uint8_t which) override {
    if (which == (deviceLinkPurgeReceive | deviceLinkPurgeTransmit))
      tcflush(fd, TCIOFLUSH);
    else if (which & deviceLinkPurgeReceive)
      tcflush(fd, TCIFLUSH);
    else if (which & deviceLinkPurgeTransmit)
      tcflush(fd, TCOFLUSH);
  }

  bool lineState(uint8_t* lines, uint8_t* errors) override {
    int status;
    if (ioctl(fd, TIOCMGET, &status) != 0)
      return false;
    *lines = (status & TIOCM_CTS ? deviceLinkCTS : 0) |
             (status & TIOCM_DSR ? deviceLinkDSR : 0) |
             (status & TIOCM_RI ? deviceLinkRI : 0) |
             (status & TIOCM_CD ? deviceLinkDCD : 0);
    *errors = 0;
#ifdef TIOCGICOUNT
    struct serial_icounter_struct now;
    if (ioctl(fd, TIOCGICOUNT, &now) == 0) {
      *errors = (now.frame != counts.frame ? deviceLinkErrFraming : 0) |
                (now.parity != counts.parity ? deviceLinkErrParity : 0) |
                (now.overrun != counts.overrun ||
                 now.buf_overrun != counts.buf_overrun ?
                   deviceLinkErrOverrun : 0) |
                (now.brk != counts.brk ? deviceLinkErrBreak : 0);
      counts = now;
    }
#endif
    return true;
  }

private:
  static void termiosParams(struct termios* tio, uint8_t mask,
                            uint32_t speed, uint8_t dataBits,
                            uint8_t parity, uint8_t stopBits,
                            uint8_t flow) {
    if ((mask & 1) && speed) {
      for (const auto& s : speeds) {
        if (s.speed == speed) {
          cfsetispeed(tio, s.code);
          cfsetospeed(tio, s.code);
        }
      }
    }
    if ((mask & 2) && dataBits) {
      static const tcflag_t sizes[] = { CS5, CS6, CS7, CS8 };
      tio->c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
#ifdef CMSPAR
      tio->c_cflag &= ~CMSPAR;
#endif
      tio->c_cflag |= sizes[(dataBits - 5) & 3];
      if (parity != deviceLinkParityNone)
        tio->c_cflag |= PARENB;
      if (parity == deviceLinkParityOdd || parity == deviceLinkParityMark)
        tio->c_cflag |= PARODD;
#ifdef CMSPAR
      if (parity == deviceLinkParityMark || parity == deviceLinkParitySpace)
        tio->c_cflag |= CMSPAR;
#endif
      if (stopBits != deviceLinkStopBits1)
        tio->c_cflag |= CSTOPB;
    }
    if (mask & 4) {
      tio->c_cflag &= ~CRTSCTS;
      tio->c_iflag &= ~(IXON | IXOFF | IXANY);
      if (flow == deviceLinkFlowRTSCTS)
        tio->c_cflag |= CRTSCTS;
      else if (flow == deviceLinkFlowXONXOFF)
        tio->c_iflag |= IXON | IXOFF;
    }
  }

  int fd;
#ifdef TIOCGICOUNT
  struct serial_icounter_struct counts;
#endif
};

static DeviceLinkPort* openSerial(const DeviceLinkTarget& t,
                                  std::string* error)
{
  int fd = ::open(t.where.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    *error = lastError();
    return nullptr;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  // Like the exclusive access Windows gives
  ioctl(fd, TIOCEXCL);

  SerialPort* port = new SerialPort(fd);
  if (!port->init(t, error)) {
    delete port;
    return nullptr;
  }
  return port;
}

std::vector<std::string> DeviceLinkPort::serialPorts()
{
  std::vector<std::string> ports;
  static const char* patterns[] = {
#ifdef __APPLE__
    "/dev/cu.*",
#else
    "/dev/serial/by-id/*", "/dev/ttyUSB*", "/dev/ttyACM*",
    "/dev/ttyS[0-3]",
#endif
  };

  for (const char* pattern : patterns) {
    glob_t g;
    if (glob(pattern, 0, nullptr, &g) == 0) {
      for (size_t i = 0; i < g.gl_pathc; i++)
        ports.push_back(g.gl_pathv[i]);
    }
    globfree(&g);
  }
  return ports;
}

// -=- Tail, POSIX

class TailPort : public DeviceLinkPort {
public:
  TailPort(const std::string& path_, bool fromStart)
    : path(path_), fd(-1), ino(0), offset(0) {
    reopen(!fromStart);
  }
  ~TailPort() {
    if (fd >= 0)
      close(fd);
  }

  int read(uint8_t* buf, size_t len, std::string* /*error*/) override {
    // A file that was replaced, or cut short, starts over
    struct stat st;
    if (stat(path.c_str(), &st) == 0 &&
        (fd < 0 || st.st_ino != ino || (uint64_t)st.st_size < offset))
      reopen(false);

    if (fd >= 0) {
      ssize_t n = pread(fd, buf, len, offset);
      if (n > 0) {
        offset += n;
        return n;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    return 0;
  }

  bool write(const uint8_t*, size_t, const std::atomic<bool>&,
             std::string*) override {
    return true;
  }

private:
  void reopen(bool atEnd) {
    struct stat st;
    if (fd >= 0)
      close(fd);
    fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    offset = 0;
    ino = 0;
    if (fd >= 0 && fstat(fd, &st) == 0) {
      ino = st.st_ino;
      if (atEnd)
        offset = st.st_size;
    }
  }

  std::string path;
  int fd;
  ino_t ino;
  uint64_t offset;
};

#endif // WIN32

// -=- TCP

#ifdef WIN32
typedef SOCKET socket_t;
#define closesocket_ closesocket
static int socketError() { return WSAGetLastError(); }
static bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
static std::string socketErrorText(int e)
{
  SetLastError(e);
  return lastError();
}
#else
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define closesocket_ close
static int socketError() { return errno; }
static bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK ||
                                       e == EINPROGRESS || e == EINTR; }
static std::string socketErrorText(int e) { return strerror(e); }
#endif

static void setNonBlocking(socket_t s)
{
#ifdef WIN32
  u_long one = 1;
  ioctlsocket(s, FIONBIO, &one);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
  fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
}

// Waits for s to become readable or writable, at most ms
static int waitSocket(socket_t s, bool write, int ms)
{
  fd_set fds, efds;
  struct timeval tv;
  FD_ZERO(&fds);
  FD_ZERO(&efds);
  FD_SET(s, &fds);
  FD_SET(s, &efds);
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return select(s + 1, write ? nullptr : &fds, write ? &fds : nullptr,
                &efds, &tv);
}

class TcpPort : public DeviceLinkPort {
public:
  TcpPort(socket_t s_) : s(s_) {}
  ~TcpPort() { closesocket_(s); }

  int read(uint8_t* buf, size_t len, std::string* error) override {
    int r = waitSocket(s, false, pollMs);
    if (r <= 0)
      return 0;
    int n = recv(s, (char*)buf, len, 0);
    if (n == 0) {
      *error = _("The connection was closed");
      return -1;
    }
    if (n < 0) {
      int e = socketError();
      if (wouldBlock(e))
        return 0;
      *error = socketErrorText(e);
      return -1;
    }
    return n;
  }

  bool write(const uint8_t* buf, size_t len, const std::atomic<bool>& stop,
             std::string* error) override {
    while (len > 0 && !stop) {
      if (waitSocket(s, true, pollMs) <= 0)
        continue;
#ifdef MSG_NOSIGNAL
      int n = send(s, (const char*)buf, len, MSG_NOSIGNAL);
#else
      int n = send(s, (const char*)buf, len, 0);
#endif
      if (n < 0) {
        int e = socketError();
        if (wouldBlock(e))
          continue;
        *error = socketErrorText(e);
        return false;
      }
      buf += n;
      len -= n;
    }
    return true;
  }

private:
  socket_t s;
};

static DeviceLinkPort* openTcp(const DeviceLinkTarget& t,
                               std::string* error)
{
  size_t colon = t.where.rfind(':');
  std::string host(t.where.substr(0, colon));
  std::string port(t.where.substr(colon + 1));
  if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);

  struct addrinfo hints, *ai;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int r = getaddrinfo(host.c_str(), port.c_str(), &hints, &ai);
  if (r != 0) {
    *error = core::format(_("Cannot resolve %s: %s"), host.c_str(),
                          gai_strerror(r));
    return nullptr;
  }

  socket_t s = INVALID_SOCKET;
  for (struct addrinfo* a = ai; a != nullptr; a = a->ai_next) {
    s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == INVALID_SOCKET)
      continue;
    setNonBlocking(s);
    if (connect(s, a->ai_addr, a->ai_addrlen) == 0)
      break;
    int e = socketError();
    if (wouldBlock(e) && waitSocket(s, true, 5000) > 0) {
      int err = 0;
      socklen_t errLen = sizeof(err);
      getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &errLen);
      if (err == 0)
        break;
      e = err;
    } else if (wouldBlock(e)) {
#ifdef WIN32
      e = WSAETIMEDOUT;
#else
      e = ETIMEDOUT;
#endif
    }
    *error = socketErrorText(e);
    closesocket_(s);
    s = INVALID_SOCKET;
  }
  freeaddrinfo(ai);

  if (s == INVALID_SOCKET)
    return nullptr;

  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
  return new TcpPort(s);
}

// -=- DeviceLinkPort

DeviceLinkPort* DeviceLinkPort::open(const DeviceLinkTarget& target,
                                     std::string* error)
{
  switch (target.kind) {
  case deviceLinkSerial:
    return openSerial(target, error);
  case deviceLinkTCP:
    return openTcp(target, error);
  case deviceLinkTail:
    return new TailPort(target.where, target.fromStart);
  }
  *error = _("Unknown kind of device");
  return nullptr;
}
