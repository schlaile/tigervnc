#!/usr/bin/env python3
#
# Two sessions share one device of the viewer's computer
# (doc/device-link.md): two Xvnc, each with a vncviewer on the same
# computer whose profiles lead to the same serial port. One session
# holds the card reader during a sale; the other waits for it.
#
#   tests/device-link/test-sharing.py PATH-TO-Xvnc PATH-TO-vncviewer
#

import os
import pty
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

failures = 0
here = os.path.dirname(os.path.abspath(__file__))
vnc_device = os.path.join(here, "..", "..", "contrib", "device-link",
                          "vnc-device")


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
            data += os.read(fd, 65536)
    return data


class App:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.buf = b""

    def line(self, timeout=10):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.time()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                return None
            self.buf += self.sock.recv(4096)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode()

    def cmd(self, text):
        self.sock.sendall(text.encode() + b"\n")


def main():
    xvnc, viewer = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="device-link-sharing-")
    procs = []
    log = open(os.path.join(tmp, "log.txt"), "w")
    try:
        # The viewer's computer: a screen, a card reader, the profiles
        procs.append(subprocess.Popen(
            [xvnc, ":48", "-geometry", "800x600", "-SecurityTypes", "None",
             "-localhost", "-rfbport", "5948"], stdout=log, stderr=log))
        devfd, slave = pty.openpty()
        port = os.ttyname(slave)
        profiles = os.path.join(tmp, "device-links")
        with open(profiles, "w") as f:
            f.write("[kasse]\nask never\ninsecure yes\n"
                    "cardreader serial %s 9600 8N1\n" % port)
        env = dict(os.environ, DISPLAY=":48", HOME=tmp,
                   XDG_STATE_HOME=os.path.join(tmp, "state"))

        # Two sessions, each with its viewer
        apps = []
        dirs = []
        for n in (45, 46):
            d = os.path.join(tmp, "dl%d" % n)
            dirs.append(d)
            procs.append(subprocess.Popen(
                [xvnc, ":%d" % n, "-geometry", "320x200", "-SecurityTypes",
                 "None", "-localhost", "-rfbport", str(5900 + n),
                 "-DeviceLink=1", "-DeviceLinkDir", d],
                stdout=log, stderr=log))
            wait_for(os.path.join(d, "control"))
            app = App(os.path.join(d, "control"))
            app.line()
            procs.append(subprocess.Popen(
                [viewer, "-DeviceLinkProfile", "kasse", "-DeviceLinksFile",
                 profiles, "-Log", "*:stderr:100",
                 "127.0.0.1::%d" % (5900 + n)],
                env=env, stdout=log, stderr=log))
            check(app.line() == "viewer present", "session :%d connected" % n)
            apps.append(app)
        os.close(slave)
        a, b = apps

        # A takes the card reader for a sale
        a.cmd("request cardreader serial Kartenleser")
        reply = a.line()
        check(reply.startswith("ok cardreader "), "A has it: %s" % reply)
        tty_a = os.open(reply.split(" ", 2)[2], os.O_RDWR | os.O_NOCTTY)

        # B without waiting is told who has it
        b.cmd("request cardreader serial Kartenleser")
        reply = b.line()
        check(reply is not None and
              reply.startswith("error cardreader busy") and
              "127.0.0.1::5945" in reply,
              "B is told who has it: %s" % reply)

        # B waits; A finishes the sale
        b.cmd("request cardreader serial wait=20 Kartenleser")
        check(b.line(1.5) is None, "B waits while A sells")
        os.write(devfd, b"%A?;1234?+?")
        check(read_all(tty_a, 11) == b"%A?;1234?+?", "card goes to A")
        os.close(tty_a)
        a.cmd("release cardreader")
        a.line()
        reply = b.line()
        check(reply is not None and reply.startswith("ok cardreader "),
              "B gets it after A released: %s" % reply)
        tty_b = os.open(reply.split(" ", 2)[2], os.O_RDWR | os.O_NOCTTY)
        os.write(devfd, b"%B?;5678?+?")
        check(read_all(tty_b, 11) == b"%B?;5678?+?", "next card goes to B")
        os.close(tty_b)

        # A waiting with a time limit gives up
        a.cmd("request cardreader serial wait=1 Kartenleser")
        reply = a.line(5)
        check(reply is not None and reply.startswith("error cardreader busy"),
              "waiting ends after its time: %s" % reply)
        b.cmd("release cardreader")
        b.line()

        # vnc-device holds the device while its command runs
        out = os.path.join(tmp, "out")
        r = subprocess.run(
            [sys.executable, vnc_device, "--dir", dirs[0], "--wait", "5",
             "--label", "Kartenleser", "cardreader", "serial", "--",
             "sh", "-c", "printf R > {}; echo {} > " + out],
            timeout=20)
        check(r.returncode == 0, "vnc-device runs the command")
        check(read_all(devfd, 1) == b"R", "the command wrote to the device")
        check(open(out).read().startswith(dirs[0]), "{} is the device path")
        b.cmd("request cardreader serial Kartenleser")
        reply = b.line()
        check(reply is not None and reply.startswith("ok cardreader "),
              "released when the command ended: %s" % reply)
    finally:
        for p in reversed(procs):
            p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        if failures:
            print(open(os.path.join(tmp, "log.txt")).read()[-5000:])
        shutil.rmtree(tmp)
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
