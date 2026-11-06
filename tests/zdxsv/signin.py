"""Emulator sign-in and sign-out against the local server with a fake OAuth provider.
  OUT=<dir> python tests/zdxsv/signin.py
Env as boottime.py. The server (ZBIN) must have /auth/ (Discord sign-in). The test migrates OUT's copy
of the db, serves a fake provider on 127.0.0.1, starts p1 with ZDXSV_AUTH_SIGNIN_TEST=1 and plays the
browser: the start page, its provider link and every redirect up to the emulator's 127.0.0.1 callback.
Then p1 again with ZDXSV_AUTH_SIGNIN_TEST=signout.
Exit 0 = after sign-in p1's token file holds a token the db stores for the fake provider user (no account is
made); after sign-out the file is gone and the db has no token for that user.
"""
import hashlib
import html
import http.server
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request

import launch
import tokenlogin
import zbincheck
import zrig

API = "127.0.0.1:9881"
USER_ID = "424242424242424242"


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        if u.path == "/oauth2/authorize":
            q = urllib.parse.parse_qs(u.query)
            back = q["redirect_uri"][0] + "?" + urllib.parse.urlencode({"code": "fakecode", "state": q["state"][0]})
            return self.reply(302, headers=[("Location", back)])
        if u.path == "/api/users/@me" and self.headers.get("Authorization") == "Bearer fake-access":
            return self.reply(200, json.dumps({"id": USER_ID}).encode(), [("Content-Type", "application/json")])
        self.reply(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        form = urllib.parse.parse_qs(self.rfile.read(n).decode())
        if self.path == "/api/oauth2/token" and form.get("code") == ["fakecode"]:
            body = json.dumps({"access_token": "fake-access", "token_type": "Bearer"}).encode()
            return self.reply(200, body, [("Content-Type", "application/json")])
        self.reply(400)


def wait(pred, secs):
    end = time.time() + secs
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.5)
    return None


def emulog_url(emulog):
    try:
        m = re.search(r"Zdxsv: auth: open (\S+)", open(emulog, encoding="utf-8", errors="replace").read())
    except OSError:
        return None
    return m.group(1) if m else None


def browse(start):
    page = urllib.request.urlopen(start, timeout=10).read().decode("utf-8", "replace")
    m = re.search(r'<a class="b" href="([^"]+)"', page)
    if not m:
        return "no provider link on the start page"
    done = urllib.request.urlopen(html.unescape(m.group(1)), timeout=10)
    text = done.read().decode("utf-8", "replace")
    if not done.geturl().startswith("http://127.0.0.1:") or "Signed in" not in text:
        return f"browser ended at {done.geturl()}: {text[:200]}"
    return ""


def db_query(out, sql, *args):
    c = sqlite3.connect(os.path.join(out, "zdxsv.db"))
    try:
        return c.execute(sql, args).fetchall()
    finally:
        c.close()


def run_p1(out, run_, card, value, emulog):
    if os.path.exists(emulog):
        os.remove(emulog)
    ex = {"ZDXSV_TLS": "0", "ZDXSV_FAST_LOGIN": "0", "ZDXSV_NODEV9": "0", "ZDXSV_AUTH_SIGNIN_TEST": value,
          "ZDXSV_AUTH_URL": f"http://{API}/auth"}
    return launch.launch([1], speed="1", memcard=os.path.join(run_, f"Mcd001-{card}.ps2"),
                         headless=not zrig.env("SHOW"), extra_env=ex)[1]


def main():
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    out = os.environ["OUT"]
    run_ = os.environ["RUN"]
    p1 = os.path.join(run_, "p1", "PCSX2")
    emulog = os.path.join(p1, "logs", "emulog.txt")
    card = zrig.env("CARD") or os.environ["CARDS"].split()[0]
    token_file = tokenlogin.token_path()
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(os.path.join(run_, "zdxsv.db"), os.path.join(out, "zdxsv.db"))
    tokenlogin.migrate(out)
    if os.path.exists(token_file):
        os.remove(token_file)
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    procs = []
    try:
        if zbincheck.check(zrig.env("ZBIN") or os.path.join(os.environ["ZDXSV_DIR"], "bin", "zdxsv" + zrig.EXE)):
            return 1
        e = dict(os.environ, ZDXSV_LOBBY_API_ADDR=API, LOBBY_ARGS="-v=2", RUN=out,
                 ZDXSV_DISCORD_CLIENT_ID="test-client", ZDXSV_DISCORD_CLIENT_SECRET="test-secret",
                 ZDXSV_AUTH_PUBLIC_URL=f"http://{API}",
                 ZDXSV_AUTH_PROVIDER_URL=f"http://127.0.0.1:{provider.server_address[1]}")
        with open(os.path.join(out, "stack.out"), "wb") as f:
            procs.append(subprocess.Popen([zrig.PY, os.path.join(zrig.HERE, "stack.py"), os.environ["IP"]],
                                          env=e, stdout=f, stderr=subprocess.STDOUT))
        time.sleep(6)
        pid = run_p1(out, run_, card, "1", emulog)
        start = wait(lambda: emulog_url(emulog), 60)
        if not start:
            print("FAIL: emulog has no `auth: open`")
            return 1
        err = browse(start)
        if err:
            print(f"FAIL: {err}")
            return 1
        token = wait(lambda: os.path.exists(token_file) and open(token_file).read().strip(), 15)
        stored = db_query(out, "SELECT provider, provider_user_id FROM auth_token WHERE token_hash = ?",
                          hashlib.sha256(token.encode()).hexdigest()) if token else []
        signed_in = stored == [("discord", USER_ID)]
        print(f"sign-in: token file {'set' if token else 'none'}, db token identity {stored[0] if stored else None}")
        zrig.kill(pid)
        zrig.kill_name("pcsx2")
        pid = run_p1(out, run_, card, "signout", emulog)
        gone = wait(lambda: not os.path.exists(token_file), 60)
        left = db_query(out, "SELECT COUNT(*) FROM auth_token WHERE provider = 'discord' AND provider_user_id = ?",
                        USER_ID)[0][0]
        clean = wait(lambda: re.search(r"Zdxsv: auth: signed out\r?$", open(emulog, encoding="utf-8",
                                                                         errors="replace").read(), re.M), 10)
        signed_out = bool(gone) and left == 0 and bool(clean)
        print(f"sign-out: token file {'gone' if gone else 'still there'}, db tokens for the identity {left}, "
              f"emulog {'clean' if clean else 'reports an error'}")
        ok = signed_in and signed_out
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        provider.shutdown()
        for p in procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        zrig.kill_name("pcsx2", "zdxsv", "dnas")
        try:
            shutil.copyfile(emulog, os.path.join(out, "emulog-p1.txt"))
        except OSError:
            pass
        if os.path.exists(token_file):
            os.remove(token_file)


if __name__ == "__main__":
    sys.exit(main())
