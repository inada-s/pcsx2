"""Teammate HP on the HUD: `rbk.py 2 1` with RAM dumps of position 0 every 600 vsyncs, then each dump is read.
Exit 0 = in at least 3 battle dumps with the players loaded, the 4 max-HP loads of the HUD read the teammate HP field
and it holds the teammate's HP, and in every dump outside battle they read max HP.
  OUT=<dir> python tests/zdxsv/allyhp.py
Env: CONTROL=1 runs with ZDXSV_ALLY_HP=0 (must FAIL). Other rbk.py env passes through.
"""
import os
import struct
import subprocess
import sys

import zrig

HERE = os.path.dirname(os.path.abspath(__file__))

SCENE_BLOCK = 0x6dd770 - 0x5ce4  # pointer; +0x52 = own player index
PLAYERS, PLAYER_SIZE = 0x839330, 0x2200
MAX_HP, HP, ALLY_HP, TEAM = 0x1f0, 0x1f2, 0x1ee, 0x21d5
PHASE = 0xc627b4  # u8: 6 battle, 7 round or game end
MAX_HP_LOADS = (0x14dbc4, 0x14dc1c, 0x15ac34, 0x15ac8c)


def check(path):
    m = open(path, "rb").read()
    s16 = lambda a: struct.unpack_from("<h", m, a)[0]
    loads = {struct.unpack_from("<I", m, a)[0] & 0xffff for a in MAX_HP_LOADS}
    battle = m[PHASE] in (6, 7)
    me = m[struct.unpack_from("<I", m, SCENE_BLOCK)[0] + 0x52]
    own = PLAYERS + PLAYER_SIZE * me
    ally = [PLAYERS + PLAYER_SIZE * p for p in range(4)
            if p != me and m[PLAYERS + PLAYER_SIZE * p + TEAM] == m[own + TEAM] and s16(PLAYERS + PLAYER_SIZE * p + MAX_HP) > 0]
    if not battle or not ally:
        return "idle", loads == {MAX_HP}, f"loads {sorted(map(hex, loads))}"
    want = max(0, s16(ally[0] + HP))
    ok = loads == {ALLY_HP} and s16(own + ALLY_HP) == want
    return "battle", ok, f"loads {sorted(map(hex, loads))} field {s16(own + ALLY_HP)} teammate hp {want}"


def main():
    out = zrig.env("OUT")
    if not out:
        sys.exit("OUT=<dir> required")
    dump = os.path.join(out, "dump")
    os.makedirs(dump, exist_ok=True)
    env = dict(os.environ)
    env0 = f"ZDXSV_RAM_DUMP={dump},0,600,100000"
    if env.get("CONTROL") == "1":
        env0 = "ZDXSV_ALLY_HP=0 " + env0
    env["ENV0"] = (env.get("ENV0", "") + " " + env0).strip()
    rc = subprocess.call([sys.executable, os.path.join(HERE, "rbk.py"), "2", "1"], env=env)
    if rc != 0:
        sys.exit(f"allyhp: rbk.py exit {rc}, FAIL")
    battles = 0
    ok_all = True
    for f in sorted(os.listdir(dump), key=lambda x: int(x.split(".")[0])):
        kind, ok, info = check(os.path.join(dump, f))
        print(f"{f} {kind} {'ok' if ok else 'BAD'} {info}", flush=True)
        battles += kind == "battle"
        ok_all = ok_all and ok
    passed = ok_all and battles >= 3
    print(f"allyhp: {battles} battle dumps, {'PASS' if passed else 'FAIL'}")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
