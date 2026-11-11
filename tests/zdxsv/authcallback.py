"""Sign-in callback page reaches the browser whole (no game, no server).
  python tests/zdxsv/authcallback.py
PCSX2 starts without a game with ZDXSV_AUTH_SIGNIN_TEST=1 (data dir $RUN/authcallback) and ZDXSV_AUTH_URL at a
stand-in served here, which answers POST /token. The script sends the 127.0.0.1 callback the way a browser
does, with about 3 KB of headers, and reads the answer to the end.
Exit 0 = the answer is the whole `Signed in` page (no connection reset) and emulog has `auth: signed in`.
"""
import http.server
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.parse

import launch
import zrig

TOKEN = "authcallbacktoken"


class TokenStandIn(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        body = json.dumps({"token": TOKEN}).encode() if self.path == "/auth/token" else b""
        self.send_response(200 if body else 404)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def callback(url):
    """The browser's GET of the emulator's callback; returns (answer, reset)."""
    u = urllib.parse.urlsplit(url)
    req = (f"GET {u.path}?{u.query} HTTP/1.1\r\nHost: {u.netloc}\r\n"
           "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) authcallback\r\n"
           "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\nAccept-Language: ja,en;q=0.9\r\n"
           + "".join(f"X-Browser-Like-{i}: {'x' * 100}\r\n" for i in range(30)) + "\r\n")
    got = b""
    with socket.create_connection((u.hostname, u.port), timeout=10) as s:
        s.sendall(req.encode())
        try:
            while True:
                d = s.recv(4096)
                if not d:
                    return got.decode(errors="replace"), False
                got += d
        except ConnectionResetError:
            return got.decode(errors="replace"), True


def main():
    zrig.take()
    zrig.need("RUN")
    exe = launch.exe_path()
    data = os.path.join(os.path.abspath(os.environ["RUN"]), "authcallback")
    ini = os.path.join(data, "PCSX2", "inis", "PCSX2.ini")
    if not os.path.exists(ini):
        os.makedirs(data, exist_ok=True)
        subprocess.run([exe, "-datapath", data, "-testconfig"], timeout=60)
        with open(ini, encoding="utf-8-sig") as f:
            text = f.read().replace("SetupWizardIncomplete = true", "SetupWizardIncomplete = false")
        with open(ini, "w", encoding="utf-8") as f:
            f.write(text)
    zrig.ini_set(ini, "AutoUpdater", "CheckAtStartup", "false")
    token_file = os.path.join(data, "PCSX2", "inis", "zdxsv_auth_token.txt")
    log = os.path.join(data, "emulog.txt")
    for f in (token_file, log):
        if os.path.exists(f):
            os.remove(f)

    stand_in = http.server.ThreadingHTTPServer(("127.0.0.1", 0), TokenStandIn)
    threading.Thread(target=stand_in.serve_forever, daemon=True).start()
    env = dict(os.environ, ZDXSV_AUTH_SIGNIN_TEST="1",
               ZDXSV_AUTH_URL=f"http://127.0.0.1:{stand_in.server_port}/auth")
    p = subprocess.Popen([exe, "-datapath", data, "-logfile", log], env=env)
    try:
        start = None
        for _ in range(120):
            time.sleep(0.5)
            m = os.path.exists(log) and re.search(r"Zdxsv: auth: open (\S+)", open(log, encoding="utf-8", errors="replace").read())
            if m:
                start = urllib.parse.parse_qs(urllib.parse.urlsplit(m.group(1)).query)
                break
        if not start:
            print("FAIL: no `auth: open` line")
            return 1
        port, state = start["redirect_port"][0], start["state"][0]
        answer, reset = callback(f"http://127.0.0.1:{port}/callback?code=authcallbackcode&state={state}")
        signed = False
        for _ in range(20):
            if "Zdxsv: auth: signed in" in open(log, encoding="utf-8", errors="replace").read():
                signed = True
                break
            time.sleep(0.5)
    finally:
        zrig.kill(p.pid)
        stand_in.shutdown()
    whole = answer.startswith("HTTP/1.1 200") and "Signed in" in answer
    ok = whole and not reset and signed
    print(f"{'PASS' if ok else 'FAIL'}: page {'whole' if whole else 'cut'} ({len(answer)} bytes), "
          f"reset {reset}, emulog signed in {signed}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
