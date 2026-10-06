# osdname.py <zdxsv.db> <osd-pN.txt>...: exit 1 unless every `|NP <user id> | <name> |` in the
# OSD log lines equals that user's name in the lobby db (UTF-8). Called by m4z.sh OSD=1.
# Why: a check that matched "some name" passed Shift-JIS-decoded-twice names
# ("縺ゅ≠縺" for "あああ") and still passed.
import re
import sqlite3
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
db = sqlite3.connect(sys.argv[1])
names = dict(db.execute("select user_id, name from user"))
bad = 0
seen = 0
for path in sys.argv[2:]:
    text = open(path, encoding="utf-8", errors="replace").read()
    for uid, name in re.findall(r"\|[1-4]P ([A-Z0-9]{6}) \| ([^|]*?) \|", text):
        seen += 1
        want = names.get(uid)
        ok = want is not None and name == want.strip()
        bad += not ok
        print(f"{'ok' if ok else 'BAD'} {path.split('/')[-1]} {uid} osd={name!r} db={want!r}")
sys.exit(1 if bad or not seen else 0)
