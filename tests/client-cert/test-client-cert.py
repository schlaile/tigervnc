#!/usr/bin/env python3
#
# Client certificates with a small authority of our own (contrib/vnc-ca):
# the server takes only viewers with a client certificate of the
# authority, the viewer takes only servers with a server certificate of
# it, and neither takes the other's kind of certificate.
#
#   tests/client-cert/test-client-cert.py PATH-TO-x0vncserver \
#       PATH-TO-vncviewer
#
# Needs Xvfb and openssl.

import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

failures = 0
here = os.path.dirname(os.path.abspath(__file__))
vnc_ca = os.path.join(here, "..", "..", "contrib", "vnc-ca", "vnc-ca")

secTypeVeNCrypt = 19
secTypeX509None = 260


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures += 1


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data += chunk
    return data


def make_certs(tmp):
    ca = os.path.join(tmp, "ca")
    run = lambda *args: subprocess.run(args, check=True,
                                       stdout=subprocess.DEVNULL)
    run(vnc_ca, "-d", ca, "init", "Test CA")
    run(vnc_ca, "-d", ca, "server", "localhost", "127.0.0.1")
    run(vnc_ca, "-d", ca, "client", "kasse1")
    run(vnc_ca, "-d", ca, "client", "kasse3")

    # A server certificate of our authority without extended key usage
    ext = os.path.join(tmp, "noeku.ext")
    with open(ext, "w") as f:
        f.write("basicConstraints = critical, CA:false\n"
                "subjectAltName = DNS:localhost, IP:127.0.0.1\n")
    run("openssl", "req", "-new", "-newkey", "ec", "-pkeyopt",
        "ec_paramgen_curve:prime256v1", "-nodes", "-subj", "/CN=localhost",
        "-keyout", os.path.join(tmp, "noeku.key"),
        "-out", os.path.join(tmp, "noeku.csr"))
    run("openssl", "x509", "-req", "-in", os.path.join(tmp, "noeku.csr"),
        "-CA", os.path.join(ca, "ca.pem"), "-CAkey", os.path.join(ca, "ca.key"),
        "-set_serial", "4242", "-days", "30", "-extfile", ext,
        "-out", os.path.join(tmp, "noeku.pem"))

    # Another authority with a server and a client certificate
    other = os.path.join(tmp, "other")
    run(vnc_ca, "-d", other, "init", "Other CA")
    run(vnc_ca, "-d", other, "server", "localhost", "127.0.0.1")
    run(vnc_ca, "-d", other, "client", "stranger")
    return ca, other


# --- The server: x0vncserver ------------------------------------------------

def rfb_types(port):
    """Security types and VeNCrypt subtypes the server offers"""
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    recv_exact(s, 12)
    s.sendall(b"RFB 003.008\n")
    n = recv_exact(s, 1)[0]
    types = list(recv_exact(s, n))
    subtypes = []
    if secTypeVeNCrypt in types:
        s.sendall(bytes([secTypeVeNCrypt]))
        recv_exact(s, 2)
        s.sendall(b"\x00\x02")
        recv_exact(s, 1)
        n = recv_exact(s, 1)[0]
        subtypes = list(struct.unpack(">%dI" % n, recv_exact(s, 4 * n)))
    s.close()
    return types, subtypes


def connect_x509(port, cert=None, key=None, ca=None):
    """Connects with X509None; returns (True, None) when the server
    accepted us, else (False, reason)"""
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    try:
        recv_exact(s, 12)
        s.sendall(b"RFB 003.008\n")
        n = recv_exact(s, 1)[0]
        types = recv_exact(s, n)
        if secTypeVeNCrypt not in types:
            return False, "no VeNCrypt"
        s.sendall(bytes([secTypeVeNCrypt]))
        recv_exact(s, 2)
        s.sendall(b"\x00\x02")
        recv_exact(s, 1)
        n = recv_exact(s, 1)[0]
        recv_exact(s, 4 * n)
        s.sendall(struct.pack(">I", secTypeX509None))
        if recv_exact(s, 1) != b"\x01":
            return False, "server failed to set up TLS"

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.load_verify_locations(ca)
        if cert:
            ctx.load_cert_chain(cert, key)
        try:
            t = ctx.wrap_socket(s, server_hostname="localhost")
            result = struct.unpack(">I", recv_exact(t, 4))[0]
        except (ssl.SSLError, EOFError, ConnectionError) as e:
            return False, "TLS: %s" % e
        if result == 0:
            return True, None
        n = struct.unpack(">I", recv_exact(t, 4))[0]
        return False, recv_exact(t, n).decode()
    finally:
        s.close()


