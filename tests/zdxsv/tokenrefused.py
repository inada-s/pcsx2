"""Fast login with a token the server refuses (401) signs out on this PC (no zdxsv server needed).
  python tests/zdxsv/tokenrefused.py
A stand-in login API on 127.0.0.1 answers 401 to every POST. p1 boots through the `top` route with
ZDXSV_FAST_LOGIN=1 and a token file in its settings folder.
Exit 0 = the stand-in got the token, the game went back to the connection menu (net 3), the emulog has
`auth: the server refused the token` and the expired sign-in line, and the token file is gone.
Control: a build without the 401 handling keeps the token file.
"""
import http.server
import os
import secrets
import sys
import threading
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


class Refuse(http.server.BaseHTTPRequestHandler):
    tokens = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0)).decode("ascii", "replace")
        Refuse.tokens += urllib.parse.parse_qs(body).get("token", [])
        self.send_response(401)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *a):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Refuse)
os.environ.update(ZDXSV_FAST_LOGIN="1", ZDXSV_AUTH_AT_DNAS="0", ZDXSV_FAST_BOOT="0", ZDXSV_TLS="0",
                  ZDXSV_SERVER_HOST="127.0.0.1",
                  ZDXSV_LOGIN_API_URL=f"http://127.0.0.1:{server.server_address[1]}/lbs/login")
import launch  # noqa: E402
import zrig  # noqa: E402
from drive import route_text  # noqa: E402
from pine import Pine  # noqa: E402

NET = 0xcd1360
PHASE = 0x6dd770 - 0x59ec
LIMIT = float(os.environ.get("LIMIT", "180"))


def main():
    token_file = os.path.join(os.environ["RUN"], "p1", "PCSX2", "inis", "zdxsv_auth_token.txt")
    token = secrets.token_urlsafe(32)
    with open(token_file, "w") as f:
        f.write(token + "\n")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    pids = launch.launch(["1"], headless=True)
    back = False
    try:
        time.sleep(8)
        p = Pine(28011)
        lock = threading.Lock()
        call = p.call
        p.call = lambda *a: (lock.acquire(), call(*a), lock.release())[1]

        def press(text):
            for tok in text.split(","):
                try:
                    p.seq(tok)
                except Exception as e:  # a missed key ends the route; the checks below fail
                    print("route:", tok, e, flush=True)
                    return

        threading.Thread(target=press, args=(route_text("top", 1),), daemon=True).start()
        t0, last = time.time(), None
        while time.time() - t0 < LIMIT and not back:
            s = (p.r8(NET), p.r8(PHASE))
            if s != last:
                print("%.1f s net %x phase %d" % (time.time() - t0, s[0], s[1]), flush=True)
                back = s[0] == 3 and s[1] == 2
                last = s
            time.sleep(0.02)
    finally:
        zrig.kill(*pids.values())
        server.shutdown()
    emulog = open(os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt"), encoding="utf-8",
                  errors="replace").read()
    refused = "Zdxsv: auth: the server refused the token" in emulog
    told = "Zdxsv: fast login: the Discord sign-in has expired or was revoked, back to the network menu" in emulog
    kept = os.path.exists(token_file)
    if kept:
        os.remove(token_file)
    ok = token in Refuse.tokens and back and refused and told and not kept
    print(f"{'PASS' if ok else 'FAIL'}: stand-in got the token {token in Refuse.tokens}, back to the connection "
          f"menu {back}, emulog refused {refused} expired {told}, token file kept {kept}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
