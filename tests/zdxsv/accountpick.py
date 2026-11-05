"""Fast login with a picked account: tokenlogin.py with two accounts of one Discord identity.
  OUT=<dir> [PICK=2] python tests/zdxsv/accountpick.py
Env as boottime.py. The server (ZBIN) must take /lbs/login form `account`. The test migrates OUT's copy
of the db, links the two oldest accounts with users to one provider identity (in that order), stores a
token for the first, writes the token file and `ZdxsvAccount = PICK` in p1's ini (both removed after).
Exit 0 = tokenlogin.py's checks hold for the picked account (PICK 0 or 1: the first, 2: the second).
"""
import base64
import hashlib
import os
import secrets
import sqlite3
import sys
from datetime import datetime, timedelta

import boottime
import tokenlogin
import zrig


def ini_path():
    return os.path.join(os.environ["RUN"], "p1", "PCSX2", "inis", "PCSX2.ini")


def set_pick(value):
    path = ini_path()
    lines = [l for l in open(path, encoding="utf-8", errors="surrogateescape").read().split("\n")
             if not l.startswith("ZdxsvAccount =")]
    if value is not None:
        if "[DEV9/Eth]" not in lines:
            lines += ["", "[DEV9/Eth]"]
        lines.insert(lines.index("[DEV9/Eth]") + 1, f"ZdxsvAccount = {value}")
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write("\n".join(lines))


def prepare(out):
    tokenlogin.migrate(out)
    pick = int(zrig.env("PICK") or "2")
    token = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    c = sqlite3.connect(os.path.join(out, "zdxsv.db"))
    try:
        keys = [r[0] for r in c.execute("SELECT a.login_key FROM account a WHERE EXISTS "
                                        "(SELECT 1 FROM user u WHERE u.login_key = a.login_key) "
                                        "ORDER BY a.created LIMIT 2")]
        if len(keys) < 2:
            print("FAIL: fewer than 2 accounts with users in the db")
            return 1
        now = datetime.now()
        c.execute("DELETE FROM auth_identity WHERE login_key IN (?, ?)", keys)
        for i, k in enumerate(keys):
            c.execute("INSERT INTO auth_identity (provider, provider_user_id, login_key, created) VALUES (?, ?, ?, ?)",
                      ("discord", "accountpick-rig", k, now + timedelta(seconds=i)))
        c.execute("INSERT INTO auth_token (token_hash, login_key, created) VALUES (?, ?, ?)",
                  (hashlib.sha256(token.encode()).hexdigest(), keys[0], now))
        c.commit()
    except sqlite3.Error as e:
        print(f"FAIL: db: {e} (migratedb.log)")
        return 1
    finally:
        c.close()
    want = keys[1] if pick == 2 else keys[0]
    print(f"accounts {keys}, pick {pick}, want {want}")
    with open(os.path.join(out, "token_account.txt"), "w") as f:
        f.write(want)
    with open(tokenlogin.token_path(), "w") as f:
        f.write(token + "\n")
    set_pick(pick)
    return 0


def main():
    zrig.take()
    zrig.need("ZDXSV_DIR", "IP", "OUT", "CARDS")
    try:
        return boottime.run(os.environ["OUT"], tokenlogin.after, prepare)
    finally:
        set_pick(None)
        if os.path.exists(tokenlogin.token_path()):
            os.remove(tokenlogin.token_path())


if __name__ == "__main__":
    sys.exit(main())
