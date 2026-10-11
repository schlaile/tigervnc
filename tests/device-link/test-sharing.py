#!/usr/bin/env python3
#
# Two sessions share one device of the viewer's computer
# (doc/device-link.md): two Xvnc, each with a vncviewer on the same
# computer whose profiles lead to the same serial port. The port is
# opened once; cards go to the session with the lock, else to the viewer
# with the focus; a session locks the card reader during a sale.
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
        viewers = []
        for n in (45, 46):
            d = os.path.join(tmp, "dl%d" % n)
            dirs.append(d)
            procs.append(subprocess.Popen(
                [xvnc, ":%d" % n, "-geometry", "320x200", "-SecurityTypes",
                 "None", "-localhost", "-rfbport", str(5900 + n),
                 "-desktop", "kasse%d" % (n - 44),
                 "-DeviceLink=1", "-DeviceLinkDir", d],
                stdout=log, stderr=log))
            wait_for(os.path.join(d, "control"))
            app = App(os.path.join(d, "control"))
            app.line()
            v = subprocess.Popen(
                [viewer, "-DeviceLinkProfile", "kasse", "-DeviceLinksFile",
                 profiles, "-Log", "*:stderr:100",
                 "127.0.0.1::%d" % (5900 + n)],
                env=env, stdout=log, stderr=log)
            procs.append(v)
            viewers.append(v)
            check(app.line() == "viewer present", "session :%d connected" % n)
            apps.append(app)
        os.close(slave)
        a, b = apps

        def focus(n):
            r = subprocess.run(["xdotool", "search", "--name", "kasse%d" % n],
                               env=env, capture_output=True, text=True)
            for w in r.stdout.split():
                subprocess.run(["xdotool", "windowactivate", "--sync", w],
                               env=env, capture_output=True)
                subprocess.run(["xdotool", "windowfocus", "--sync", w],
                               env=env, capture_output=True)
            time.sleep(0.5)

        # Both sessions open the card reader; the device is opened once
        # (the first viewer holds it exclusively, the second shares it)
        a.cmd("request cardreader serial Kartenleser")
        reply = a.line()
        check(reply.startswith("ok cardreader "), "A has it: %s" % reply)
        tty_a = os.open(reply.split(" ", 2)[2], os.O_RDWR | os.O_NOCTTY)
        time.sleep(0.5)
        b.cmd("request cardreader serial Kartenleser")
        reply = b.line()
        check(reply is not None and reply.startswith("ok cardreader "),
              "B shares it: %s" % reply)
        tty_b = os.open(reply.split(" ", 2)[2], os.O_RDWR | os.O_NOCTTY)

        def card(text):
            os.write(devfd, text)
            time.sleep(0.5)
            got_a = read_all(tty_a, 100, 0.5)
            got_b = read_all(tty_b, 100, 0.5)
            return got_a, got_b

        # Without a lock, cards go to the viewer with the focus
        focus(2)
        check(card(b"%1?") == (b"", b"%1?"), "no lock: to B, which has focus")
        focus(1)
        check(card(b"%2?") == (b"%2?", b""), "no lock: to A, which has focus")

        # Both may write without a lock
        os.write(tty_b, b"R")
        check(read_all(devfd, 1) == b"R", "B writes without a lock")

        # A locks for a sale: cards go to A even if B has the focus, B's
        # writes are dropped
        a.cmd("lock cardreader")
        check(a.line() == "ok cardreader", "A locks")
        focus(2)
        check(card(b"%3?") == (b"%3?", b""), "locked: to A despite focus")
        os.write(tty_b, b"X")
        os.write(tty_a, b"A")
        check(read_all(devfd, 2, 1) == b"A", "B's write dropped during A's sale")

        # B cannot lock, or waits
        b.cmd("lock cardreader")
        reply = b.line()
        check(reply is not None and reply.startswith("error cardreader busy")
              and "127.0.0.1::5945" in reply, "B is told who has it: %s"
              % reply)
        b.cmd("lock cardreader wait=20")
        check(b.line(1.5) is None, "B waits for the lock")
        a.cmd("unlock cardreader")
        check(a.line() == "ok cardreader", "A unlocks")
        check(b.line() == "ok cardreader", "B gets the lock")
        focus(1)
        check(card(b"%4?") == (b"", b"%4?"), "locked by B: to B")
        a.cmd("lock cardreader wait=1")
        reply = a.line(5)
        check(reply is not None and reply.startswith("error cardreader busy"),
              "waiting for the lock ends after its time")

        # The lock ends with the application that holds it
        b.sock.close()
        time.sleep(0.5)
        a.cmd("lock cardreader")
        check(a.line() == "ok cardreader", "lock free after B's application went")
        a.cmd("unlock cardreader")
        a.line()

        # The viewer that holds the device goes: the other one loses the
        # device and opens it itself on the next request
        os.close(tty_a)
        os.close(tty_b)
        viewers[0].terminate()
        viewers[0].wait()
        b = App(os.path.join(dirs[1], "control"))
        b.line()
        deadline = time.time() + 10
        reply = None
        while time.time() < deadline:
            b.cmd("request cardreader serial Kartenleser")
            reply = b.line()
            if reply and reply.startswith("ok"):
                break
            time.sleep(0.5)
        check(reply is not None and reply.startswith("ok cardreader "),
              "B opens it after A's viewer went: %s" % reply)
        tty_b = os.open(reply.split(" ", 2)[2], os.O_RDWR | os.O_NOCTTY)
        check(card(b"%5?")[1] == b"%5?", "cards reach B")

        # vnc-device holds the device while its command runs
        b.cmd("release cardreader")
        b.line()
        out = os.path.join(tmp, "out")
        r = subprocess.run(
            [sys.executable, vnc_device, "--dir", dirs[1],
             "--label", "Kartenleser", "cardreader", "serial", "--",
             "sh", "-c", "printf R > {}; echo {} > " + out],
            timeout=20)
        check(r.returncode == 0, "vnc-device runs the command")
        check(read_all(devfd, 1) == b"R", "the command wrote to the device")
        check(open(out).read().startswith(dirs[1]), "{} is the device path")
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
