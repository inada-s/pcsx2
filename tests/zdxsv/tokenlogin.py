"""Fast login by an emulator token: boottime.py with a token file in p1's settings folder.
  OUT=<dir> python tests/zdxsv/tokenlogin.py
Env as boottime.py. The server (ZBIN) must have token logins (/lbs/login form token). The test
migrates OUT's copy of the db, stores a token for an account with users, writes it to
<p1>/inis/zdxsv_auth_token.txt (removed after the run).
Exit 0 = the game reached the user_id screen, emulog has `fast login: by token` and a session, and the
token account's last login in the db is this run's.
"""
import base64
import hashlib
import os
import secrets
import sqlite3
import subprocess
import sys
from datetime import datetime

import boottime
import zrig

TOKEN_FILE = "zdxsv_auth_token.txt"


def token_path():
    return os.path.join(os.environ["RUN"], "p1", "PCSX2", "inis", TOKEN_FILE)


def migrate(out):
    z = zrig.env("ZBIN") or os.path.join(os.environ["ZDXSV_DIR"], "bin", "zdxsv" + zrig.EXE)
    with open(os.path.join(out, "migratedb.log"), "wb") as f:
        subprocess.call([z, "migratedb"], cwd=out, env=dict(os.environ, ZDXSV_DB_NAME="zdxsv.db"),
                        stdout=f, stderr=subprocess.STDOUT)
    return 0


def card_token(db, card, stand_in=False):
    """(token, error) for the account whose login_key the card holds in DB. stand_in: the token is the
    login_key itself (fake_lobby.py, boot_lobby.py); else a new token stored in DB (migrated first)."""
    if not stand_in:
        migrate(os.path.dirname(db))
    data = open(card, "rb").read()
    token = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    c = sqlite3.connect(db)
    try:
        keys = [r[0] for r in c.execute("SELECT login_key FROM account") if r[0] and r[0].encode() in data]
        if len(keys) != 1:
            return None, f"{os.path.basename(card)} matches {len(keys)} accounts in {db}"
        if stand_in:
            return keys[0], None
        c.execute("INSERT INTO auth_token (token_hash, login_key, created) VALUES (?, ?, ?)",
                  (hashlib.sha256(token.encode()).hexdigest(), keys[0], datetime.now()))
        c.commit()
    except sqlite3.Error as e:
        return None, f"db: {e} (migratedb.log)"
    finally:
        c.close()
    return token, None


def prepare(out):
    migrate(out)
    token = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    c = sqlite3.connect(os.path.join(out, "zdxsv.db"))
    try:
        row = c.execute("SELECT a.login_key FROM account a WHERE EXISTS "
                        "(SELECT 1 FROM user u WHERE u.login_key = a.login_key) ORDER BY a.created LIMIT 1").fetchone()
        if not row:
            print("FAIL: no account with users in the db")
            return 1
        c.execute("INSERT INTO auth_token (token_hash, login_key, created) VALUES (?, ?, ?)",
                  (hashlib.sha256(token.encode()).hexdigest(), row[0], datetime.now()))
        c.commit()
    except sqlite3.Error as e:
        print(f"FAIL: db: {e} (migratedb.log)")
        return 1
    finally:
        c.close()
    with open(os.path.join(out, "token_account.txt"), "w") as f:
        f.write(row[0])
    LAST_LOGIN[out] = last_login(out, row[0])
    with open(token_path(), "w") as f:
        f.write(token + "\n")
    return 0


def after(out):
    emulog = open(os.path.join(os.environ["RUN"], "p1", "PCSX2", "logs", "emulog.txt"), encoding="utf-8",
                  errors="replace").read()
    if "Zdxsv: fast login: by token" not in emulog:
        print("FAIL: emulog has no `fast login: by token`")
        return 1
    session = "Zdxsv: fast login: session " in emulog
    key = open(os.path.join(out, "token_account.txt")).read()
    logged_in = last_login(out, key) != LAST_LOGIN.get(out)
    ok = session and logged_in
    print(f"{'PASS' if ok else 'FAIL'}: emulog session {session}, token account logged in {logged_in}")
    return 0 if ok else 1


LAST_LOGIN = {}


def last_login(out, key):
    c = sqlite3.connect(os.path.join(out, "zdxsv.db"))
    try:
        return c.execute("SELECT last_login FROM account WHERE login_key = ?", (key,)).fetchone()[0]
    finally:
        c.close()


def main():
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    try:
        return boottime.run(os.environ["OUT"], after, prepare)
    finally:
        if os.path.exists(token_path()):
            os.remove(token_path())


if __name__ == "__main__":
    sys.exit(main())
