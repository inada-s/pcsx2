"""replay play test (pcsx2 ZDXSV_REPLAY): plays a replay .pb headless in instance pN (turbo, exits at its end) with
PW hashes, then pwcheck compares them with the live battle's traces (rbk.py OUT/trace-p*.txt).
  OUT=dir FILE=x.pb python tests/zdxsv/rplay.py <live trace>...   Exit 0 = replay end logged + player work equal.
Env: N (instance, default 1), CLAMP (ZDXSV_EE_CLAMP of the recording: rbk.py's is in its emulog `rbk env`;
a lobby battle has none; unset = read from the `rbk env` lines of emulog-p*.txt next to the live traces, the
replay or its parent dir, a CLAMP differing from them FAILs before launch, CLAMP=none plays without one), PLAYERS (players compared, default 2), OWN (default 1: pwcheck --own),
STATE (any state of the game to boot from; default rbk-p1), MAXS (default 300).
WINDOW=1: windowed, 1x (no turbo). KEYS="secs:seq;..." (needs WINDOW=1): pcsx2ctl -Seq at secs after the
replay started; binds the seek hotkeys (PageUp back / PageDown forward 10 s), step (Home back / Insert forward), TogglePause (Space), key
display (End), round jump (Shift+PageUp / Shift+PageDown) in the instance ini. With PCSX2_ENV=ZDXSV_REPLAY_EXIT=0 the replay pauses at its end; Space then ends it (H lines written).
POV=p: plays position p's point of view (ZDXSV_REPLAY_POV; default the recorder's): the lobby answers name it.
SKIP_MS=1: skip MS selection (pcsx2's default; off here so KEYS seconds keep their frames).
SKIP_MS=ini: no ZDXSV_REPLAY_SKIP_MS, the instance ini's ZdxsvReplaySkipMs decides.
FOUR=1: four-screen, the point of view in pN, one spawned guest per other position (PLAYERS); each
guest's trace (trace-play-povP.txt) is pwchecked too, and the host's `spread` lines with all members live must stay
<= SPREAD (default 4) frames. With PCSX2_ENV=ZDXSV_REPLAY_SYNC=0 (control: no waiting) the spread check FAILs.
Control bar: KEYS="15:bar:show,w600,bar:timeline:0.7,shot:$OUT/a.png" (pcsx2ctl mouse tokens).
API=1: plays ZDXSV_REPLAY=http://127.0.0.1:API_PORT/lbs/replay?battle_code=<FILE's name> from apiserve.py (API_PORT,
default 9891), whose replay_url serves FILE; checks the replay_url line too. GZIP=1: replay_url serves FILE gzip encoded (Content-Encoding: gzip), as GCS serves the lobby's objects.
BAD=1: FILE is not a playable replay: exit 0 if the player is told it did not start (`replay: not started`).
"""
import atexit
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

import launch
import zrig

HOTKEYS = ("ZdxsvReplaySeekBack = Keyboard/PageUp", "ZdxsvReplaySeekForward = Keyboard/PageDown",
           "TogglePause = Keyboard/Space", "ZdxsvReplayToggleKeys = Keyboard/End",
           "ZdxsvReplayPrevRound = Keyboard/Shift & Keyboard/PageUp",
           "ZdxsvReplayNextRound = Keyboard/Shift & Keyboard/PageDown",
           "ZdxsvReplayStepBack = Keyboard/Home", "ZdxsvReplayStepForward = Keyboard/Insert")


def files_digest(root):
    """{relative path: sha1} of the files under ROOT but the child's own folder."""
    d = {}
    for dp, dns, fns in os.walk(root):
        dns[:] = [x for x in dns if os.path.join(dp, x) != os.path.join(root, "replay-child")]
        for f in fns:
            p = os.path.join(dp, f)
            try:
                with open(p, "rb") as fh:
                    d[os.path.relpath(p, root)] = hashlib.sha1(fh.read()).hexdigest()
            except OSError:
                d[os.path.relpath(p, root)] = "unreadable"
    return d


def grep(path, rx):
    try:
        return zrig.log_lines(path, rx)
    except OSError:
        return []


