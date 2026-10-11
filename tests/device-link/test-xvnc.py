#!/usr/bin/env python3
#
# Exercises the device links of Xvnc (doc/device-link.md) with a minimal
# RFB client in place of the viewer and an application on the control
# socket.
#
#   tests/device-link/test-xvnc.py PATH-TO-Xvnc
#

import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

MSG_DEVICE_LINK = 200
PSEUDO_DEVICE_LINK = 0x44564C31

HELLO, REQUEST, REQUEST_RESULT, DATA, CREDIT, SET_PARAMS, SET_LINES, \
    LINE_STATE, PURGE, CLOSE, LOCK, LOCK_RESULT, UNLOCK = range(13)

failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1


class Viewer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.buf = b""
        version = self.read(12)
        self.sock.sendall(version)
        n = self.read(1)[0]
        types = self.read(n)
        assert 1 in types, types
        self.sock.sendall(b"\x01")
        assert struct.unpack(">I", self.read(4))[0] == 0
        self.sock.sendall(b"\x01")
        self.read(2 + 2 + 16)
        name_len = struct.unpack(">I", self.read(4))[0]
        self.read(name_len)
        encodings = [0, PSEUDO_DEVICE_LINK]
        self.sock.sendall(struct.pack(">BxH", 2, len(encodings)) +
                          b"".join(struct.pack(">i", e) for e in encodings))

    def read(self, n, timeout=5):
        deadline = time.time() + timeout
        while len(self.buf) < n:
            left = deadline - time.time()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                raise TimeoutError("viewer read")
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("viewer")
            self.buf += data
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def msg(self, timeout=5):
        t = self.read(1, timeout)[0]
        assert t == MSG_DEVICE_LINK, "unexpected message %d" % t
        sub, chan, length = struct.unpack(">BHI", self.read(7, timeout))
        return sub, chan, self.read(length, timeout)

    def expect(self, subtype, timeout=5):
        m = self.msg(timeout)
        assert m[0] == subtype, "expected %d, got %r" % (subtype, m)
        return m

    def nothing(self, timeout=0.3):
        try:
            m = self.msg(timeout)
            return m
        except TimeoutError:
            return None

    def send(self, sub, chan, payload=b""):
        self.sock.sendall(struct.pack(">BBHI", MSG_DEVICE_LINK, sub, chan,
                                      len(payload)) + payload)


class App:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.f = self.sock.makefile("rw", buffering=1)

    def line(self, timeout=5):
        self.sock.settimeout(timeout)
        return self.f.readline().rstrip("\n")

    def cmd(self, text):
        self.f.write(text + "\n")
        self.f.flush()


def main():
    xvnc = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="device-link-test-")
    devdir = os.path.join(tmp, "dl")
    display = "47"
    port = 5947
    proc = subprocess.Popen(
        [xvnc, ":" + display, "-geometry", "320x200", "-SecurityTypes",
         "None", "-localhost", "-rfbport", str(port), "-DeviceLink=1",
         "-DeviceLinkDir", devdir, "-DeviceLinkChannels", "3"],
        stdout=open(os.path.join(tmp, "xvnc.log"), "w"),
        stderr=subprocess.STDOUT)
    try:
        for _ in range(50):
            if os.path.exists(os.path.join(devdir, "control")):
                break
            time.sleep(0.1)
        run(port, devdir)
    finally:
        proc.terminate()
        proc.wait()
        if failures:
            print(open(os.path.join(tmp, "xvnc.log")).read()[-3000:])
        shutil.rmtree(tmp)
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


