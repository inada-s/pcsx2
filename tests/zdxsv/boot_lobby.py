"""Stand-in lobby for the boot to the user_id selection (boottime.sh LOBBY=fake): the lobby API's
POST /lbs/login and the lobby's login exchange up to the user_id list (0x6131), nothing after.

  boot_lobby.py --public IP:8200 [--port 8200] [--api 127.0.0.1:9881] [--pidfile path]

The list holds one user, FAKE01. Each frame is logged; "user_id list sent" marks the end.
"""
import argparse, http.server, json, os, socket, struct, sys, threading, time, urllib.parse

HDR = struct.Struct(">BBHHHI")
Q, A, N, C = 0x01, 0x02, 0x10, 0xFF
SESSION = "0000000001"
LBS_INFO = "0ad601082008,ZDX BETA SERVER"  # 12-char lobby id, then the name
# Answer to 0x61A0, as the zdxsv lobby gives it.
TIMES = bytes.fromhex("00000e1000000258")
USER = b"FAKE01"
NAME = bytes.fromhex("82a082a082a0")  # Shift-JIS あああ
lock = threading.Lock()


def log(s):
    with lock:
        sys.stdout.write("%s %s\n" % (time.strftime("%T"), s))
        sys.stdout.flush()


def frame(cat, cmd, seq, body=b""):
    return HDR.pack(0x18, cat, cmd, len(body), seq, 0x00FFFFFF) + body


def user_list():
    return b"\x01" + struct.pack(">H", len(USER)) + USER + struct.pack(">H", len(NAME)) + NAME + b"\x00\x00"


def recv(sock, n):
    b = b""
    while len(b) < n:
        c = sock.recv(n - len(b))
        if not c:
            raise ConnectionError("closed")
        b += c
    return b


def lobby(sock, addr):
    sock.settimeout(60)
    try:
        sock.sendall(frame(Q, 0x6101, 1, b"\x28\x37"))
        while True:
            _, cat, cmd, n, seq, _ = HDR.unpack(recv(sock, HDR.size))
            if n > 4096:
                raise ValueError("body %d bytes" % n)
            body = recv(sock, n)
            log("%s C->S cat %02x 0x%04X seq %d %s" % (addr[0], cat, cmd, seq, body.hex()[:64]))
            if cat == A and cmd == 0x6101:
                sock.sendall(frame(Q, 0x6103, 3))
            elif cat == Q and cmd == 0x61A0:
                sock.sendall(frame(A, 0x61A0, seq, TIMES))
            elif cat == Q and cmd == 0x600E:
                sock.sendall(frame(A, 0x600E, seq, body))
            elif cat == A and cmd == 0x6103:
                sock.sendall(frame(N, 0x6131, 4, user_list()))
                log("%s user_id list sent" % addr[0])
    except (OSError, ValueError, ConnectionError) as e:
        log("%s closed: %s" % (addr[0], e))
    finally:
        sock.close()


def api(public):
    class H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = min(int(self.headers.get("Content-Length") or 0), 4096)
            form = urllib.parse.parse_qs(self.rfile.read(n).decode("ascii", "replace"))
            key = form.get("login_key", [""])[0]
            if self.path != "/lbs/login" or not key:
                self.send_response(404)
                self.end_headers()
                return
            body = json.dumps({"login_key": key, "session_id": SESSION, "lobby_addr": public, "lbs_info": LBS_INFO},
                              separators=(",", ":")).encode()  # the emulator reads only compact JSON
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            log("login %s" % key)

        def log_message(self, *a):
            pass

    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--public", required=True, help="lobby address as the game dials it (IP:8200)")
    ap.add_argument("--port", type=int, default=8200)
    ap.add_argument("--api", default="127.0.0.1:9881")
    ap.add_argument("--pidfile", help="written with this process id once listening")
    a = ap.parse_args()
    host, port = a.api.rsplit(":", 1)
    srv = http.server.ThreadingHTTPServer((host, int(port)), api(a.public))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", a.port))
    ls.listen(4)
    if a.pidfile:
        with open(a.pidfile, "w") as f:
            f.write(str(os.getpid()))
    log("boot lobby up: api %s, lobby :%d as %s" % (a.api, a.port, a.public))
    while True:
        s, addr = ls.accept()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=lobby, args=(s, addr), daemon=True).start()


if __name__ == "__main__":
    main()
