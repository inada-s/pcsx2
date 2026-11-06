"""Sign-in at DNAS: fast login with no token pauses the VM, signs in, then logs in by the new token.
  OUT=<dir> python tests/zdxsv/dnassignin.py
Env as boottime.py, plus DELAY (s the fake browser waits before signing in, default 90). The server
(ZBIN) must have Discord sign-in. A stdlib fake Discord (signin.py) stands in for the provider; its user
is linked to an account with users in OUT's db, so the sign-in yields that account.
Exit 0 = the game reached the user_id screen, emulog has the paused sign-in and `fast login: by token`,
the session is the linked account's, and the token file holds a token the db maps to it.
"""
import hashlib
import http.server
import os
import re
import sqlite3
import sys
import threading
import time
from datetime import datetime

import boottime
import signin
import tokenlogin
import zrig


def prepare(out):
    if tokenlogin.migrate(out):
        return 1
    c = sqlite3.connect(os.path.join(out, "zdxsv.db"))
    try:
        row = c.execute("SELECT a.login_key FROM account a WHERE EXISTS "
                        "(SELECT 1 FROM user u WHERE u.login_key = a.login_key) ORDER BY a.created LIMIT 1").fetchone()
        if not row:
            print("FAIL: no account with users in the db")
            return 1
        c.execute("INSERT INTO auth_identity (provider, provider_user_id, login_key, created) VALUES (?, ?, ?, ?)",
                  ("discord", signin.USER_ID, row[0], datetime.now().isoformat(" ")))
        c.commit()
    finally:
        c.close()
    with open(os.path.join(out, "token_account.txt"), "w") as f:
        f.write(row[0])
    if os.path.exists(tokenlogin.token_path()):
        os.remove(tokenlogin.token_path())
    emulog = os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt")
    if os.path.exists(emulog):
        os.remove(emulog)
    threading.Thread(target=browser, args=(out,), daemon=True).start()
    return 0


def browser(out):
    emulog = os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt")
    start = signin.wait(lambda: signin.emulog_url(emulog), int(zrig.env("MAXS", "200")))
    if not start:
        return
    time.sleep(int(zrig.env("DELAY", "90")))
    err = signin.browse(start)
    with open(os.path.join(out, "browser.txt"), "w") as f:
        f.write(err or "signed in")


def after(out):
    emulog = open(os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt"),
                  encoding="utf-8", errors="replace").read()
    key = open(os.path.join(out, "token_account.txt")).read()
    paused = "fast login: no token, signing in with the VM paused" in emulog
    by_token = "Zdxsv: fast login: by token" in emulog
    m = re.search(r"Zdxsv: fast login: session ([^,\s]+)", emulog)
    c = sqlite3.connect(os.path.join(out, "zdxsv.db"))
    try:
        session = c.execute("SELECT session_id FROM account WHERE login_key = ?", (key,)).fetchone()[0]
        tok = open(tokenlogin.token_path()).read().strip() if os.path.exists(tokenlogin.token_path()) else ""
        stored = c.execute("SELECT login_key FROM auth_token WHERE token_hash = ?",
                           (hashlib.sha256(tok.encode()).hexdigest(),)).fetchall()
    finally:
        c.close()
    ok = paused and by_token and bool(m) and m.group(1) == session and stored == [(key,)]
    print(f"{'PASS' if ok else 'FAIL'}: paused sign-in {paused}, by token {by_token}, emulog session "
          f"{m.group(1) if m else None}, account session {session}, token file account "
          f"{stored[0][0] if stored else None}")
    return 0 if ok else 1


def main():
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), signin.Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    os.environ.update(ZDXSV_DISCORD_CLIENT_ID="test-client", ZDXSV_DISCORD_CLIENT_SECRET="test-secret",
                      ZDXSV_AUTH_PUBLIC_URL=f"http://{signin.API}",
                      ZDXSV_AUTH_PROVIDER_URL=f"http://127.0.0.1:{provider.server_address[1]}",
                      ZDXSV_FAST_LOGIN="1", ZDXSV_AUTH_AT_DNAS="1", ZDXSV_NODEV9="0")
    os.environ.setdefault("LIMIT", "600")
    try:
        return boottime.run(os.environ["OUT"], after, prepare)
    finally:
        provider.shutdown()
        if os.path.exists(tokenlogin.token_path()):
            os.remove(tokenlogin.token_path())


if __name__ == "__main__":
    sys.exit(main())