def run(port, devdir):
    control = os.path.join(devdir, "control")

    app = App(control)
    check(app.line() == "viewer absent", "no viewer at first")
    app.cmd("request display serial Kundendisplay")
    check(app.line() == "error display no-viewer", "request without viewer")

    viewer = Viewer(port)
    sub, chan, payload = viewer.expect(HELLO)
    check(payload == bytes([1, 0, 0, 3]), "hello with version and channels")
    check(app.line() == "viewer present", "viewer present event")

    # -- serial
    app.cmd("request display serial Kundendisplay")
    sub, chan, payload = viewer.expect(REQUEST)
    check(payload == bytes([1, 7]) + b"display" + bytes([13]) +
          b"Kundendisplay" + bytes([0]),
          "request carries kind, name, label, flags")
    display = chan
    viewer.send(REQUEST_RESULT, display, bytes([0, 0, 0]))
    reply = app.line()
    check(reply.startswith("ok display " + devdir), "ok with path: " + reply)
    path = reply.split(" ", 2)[2]
    check(os.readlink(path).startswith("/dev/pts/"), "path is a pty")

    tty = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(tty)
    attrs[3] = 0                # c_lflag = 0, as many programs do
    attrs[4] = attrs[5] = termios.B9600
    termios.tcsetattr(tty, termios.TCSANOW, attrs)
    sub, chan, payload = viewer.expect(SET_PARAMS)
    mask, speed = struct.unpack(">BI", payload[:5])
    check(mask == 1 and speed == 9600, "speed 9600 reported")

    attrs = termios.tcgetattr(tty)
    attrs[4] = attrs[5] = termios.B19200
    attrs[2] |= termios.CRTSCTS
    termios.tcsetattr(tty, termios.TCSANOW, attrs)
    sub, chan, payload = viewer.expect(SET_PARAMS)
    mask, speed = struct.unpack(">BI", payload[:5])
    check(mask == 5 and speed == 19200 and payload[8] == 1,
          "second change still reported (EXTPROC restored)")

    termios.tcsetattr(tty, termios.TCSANOW, termios.tcgetattr(tty))
    check(viewer.nothing() is None, "unchanged settings not reported")

    os.write(tty, b"hello")
    sub, chan, payload = viewer.expect(DATA)
    check(payload == b"hello", "application to device")
    viewer.send(CREDIT, display, struct.pack(">I", len(payload)))

    viewer.send(DATA, display, b"world")
    r = select.select([tty], [], [], 2)[0]
    check(r and os.read(tty, 100) == b"world", "device to application")
    sub, chan, payload = viewer.expect(CREDIT)
    check(struct.unpack(">I", payload)[0] == 5, "credit returned")

    app.cmd("format display 7E1")
    check(app.line() == "ok display", "format accepted")
    sub, chan, payload = viewer.expect(SET_PARAMS)
    check(payload[0] == 2 and payload[5:8] == bytes([7, 2, 1]),
          "format 7E1 sent")

    app.cmd("lines display dtr=1 rts=0")
    check(app.line() == "ok display", "lines accepted")
    sub, chan, payload = viewer.expect(SET_LINES)
    check(payload == bytes([3, 1, 0, 0]), "SetLines dtr=1 rts=0")

    app.cmd("break display 250")
    app.line()
    sub, chan, payload = viewer.expect(SET_LINES)
    check(payload == bytes([0, 0, 0, 250]), "break 250 ms")

    viewer.send(LINE_STATE, display, bytes([1 | 8, 1]))
    check(app.line() == "lines display cts=1 dsr=0 ri=0 dcd=1",
          "line state event")
    check(app.line() == "error display framing", "framing error event")

    termios.tcflush(tty, termios.TCIFLUSH)
    sub, chan, payload = viewer.expect(PURGE)
    check(payload == bytes([1]), "tcflush becomes purge receive")

    # -- flow control: the viewer gives no credit
    big = b"x" * 100000
    written = 0
    os.set_blocking(tty, False)
    received = 0
    deadline = time.time() + 3
    while time.time() < deadline:
        if written < len(big):
            try:
                written += os.write(tty, big[written:written + 4096])
            except BlockingIOError:
                pass
        m = viewer.nothing(0.05)
        if m:
            if m[0] != DATA:
                check(False, "only data during flood: %r" % (m,))
            received += len(m[2])
    check(received == 65536, "window stops at 65536 (got %d)" % received)
    viewer.send(CREDIT, display, struct.pack(">I", 100000))
    deadline = time.time() + 3
    while received < 100000 and time.time() < deadline:
        if written < len(big):
            try:
                written += os.write(tty, big[written:written + 4096])
            except BlockingIOError:
                pass
        m = viewer.nothing(0.05)
        if m:
            received += len(m[2])
    check(received == 100000, "rest arrives after credit")

    # -- refused tcp
    app.cmd("request payment tcp ZVT-Terminal")
    sub, chan, payload = viewer.expect(REQUEST)
    viewer.send(REQUEST_RESULT, chan, bytes([1, 0, 4]) + b"nope")
    check(app.line() == "error payment refused nope", "refused request")

    # -- tail
    app.cmd("request cts tail CTS Sales")
    sub, chan, payload = viewer.expect(REQUEST)
    cts = chan
    viewer.send(REQUEST_RESULT, cts, bytes([0, 0, 0]))
    reply = app.line()
    check(reply == "ok cts " + os.path.join(devdir, "cts"), "tail open")
    viewer.send(DATA, cts, b"line 1\n")
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(devdir, "cts"))
    s.settimeout(2)
    check(s.recv(100) == b"line 1\n", "buffered tail data reaches app")
    sub, chan, payload = viewer.expect(CREDIT)
    s.sendall(b"ignored")
    check(viewer.nothing() is None, "tail is read-only")

    # -- channel limit (display, cts open; limit 3)
    app.cmd("request a serial A")
    viewer.expect(REQUEST)
    app.cmd("request b serial B")
    check(app.line() == "error b too-many", "channel limit")

    # -- a second application cannot take a requested name
    app2 = App(control)
    app2.line()
    app2.cmd("request display serial X")
    check(app2.line() == "error display busy", "name is busy")
    app2.cmd("lines display dtr=1")
    check(app2.line() == "error display not-open", "foreign device")

    # -- release
    # (Xvnc confirms once the viewer has closed the device)
    app.cmd("release display")
    sub, chan, payload = viewer.expect(CLOSE)
    check(chan == display, "close sent for released device")
    check(not os.path.lexists(path), "path removed")
    viewer.send(CLOSE, display, bytes([0]))
    check(app.line() == "ok display", "release confirmed after the viewer")

    # -- locks: any application of the session may lock a requested
    #    device; the viewer decides
    app.cmd("request scanner serial Scanner")
    sub, scanner, payload = viewer.expect(REQUEST)
    viewer.send(REQUEST_RESULT, scanner, bytes([0, 0, 0]))
    app.line()
    app2.cmd("lock scanner wait=30")
    sub, chan, payload = viewer.expect(LOCK)
    check(chan == scanner and payload == bytes([1, 0, 30]),
          "lock with wait=30 reaches the viewer")
    app.cmd("lock scanner")
    check(app.line() == "error scanner busy this session",
          "one lock per session")
    viewer.send(LOCK_RESULT, scanner, bytes([0, 0, 0]))
    check(app2.line() == "ok scanner", "locked")
    app.cmd("unlock scanner")
    check(app.line() == "error scanner not-locked", "only the locker unlocks")
    app2.cmd("unlock scanner")
    check(app2.line() == "ok scanner", "unlocked")
    viewer.expect(UNLOCK)
    app2.cmd("lock scanner")
    sub, chan, payload = viewer.expect(LOCK)
    check(payload == bytes([0, 0, 0]), "lock without waiting")
    viewer.send(LOCK_RESULT, scanner, bytes([3, 0, 7]) + b"kasse:2")
    check(app2.line() == "error scanner busy kasse:2", "busy with holder")
    app2.cmd("lock scanner")
    viewer.expect(LOCK)
    viewer.send(LOCK_RESULT, scanner, bytes([0, 0, 0]))
    app2.line()
    app2.f.close()
    app2.sock.close()
    sub, chan, payload = viewer.expect(UNLOCK)
    check(chan == scanner, "lock ends with its application")
    app2 = App(control)
    app2.line()
    app.cmd("release scanner")
    viewer.expect(CLOSE)
    viewer.send(CLOSE, scanner, bytes([0]))
    check(app.line() == "ok scanner", "scanner released")

    # -- viewer closes a device
    viewer.send(CLOSE, cts, bytes([1]))
    check(app.line() == "lost cts", "lost after viewer close")
    viewer.expect(CLOSE)

    # -- the owner leaving takes the devices away; the next viewer
    #    becomes the owner
    viewer.sock.close()
    check(app.line() == "error a no-viewer", "pending request fails")
    check(app.line() == "viewer absent", "viewer absent event")
    check(app2.line() == "viewer absent", "broadcast to all apps")

    viewer = Viewer(port)
    viewer.expect(HELLO)
    check(app.line() == "viewer present", "new owner")

    # -- application disconnect releases its devices
    app.cmd("request display serial Kundendisplay")
    sub, chan, payload = viewer.expect(REQUEST)
    viewer.send(REQUEST_RESULT, chan, bytes([0, 0, 0]))
    app.line()
    app.f.close()
    app.sock.close()
    sub, chan2, payload = viewer.expect(CLOSE)
    check(chan2 == chan, "disconnect releases devices")


if __name__ == "__main__":
    main()
