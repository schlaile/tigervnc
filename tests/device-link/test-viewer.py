#!/usr/bin/env python3
#
# End-to-end test of device links (doc/device-link.md): Xvnc with
# -DeviceLink, vncviewer (shown on a second Xvnc) with a device profile
# for a serial port (a pseudo terminal), a TCP echo server and a growing
# file, and an application on the control socket of the first Xvnc.
#
#   tests/device-link/test-viewer.py PATH-TO-Xvnc PATH-TO-vncviewer
#

import os
import pty
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import termios
import threading
import time

failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1


def wait_for(path, timeout=10):
    deadline = time.time() + timeout
    while not os.path.exists(path):
        if time.time() > deadline:
            raise TimeoutError(path)
        time.sleep(0.05)


def read_all(fd, want, timeout=5):
    data = b""
    deadline = time.time() + timeout
    while len(data) < want and time.time() < deadline:
        if select.select([fd], [], [], 0.1)[0]:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            data += chunk
    return data


class App:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.f = self.sock.makefile("rw", buffering=1)

    def line(self, timeout=10):
        self.sock.settimeout(timeout)
        return self.f.readline().rstrip("\n")

    def cmd(self, text):
        self.f.write(text + "\n")
        self.f.flush()

    def until(self, prefix, timeout=10):
        while True:
            line = self.line(timeout)
            if line.startswith(prefix) or not line:
                return line


def tty_raw(a):
    a[0] = 0
    a[1] = 0
    a[3] = 0
    a[6][termios.VMIN] = 1
    a[6][termios.VTIME] = 0


def echo_server(sock):
    while True:
        try:
            conn, _ = sock.accept()
        except OSError:
            return
        def serve(c):
            while True:
                data = c.recv(4096)
                if not data:
                    return
                c.sendall(b"echo:" + data)
        threading.Thread(target=serve, args=(conn,), daemon=True).start()


def main():
    xvnc, viewer = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="device-link-viewer-")
    procs = []
    try:
        devdir = os.path.join(tmp, "dl")
        log = open(os.path.join(tmp, "log.txt"), "w")

        # The session
        procs.append(subprocess.Popen(
            [xvnc, ":47", "-geometry", "320x200", "-SecurityTypes", "None",
             "-localhost", "-rfbport", "5947", "-DeviceLink=1",
             "-DeviceLinkDir", devdir], stdout=log, stderr=log))
        # The screen of the viewer
        procs.append(subprocess.Popen(
            [xvnc, ":48", "-geometry", "800x600", "-SecurityTypes", "None",
             "-localhost", "-rfbport", "5948"], stdout=log, stderr=log))

        # The serial port of the viewer's computer: the viewer opens the
        # slave of a pseudo terminal, the test plays the device on the
        # master, which also sees the settings of the slave
        devfd, slave = pty.openpty()
        port = os.ttyname(slave)
        attrs = termios.tcgetattr(slave)
        tty_raw(attrs)
        termios.tcsetattr(slave, termios.TCSANOW, attrs)

        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(4)
        threading.Thread(target=echo_server, args=(srv,),
                         daemon=True).start()

        tail = os.path.join(tmp, "sales.log")
        with open(tail, "w") as f:
            f.write("old line\n")

        wait_for(os.path.join(devdir, "control"))
        app = App(os.path.join(devdir, "control"))
        check(app.line() == "viewer absent", "no viewer yet")

        profiles = os.path.join(tmp, "device-links")
        with open(profiles, "w") as f:
            f.write("""# Test profiles
[kasse]
description  Kasse Test
server       127.0.0.1::59*
ask          once
insecure     yes
display      serial  %s   9600 7E1     # Kundendisplay
payment      tcp     127.0.0.1:%d
cts          tail    "%s"

[anderswo]
server       other.example.com:*
insecure     yes
display      serial  %s
""" % (port, srv.getsockname()[1], tail, port))

        env = dict(os.environ, DISPLAY=":48", HOME=tmp)

        def start_viewer(profile):
            p = subprocess.Popen(
                [viewer, "-DeviceLinkProfile", profile, "-DeviceLinksFile",
                 profiles, "-Log", "*:stderr:100", "127.0.0.1::5947"],
                env=env, stdout=log, stderr=log)
            procs.append(p)
            return p

        # A profile for another server: no device links at all
        other = start_viewer("anderswo")
        try:
            line = app.line(3)
        except (socket.timeout, TimeoutError):
            line = ""
        check(line == "", "profile for another server offers nothing")
        other.terminate()
        other.wait()
        # (a timed out file object cannot be read any more)
        app.f.close()
        app.sock.close()
        app = App(os.path.join(devdir, "control"))
        app.line()

        start_viewer("kasse")
        check(app.line() == "viewer present", "viewer connected")
        os.close(slave)
        run(app, devfd, tail, devdir, env, profiles)
    finally:
        for p in reversed(procs):
            p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        if failures:
            print(open(os.path.join(tmp, "log.txt")).read()[-6000:])
        if os.environ.get("KEEP"):
            print("kept", tmp)
        else:
            shutil.rmtree(tmp)
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


