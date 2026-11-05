"""Login API down at DNAS: fast login by token holds the game (paused) and retries until the API answers.
  OUT=<dir> python tests/zdxsv/apidown.py
Env as tokenlogin.py, plus DELAY (s the login API stays down after `fast login: by token`, default 60).
The game's login API URL points at a local relay that starts listening DELAY s late and then forwards
to the stack's /lbs/login.
Exit 0 = tokenlogin.py's checks pass, emulog has the hold and its end, and the relay was up before
the session.
"""
import http.server
import os
import sys
import threading
import time
import urllib.request

import boottime
import signin
import tokenlogin
import zrig

PORT = 9893


class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        req = urllib.request.Request(f"http://{signin.API}{self.path}", data=body,
                                     headers={"Content-Type": self.headers.get("Content-Type", "")})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                code, data = r.status, r.read()
        except urllib.error.HTTPError as e:
            code, data = e.code, e.read()
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def emulog_text():
    p = os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt")
    return open(p, encoding="utf-8", errors="replace").read() if os.path.exists(p) else ""


def relay(out):
    if not signin.wait(lambda: "Zdxsv: fast login: by token" in emulog_text(), int(zrig.env("MAXS", "200"))):
        return
    time.sleep(int(zrig.env("DELAY", "60")))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Relay)
    with open(os.path.join(out, "relay.txt"), "w") as f:
        f.write(f"up {time.time():.0f}\n")
    srv.serve_forever()


def prepare(out):
    emulog = os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt")
    if os.path.exists(emulog):
        os.remove(emulog)
    if tokenlogin.prepare(out):
        return 1
    threading.Thread(target=relay, args=(out,), daemon=True).start()
    return 0


def after(out):
    rc = tokenlogin.after(out)
    log = emulog_text()
    held = "game held, retry in" in log
    answered = "Zdxsv: fast login: login API answered, game resumed" in log
    up = os.path.exists(os.path.join(out, "relay.txt"))
    ok = rc == 0 and held and answered and up
    print(f"{'PASS' if ok else 'FAIL'}: token login {rc == 0}, held {held}, answered after hold {answered}, relay up {up}")
    return 0 if ok else 1


def main():
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    os.environ.update(ZDXSV_FAST_LOGIN="1", ZDXSV_LOGIN_API_URL=f"http://127.0.0.1:{PORT}/lbs/login")
    os.environ.setdefault("LIMIT", "600")
    try:
        return boottime.run(os.environ["OUT"], after, prepare)
    finally:
        if os.path.exists(tokenlogin.token_path()):
            os.remove(tokenlogin.token_path())


if __name__ == "__main__":
    sys.exit(main())
