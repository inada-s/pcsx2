"""Slow login API under fast boot: the game waits at DNAS (paused) for the answer instead of running its own
DNAS, which fails and shows its error.
  OUT=<dir> python tests/zdxsv/slowlogin.py
Env as freshboot.py (servers + fresh_boot p1, FB_LOBBY=0), plus DELAY (s the login API answers late, default 2).
The game's login API URL points at a relay on 127.0.0.1:9896 that forwards to the fake login API DELAY s late.
Exit 0 = fast boot reaches the user_id selection after one login and the emulog has no
`the game left DNAS before the answer`.
"""
import http.server
import os
import sys
import threading
import time
import urllib.request

import freshboot
import zrig

PORT = 9896


class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        time.sleep(float(zrig.env("DELAY", "2")))
        req = urllib.request.Request(f"http://127.0.0.1:{os.environ['API']}{self.path}", data=body,
                                     headers={"Content-Type": self.headers.get("Content-Type", "")})
        with urllib.request.urlopen(req, timeout=10) as r:
            code, data = r.status, r.read()
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


def main():
    zrig.take()
    os.environ.setdefault("API", "9881")
    freshboot.servers()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Relay)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    os.environ.update(FB_LOBBY="0", PCSX2_ENV=f"ZDXSV_LOGIN_API_URL=http://127.0.0.1:{PORT}/lbs/login "
                      + zrig.env("PCSX2_ENV"))
    os.environ.setdefault("BOOT_WAIT", "120")
    booted = freshboot.fresh_boot(1)
    log = os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt")
    logins = len(zrig.log_lines(log, r"Zdxsv: fast login: by token"))
    left = len(zrig.log_lines(log, r"Zdxsv: fast login: the game left DNAS before the answer"))
    ok = booted and logins == 1 and left == 0
    print(f"{'PASS' if ok else 'FAIL'}: user_id selection {booted}, logins {logins}, left DNAS {left}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