def dialog(env, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = subprocess.run(["xdotool", "search", "--onlyvisible", "--name",
                            "Device for the remote desktop"],
                           env=env, capture_output=True, text=True)
        if r.stdout.split():
            return r.stdout.split()[0]
        time.sleep(0.1)
    return None


def click(env, win, x, y):
    subprocess.run(["xdotool", "mousemove", "--window", win, str(x), str(y),
                    "click", "1"], env=env)


def allow(env):
    win = dialog(env)
    if win:
        click(env, win, 250 + 47, 110 + 14)     # "Allow"
    return win is not None


def run(app, devfd, tail, devdir, env, profiles):
    # -- serial, confirmed by the user (DeviceLinkOpen=Once)
    app.cmd("request display serial Kundendisplay")
    check(allow(env), "the user is asked")
    reply = app.line()
    check(reply.startswith("ok display "), "serial open: " + reply)
    path = reply.split(" ", 2)[2]
    tty = os.open(path, os.O_RDWR | os.O_NOCTTY)

    os.write(tty, b"Summe 12,50 EUR")
    check(read_all(devfd, 15) == b"Summe 12,50 EUR",
          "application -> serial port")

    os.write(devfd, b"\x02card 4711\x03")
    check(read_all(tty, 11) == b"\x02card 4711\x03",
          "serial port -> application")

    # The viewer configured 7E1 at 9600 when it opened the port
    attrs = termios.tcgetattr(devfd)
    # (a pseudo terminal keeps the speed but forces 8N1, so only the
    # speed can be seen here)
    check(attrs[4] == termios.B9600, "port opened with the configured speed")

    # The application changes the speed
    a = termios.tcgetattr(tty)
    a[4] = a[5] = termios.B19200
    termios.tcsetattr(tty, termios.TCSANOW, a)
    deadline = time.time() + 3
    while time.time() < deadline:
        attrs = termios.tcgetattr(devfd)
        if attrs[4] == termios.B19200:
            break
        time.sleep(0.05)
    check(attrs[4] == termios.B19200, "speed change reaches the port")

    big = bytes(range(256)) * 400
    threading.Thread(target=lambda: os.write(tty, big), daemon=True).start()
    got = read_all(devfd, len(big), 20)
    check(got == big, "100 KiB in order through the window (%d)" % len(got))

    # -- tcp
    app.cmd("request payment tcp ZVT")
    allow(env)
    reply = app.line()
    check(reply.startswith("ok payment "), "tcp open: " + reply)
    s = socket.socket(socket.AF_UNIX)
    s.connect(reply.split(" ", 2)[2])
    s.settimeout(5)
    s.sendall(b"\x06\x01")
    check(s.recv(100) == b"echo:\x06\x01", "tcp round trip")

    # -- tail: only what is written after opening
    app.cmd("request cts tail CTS Sales")
    allow(env)
    reply = app.line()
    check(reply.startswith("ok cts "), "tail open: " + reply)
    t = socket.socket(socket.AF_UNIX)
    t.connect(reply.split(" ", 2)[2])
    t.settimeout(5)
    time.sleep(0.3)
    with open(tail, "a") as f:
        f.write("Ticket 1 verkauft\n")
    check(t.recv(100) == b"Ticket 1 verkauft\n", "tail follows the file")

    # Rotation: a new, shorter file starts over
    os.rename(tail, tail + ".1")
    with open(tail, "w") as f:
        f.write("neu\n")
    check(t.recv(100) == b"neu\n", "tail follows a rotated file")

    # -- kind mismatch
    app.cmd("request payment serial X")
    check(app.line() == "error payment busy", "name in use")
    app2 = App(os.path.join(devdir, "control"))
    app2.line()
    app2.cmd("request cts serial X")
    check(app2.line() == "error cts busy", "busy for other applications")

    # -- not configured: the user chooses, and the choice is remembered
    #    in the profile
    scanfd, scanslave = pty.openpty()
    scanport = os.ttyname(scanslave)
    os.close(scanslave)
    app2.cmd("request scanner serial Scanner")
    win = dialog(env)
    check(win is not None, "chooser for an unconfigured name")
    if win:
        click(env, win, 160 + 150, 85 + 12)          # the input field
        subprocess.run(["xdotool", "key", "ctrl+a"], env=env)
        subprocess.run(["xdotool", "type", scanport], env=env)
        click(env, win, 270 + 47, 160 + 14)          # "Use"
    reply = app2.line()
    check(reply.startswith("ok scanner "), "chosen port opened: " + reply)
    text = open(profiles).read()
    kasse = text.split("[anderswo]")[0]
    check("scanner      serial %s" % scanport in kasse,
          "choice remembered in the profile")
    os.close(scanfd)
    app2.cmd("release scanner")
    app2.line()

    # -- the server withdrawing a request closes the dialog
    app2.cmd("request scanner2 serial Scanner")
    win = dialog(env)
    check(win is not None, "chooser for an unconfigured name")
    app2.f.close()
    app2.sock.close()
    deadline = time.time() + 5
    while dialog(env, 0.2) and time.time() < deadline:
        pass
    check(dialog(env, 0.5) is None, "withdrawn request closes the dialog")

    # -- release, and the port is free again; Once does not ask again
    app.cmd("release display")
    check(app.line() == "ok display", "release")
    os.close(tty)
    app.cmd("request display serial Kundendisplay")
    reply = app.line()
    check(reply.startswith("ok display "),
          "reopen without asking again: " + reply)

    os.close(devfd)


if __name__ == "__main__":
    main()