def test_server(x0vncserver, tmp, ca, other):
    log = open(os.path.join(tmp, "server.log"), "w")
    crl = os.path.join(tmp, "crl.pem")
    shutil.copy(os.path.join(ca, "crl.pem"), crl)
    port = 5967
    server = subprocess.Popen(
        [x0vncserver, "-display", ":47", "-rfbport", str(port),
         "-localhost", "-SecurityTypes", "X509None,None,VncAuth,TLSNone",
         "-X509Cert", os.path.join(ca, "localhost.pem"),
         "-X509Key", os.path.join(ca, "localhost.key"),
         "-X509ClientCA", os.path.join(ca, "ca.pem"),
         "-X509ClientCRL", crl, "-Log", "*:stderr:100"],
        stdout=log, stderr=log)
    try:
        deadline = time.time() + 10
        while True:
            try:
                socket.create_connection(("127.0.0.1", port)).close()
                break
            except ConnectionError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)

        types, subtypes = rfb_types(port)
        check(types == [secTypeVeNCrypt] and subtypes == [secTypeX509None],
              "server offers only X509 types: %s %s" % (types, subtypes))

        cacert = os.path.join(ca, "ca.pem")
        c = lambda name, d=ca: (os.path.join(d, name + ".pem"),
                                os.path.join(d, name + ".key"))
        ok, why = connect_x509(port, *c("kasse1"), ca=cacert)
        check(ok, "server takes a client certificate of its authority (%s)"
              % why)
        ok, why = connect_x509(port, ca=cacert)
        check(not ok and "No client certificate" in why,
              "server refuses a viewer without certificate: %s" % why)
        ok, why = connect_x509(port, *c("localhost"), ca=cacert)
        check(not ok and "rejected" in why,
              "server refuses a server certificate as client: %s" % why)
        ok, why = connect_x509(port, *c("stranger", other), ca=cacert)
        check(not ok and "rejected" in why,
              "server refuses a certificate of another authority: %s" % why)

        ok, why = connect_x509(port, *c("kasse3"), ca=cacert)
        check(ok, "kasse3 is taken before its revocation (%s)" % why)
        subprocess.run([vnc_ca, "-d", ca, "revoke", "kasse3"], check=True,
                       stdout=subprocess.DEVNULL)
        shutil.copy(os.path.join(ca, "crl.pem"), crl)
        ok, why = connect_x509(port, os.path.join(ca, "kasse3.pem.revoked"),
                               os.path.join(tmp, "kasse3.key"), ca=cacert)
        check(not ok and "revoked" in why,
              "a new revocation list applies at once: %s" % why)
    finally:
        # x0vncserver may miss a SIGTERM that comes just before it waits
        server.terminate()
        try:
            server.wait(5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


# --- The viewer --------------------------------------------------------------

class FakeServer:
    """An RFB server that offers X509None with the given certificate and
    asks for a client certificate"""

    def __init__(self, cert, key, ca):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(cert, key)
        self.ctx.load_verify_locations(ca)
        self.ctx.verify_mode = ssl.CERT_OPTIONAL
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.result = None
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        self.listener.settimeout(15)
        try:
            s, _ = self.listener.accept()
        except socket.timeout:
            self.result = "no connection"
            return
        s.settimeout(15)
        try:
            s.sendall(b"RFB 003.008\n")
            recv_exact(s, 12)
            s.sendall(bytes([1, secTypeVeNCrypt]))
            recv_exact(s, 1)
            s.sendall(b"\x00\x02")
            recv_exact(s, 2)
            s.sendall(b"\x00")
            s.sendall(b"\x01" + struct.pack(">I", secTypeX509None))
            recv_exact(s, 4)
            s.sendall(b"\x01")
            t = self.ctx.wrap_socket(s, server_side=True)
            peer = t.getpeercert()
            # The viewer checks our certificate after the handshake and
            # closes the connection if it does not like it
            t.sendall(struct.pack(">I", 0))
            t.settimeout(3)
            try:
                data = t.recv(1)
            except socket.timeout:
                data = b"?"
            if not data:
                self.result = "closed"
                return
            cn = dict(x[0] for x in peer["subject"])["commonName"] \
                if peer else None
            self.result = "accepted, client %s" % cn
        except (ssl.SSLError, EOFError, ConnectionError, OSError) as e:
            self.result = "failed: %s" % e
        finally:
            s.close()


def run_viewer(viewer, tmp, server, args):
    log = os.path.join(tmp, "viewer.log")
    env = dict(os.environ, DISPLAY=":47", HOME=tmp,
               XDG_CONFIG_HOME=os.path.join(tmp, "config"),
               XDG_STATE_HOME=os.path.join(tmp, "state"))
    with open(log, "w") as f:
        v = subprocess.Popen([viewer, "-Log", "*:stderr:100",
                              "-SecurityTypes", "X509None"] + args +
                             ["127.0.0.1::%d" % server.port],
                             env=env, stdout=f, stderr=f)
    server.thread.join(20)
    time.sleep(0.5)
    v.terminate()
    try:
        v.wait(5)
    except subprocess.TimeoutExpired:
        v.kill()
        v.wait()
    return server.result, open(log).read()


def test_viewer(viewer, tmp, ca, other):
    p = lambda name, d=ca: (os.path.join(d, name + ".pem"),
                            os.path.join(d, name + ".key"))
    cacert = os.path.join(ca, "ca.pem")
    client = ["-X509CA", cacert, "-X509ClientCert", p("kasse1")[0],
              "-X509ClientKey", p("kasse1")[1]]

    result, log = run_viewer(viewer, tmp,
                             FakeServer(*p("localhost"), cacert), client)
    check(result == "accepted, client kasse1",
          "viewer presents its certificate to a proper server: %s" % result)

    result, log = run_viewer(viewer, tmp,
                             FakeServer(*p("kasse1"), cacert), client)
    check(result != "accepted, client kasse1" and
          "Invalid server certificate" in log,
          "viewer refuses a client certificate as server certificate: %s"
          % result)

    result, log = run_viewer(viewer, tmp,
                             FakeServer(os.path.join(tmp, "noeku.pem"),
                                        os.path.join(tmp, "noeku.key"),
                                        cacert), client)
    check(result != "accepted, client kasse1" and
          "Invalid server certificate" in log,
          "viewer refuses a server certificate without purpose: %s" % result)

    result, log = run_viewer(viewer, tmp,
                             FakeServer(*p("localhost", other), cacert),
                             client)
    check(result != "accepted, client kasse1" and
          "Invalid server certificate" in log,
          "viewer refuses another authority without asking: %s" % result)

    # Without a client certificate, the viewer still refuses a client
    # certificate as server certificate
    result, log = run_viewer(viewer, tmp,
                             FakeServer(*p("kasse1"), cacert),
                             ["-X509CA", cacert])
    check(result is not None and not result.startswith("accepted") and
          "Authentication success" not in log,
          "also without client certificate: %s" % result)


def main():
    x0vncserver, viewer = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="client-cert-")
    xvfb = subprocess.Popen(["Xvfb", ":47", "-screen", "0", "800x600x24"],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        time.sleep(1)
        ca, other = make_certs(tmp)
        shutil.copy(os.path.join(ca, "kasse3.key"),
                    os.path.join(tmp, "kasse3.key"))
        test_server(x0vncserver, tmp, ca, other)
        test_viewer(viewer, tmp, ca, other)
    finally:
        xvfb.terminate()
        xvfb.wait()
        if failures:
            for name in ("server.log", "viewer.log"):
                path = os.path.join(tmp, name)
                if os.path.exists(path):
                    print("--- " + name)
                    print(open(path).read()[-3000:])
        shutil.rmtree(tmp)
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