def pwcheck(trace, live, dst, players, prefix=""):
    """pwcheck.py into DST; True when coordinates (players < PLAYERS) and RNG agree over > 0 common frames."""
    own = ["--own"] if os.environ.get("OWN", "1") == "1" else []
    with open(dst, "w") as f:
        subprocess.call([zrig.PY, os.path.join(zrig.TOOLS, "pwcheck.py")] + own + [trace] + live, stdout=f)
    s, c = 0, 0
    for l in grep(dst, r"^(common|player|rng)"):
        print(f"{prefix}{l[:160 - len(prefix)]}")
        w = l.split()
        try:
            if w[0] == "player" and int(w[1]) < players:
                s += float(w[3])
            elif w[0] == "rng:":
                s += float(w[2])
            elif w[0] == "common":
                c = float(w[2])
        except (IndexError, ValueError):
            pass
    return c > 0 and s == 0


def main(live):
    zrig.take()
    zrig.need("OUT", "FILE", "RUN")
    out, file_, run = os.environ["OUT"], os.environ["FILE"], os.environ["RUN"]
    n = zrig.env("N", "1")
    state = zrig.env("STATE")
    if not state:
        zrig.need("RBKSTATES")
        state = os.path.join(os.environ["RBKSTATES"], "rbk-p1.p2s")
    window, four = zrig.env("WINDOW") == "1", zrig.env("FOUR") == "1"
    pov, keys_env, players = zrig.env("POV"), zrig.env("KEYS"), int(zrig.env("PLAYERS", "2"))
    port = zrig.env("API_PORT", "9891")
    os.makedirs(out, exist_ok=True)
    child = zrig.env("CHILD") == "1"
    data = os.path.join(run, f"p{n}", "PCSX2")
    logs = os.path.join(data, "replay-child", "logs") if child else os.path.join(data, "logs")
    l = os.path.join(logs, "emulog.txt")
    if child and os.path.exists(l):
        os.remove(l)
    api = []

    def cleanup():
        zrig.kill_name("pcsx2")
        for f in [l] + glob.glob(os.path.join(logs, "emulog-pov*.txt")):
            try:
                shutil.copyfile(f, os.path.join(out, "emulog-play.txt" if f == l else os.path.basename(f)))
            except OSError:
                pass
        for p in api:
            p.kill()

    atexit.register(cleanup)
    # the recording's clamp (a play without it differs from the live battle at the first clamped frame)
    rec = os.path.dirname(file_)
    es = set()
    for d in [os.path.dirname(f) for f in live] + [rec, os.path.join(rec, "..")]:
        es.update(os.path.normpath(e) for e in glob.glob(os.path.join(d, "emulog-p*.txt")))
    cl = sorted({m.group(1) for e in es for x in grep(e, r"ZdxsvGgpo: rbk env .* clamp=")
                 for m in [re.search(r"ZdxsvGgpo: rbk env .* clamp=([^ ]*)", x)] if m})
    clamp = zrig.env("CLAMP")
    if len(cl) > 1:
        print(f"FAIL recordings' emulogs name different clamps: {' '.join(cl)}")
        return 1
    if cl:
        if not clamp:
            clamp = cl[0]
            print(f"CLAMP={clamp} (rbk env)")
        elif clamp != cl[0] and clamp != "none":
            print(f"FAIL CLAMP={clamp} but the recording ran with clamp={cl[0]}")
            return 1
    if clamp == "none":
        clamp = ""
    t0 = time.time()
    for f in [l, os.path.join(out, "trace-play.txt")] + glob.glob(os.path.join(logs, "emulog-pov*.txt")) + \
            glob.glob(os.path.join(out, "trace-play-pov*.txt")):
        try:
            os.remove(f)
        except OSError:
            pass
    ini = os.path.join(run, f"p{n}", "PCSX2", "inis", "PCSX2.ini")
    keys = [k for k in keys_env.split(";") if k] if keys_env else []
    if keys_env:
        if not window:
            print("FAIL KEYS needs WINDOW=1")
            return 1
        with open(ini, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        nl = "\r\n" if "\r\n" in text else "\n"
        for b in HOTKEYS:
            if not re.search(rf"^{re.escape(b.split(' ')[0])} ", text, re.M):
                text = re.sub(r"^(\[Hotkeys\][^\n]*\n)", lambda m: m.group(1) + b + nl, text, count=1, flags=re.M)
        with open(ini, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(text)
        if len(re.findall(r"^(ZdxsvReplaySeek|TogglePause |ZdxsvReplayToggleKeys )", text, re.M)) != 4:
            print(f"FAIL hotkeys not bound in {ini}")
            return 1
        # every step's keys against the ini before launch (else an unbound key is refused only at its send)
        kc = os.path.join(out, "keycheck.txt")
        for k in keys:
            with open(kc, "wb") as f:
                r = zrig.ctl_check(ini, k.split(":", 1)[1], stdout=f)
            if r:
                with open(kc, encoding="utf-8", errors="replace") as f:
                    print(f.read(), end="")
                print(f"FAIL KEYS step '{k}' before launch")
                return 1
    play = file_
    if zrig.env("API") == "1":
        f = open(os.path.join(out, "apiserve.log"), "wb")
        api.append(subprocess.Popen([zrig.PY, "-I", os.path.join(zrig.HERE, "apiserve.py"), port, file_]
                                    + (["gzip"] if zrig.env("GZIP") else []),
                                    stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT))
        name = os.path.basename(file_)
        play = f"http://127.0.0.1:{port}/lbs/replay?battle_code={name[:-3] if name.endswith('.pb') else name}"
    e = {"ZDXSV_REPLAY": play, "ZDXSV_REPLAY_EXIT": "1", "ZDXSV_PW_HASH": "1",
         "ZDXSV_NET_TRACE": os.path.join(out, "trace-play.txt")}
    if pov:
        e["ZDXSV_REPLAY_POV"] = pov
    if not window:
        e["ZDXSV_REPLAY_TURBO"] = "1"
    if clamp:
        e["ZDXSV_EE_CLAMP"] = clamp
    if four:
        e["ZDXSV_REPLAY_FOUR"] = "1"
    if zrig.env("SKIP_MS") != "ini":
        e["ZDXSV_REPLAY_SKIP_MS"] = zrig.env("SKIP_MS", "0")
    for a in zrig.env("PCSX2_ENV").split():
        k, sep, v = a.partition("=")
        if sep:
            e[k] = v
    if child:
        del e["ZDXSV_REPLAY"]
        e["ZDXSV_REPLAY_STATE"] = os.path.abspath(state)
        args = ["-zdxsvreplay", f"{pov or -1}:{os.getpid()}:{play}"]
        pid = launch.launch([n], headless=not window, extra_env=e, quiet=True, extra_args=args)[int(n)]
        before = files_digest(data)
    else:
        pid = launch.launch([n], state=state, headless=not window, extra_env=e, quiet=True)[int(n)]
    print(f"p{n} pid {pid} slot {28010 + int(n)}", flush=True)
    # start check: the replay header line within 30 s, else stop
    for _ in range(30):
        if grep(l, r"ZdxsvGgpo: replay .*: (recorded at position|not a|bad|no lobby answers)|replay: state load failed"):
            break
        time.sleep(1)
    for x in grep(l, "ZdxsvGgpo: replay"):
        print(x[:200])
    if zrig.env("BAD") == "1":
        for _ in range(5):
            if grep(l, "ZdxsvGgpo: replay: not started"):
                print("PASS not started, player told")
                return 0
            time.sleep(1)
        print("FAIL no not started line")
        return 1
    if not grep(l, r"ZdxsvGgpo: replay .*: recorded at position"):
        print("FAIL replay did not start")
        return 1
    if api and not grep(l, re.escape(f"replay_url http://127.0.0.1:{port}/pb")):
        print("FAIL no replay_url line")
        return 1
    t1 = time.time()
    maxs = int(zrig.env("MAXS", "300"))
    badrx = (r"ZdxsvGgpo: replay( [^:]*)?: (state load failed|not a replay|bad header|not another position|cannot write)"
             r"|ZdxsvGgpo: replay seek: key [0-9]* load failed")
    while time.time() - t0 < maxs:
        time.sleep(1)
        if zrig.count_name("pcsx2") == 0:
            break
        # `state load failed` can come 0.6 s after the start check's `position` line, the player ran to MAXS 300 s
        bad = grep(l, badrx)
        if bad:
            print(bad[0][:200])
            print(f"FAIL replay error at {int(time.time() - t0)} s")
            return 1
        if keys and time.time() - t1 >= float(keys[0].split(":", 1)[0]):
            seq = keys[0].split(":", 1)[1]
            print(f"{int(time.time() - t1)} s: keys {seq}", flush=True)
            kf = os.path.join(out, "keys.txt")
            with open(kf, "wb") as f:
                rc = zrig.ctl(0, seq, stdout=f)
            with open(kf, encoding="utf-8", errors="replace") as f:
                print("".join(f.readlines()[-3:]), end="")
            if rc:
                print(f"FAIL pcsx2ctl rc {rc} (2 unbound key, 4 no Accessibility: nothing sent)")
                return 1
            keys.pop(0)
    ok = 0
    if keys:
        print(f"FAIL keys not sent: {' '.join(keys)}")
        ok = 1
    print(f"{int(time.time() - t0)} s: {zrig.count_name('pcsx2')} pcsx2 left")
    for x in grep(l, r"ZdxsvGgpo: (replay end|replay:|replay seek|net sends|zd steps)"):
        print(x[:200])
    if not grep(l, "ZdxsvGgpo: replay end"):
        print("FAIL no replay end")
        ok = 1
    if child:
        if zrig.count_name("pcsx2"):
            print("FAIL the replay child did not exit")
            ok = 1
        after = files_digest(data)
        changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
        if changed:
            print(f"FAIL the child changed the instance's files: {' '.join(changed[:10])}")
            ok = 1
    # the game's own position at its arm (its first key msg) is the asked point of view
    if pov and not grep(l, rf"net armed at vsync [0-9]*, position {re.escape(pov)}\b"):
        print(f"FAIL the game did not arm as position {pov}")
        ok = 1
    if not pwcheck(os.path.join(out, "trace-play.txt"), live, os.path.join(out, "pwcheck.txt"), players):
        print(f"FAIL coordinates (players < {players}) or RNG differ, or no frames")
        ok = 1
    if four:
        for f in glob.glob(os.path.join(logs, "emulog-pov*.txt")):
            shutil.copyfile(f, os.path.join(out, os.path.basename(f)))
        nt = 0
        for t in sorted(glob.glob(os.path.join(out, "trace-play-pov*.txt"))):
            nt += 1
            p = t.rsplit("-pov", 1)[1][:-4]
            gl = os.path.join(out, f"emulog-pov{p}.txt")
            for x in grep(gl, r"ZdxsvGgpo: replay (end|four-screen: the host|four-screen: frame [0-9]*, [0-9]* behind)")[:5]:
                print(x[:160])
            if not grep(gl, "ZdxsvGgpo: replay end"):
                print(f"FAIL pov {p}: no replay end in {gl}")
                ok = 1
            if not pwcheck(t, live, os.path.join(out, f"pwcheck-pov{p}.txt"), players, f"pov {p}: "):
                print(f"FAIL pov {p}: coordinates or RNG differ, or no frames")
                ok = 1
        if nt != players - 1:
            print(f"FAIL {nt} guest traces for {players} players")
            ok = 1
        # host's spread lines while all members are live
        sp = [int(m.group(1)) for x in grep(l, rf"four-screen: frame [0-9]*, {players} members")
              for m in [re.search(r"spread (-?[0-9]+)", x)] if m]
        mx, lim = max([0] + sp), int(zrig.env("SPREAD", "4"))
        print(f"spread: {len(sp)} lines with all members, max {mx} frames")
        if not (len(sp) >= 10 and mx <= lim):
            print(f"FAIL spread over {lim} frames or under 10 samples")
            ok = 1
    print(f"rplay {os.path.basename(file_)}: {int(time.time() - t0)} s, {'PASS' if ok == 0 else 'FAIL'}")
    return ok


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
