"""rbk test: N pcsx2 resumed at once from the post-entry states (rbkprep.py), no lobby or battle server: pcsx2
ZDXSV_RBK=i/N answers the battle start and connect itself, menus run turbo, the battle runs over GGPO (net=1),
each pcsx2 exits at its session end. Exit 0 = every client logged its session end and pwcheck found the peers in sync.
  OUT=<dir> python tests/zdxsv/rbk.py [N] [seed]   (N 2|4, default 4: arguments, not env; no seed: host pad = no input)
Env: RBKSTATES (dir of rbk-p1..p4.p2s), TIME (rule 作戦時間 s, menu 90..270, 30 works too; default 90),
COUNT (連続対戦数, default 1; 0 = rematch picked by input), GAUGE (戦力ゲージ, default 1: first shoot-down ends the
battle, recorded 600), MAXS (default 900),
SELECT (出撃準備 timer frames via ZDXSV_EE_CLAMP, default 300; SELECT= off), BATTLE (battle timer frames, u16
0x838f66, default off = TIME), BRIEF (pre-play 600-frame countdown, u32 0x6f2b50 from select end, default 1;
BRIEF= off), TAIL (frames run after the end msg, ZDXSV_NET_TAIL, default 60), TURBO (default 1: battle turbo too,
frame-identical; TURBO= nominal), SHOWN=1 (a shown display window instead of the GS Null renderer), PWDUMP=1
(player-work dumps $OUT/pw-p*.bin for pwdiff.py), TRACE=0 (no ZDXSV_NET_TRACE / PW_HASH: timing as production,
sync not judged), PCSX2_ENV (extra env for all), ENV0 (extra env for position 0 only: controls), DELAY (GGPO input
delay, default 2), GGPO (ZDXSV_GGPO value, default net=1,players=N,delay=DELAY).
TSCALE (turbo cap, ini TurboScalar, default 4; TSCALE= = pcsx2 default 2). MTVU=1: MTVU speedhack on in the ini;
pcsx2 must turn it off for the VM. N=4 plays p1..p4. N=2 plays p1 (position 0) + p3 (position 1).
INI0="Section:Key=Value;..." (syncset): ini entries for peer 0 only; its ini before them is kept in PCSX2.ini.rbk0
and put back by the next run. SPEED0=X: peer 0 runs at speed X.
LAT (one-way ms; set = a udprelay.py per peer pair), JITTER (ms, 0), LOSS (0..1, 0).
MAXWAIT=n: FAIL if a peer's OSD `Wait` (frames stopped at GGPO's prediction limit) ends above n.
MAXLAG=n: FAIL if a peer's OSD Lag level (`osd lag` lines: segments filled, 4 = Poor) is above n in the 2nd half of the
battle (the 1st half holds the meter's start and its slow recovery).
MAXLOSS=n: FAIL if a peer's `peer report` counts more than n GGPO packets lost (received or sent) to a peer.
STALL=ms: suspend the peer at position 1 for ms every 5 s, 8 times from 40 s, so the others run ahead.
Sync check: pwcheck compares each player's x, y, z and game RNG B (0x6d793c) on every frame (RNG A takes
machine-local sound draws: reported, not judged; OWN: no effect).
"""
import atexit
import os
import re
import shutil
import subprocess
import sys
import threading
import time

import launch
import zrig

HERE = os.path.dirname(os.path.abspath(__file__))


# OSD `P` at the end of a stall run; a peer that time sync does not slow down stays about 5 ahead.
MAX_AHEAD_AFTER_STALL = 3

def opt(k, default):
    """Bash ${K-default}: unset -> default, set empty -> empty."""
    return os.environ.get(k, default)


def emulog(p):
    return os.path.join(os.environ["RUN"], f"p{p}", "PCSX2", "logs", "emulog.txt")


def lines(path, rx):
    try:
        return zrig.log_lines(path, rx)
    except OSError:
        return []


def main(argv):
    n_arg, seed = (argv + ["", ""])[:2]
    if "N" in os.environ and os.environ["N"] != n_arg:
        print(f"rbk.py: N={os.environ['N']} in the env is ignored: N is the 1st argument (rbk.py {os.environ['N']} [seed])")
        return 2
    if "SEED" in os.environ and os.environ["SEED"] != seed:
        print(f"rbk.py: SEED={os.environ['SEED']} in the env is ignored: seed is the 2nd argument (rbk.py <N> {os.environ['SEED']})")
        return 2
    zrig.take()
    zrig.need("RBKSTATES", "OUT", "RUN")
    out, run = os.environ["OUT"], os.environ["RUN"]
    n = int(n_arg or 4)
    if n not in (2, 4):
        print("N=2 or 4")
        return 2
    peers = [1, 3] if n == 2 else [1, 2, 3, 4]
    ggpo = zrig.env("GGPO", f"net=1,players={n},delay={opt('DELAY', '2')}")
    os.makedirs(out, exist_ok=True)
    relays = []
    lat = zrig.env("LAT")
    if lat:
        ggpo += ",relay=7200"
        zrig.kill_cmd(r"python.*udprelay[.]py")
        for i in range(n - 1):
            for j in range(i + 1, n):
                f = open(os.path.join(out, f"relay-{i}{j}.txt"), "wb")
                relays.append(subprocess.Popen(
                    [zrig.PY, "-u", os.path.join(zrig.TOOLS, "udprelay.py"), "--a", str(7200 + 8 * i + j),
                     "--b", str(7200 + 8 * j + i), "--p1", str(7001 + i), "--p2", str(7001 + j), "--delay", lat,
                     "--jitter", zrig.env("JITTER", "0"), "--loss", zrig.env("LOSS", "0"),
                     "--seed", str(10 * i + j + int(seed or 0)), "--seconds", "1200", "--idle", "10"],
                    stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT))

    def cleanup():
        for r in relays:
            r.kill()
        zrig.kill_name("pcsx2")
        for p in peers:
            try:
                shutil.copyfile(emulog(p), os.path.join(out, f"emulog-p{p}.txt"))
            except OSError:
                pass

    atexit.register(cleanup)
    t0 = time.time()
    turbo, select, battle, brief = opt("TURBO", "1"), opt("SELECT", "300"), opt("BATTLE", ""), opt("BRIEF", "1")
    tscale, ttime = opt("TSCALE", "4"), zrig.env("TIME", "90")
    for p in peers:
        ini = os.path.join(run, f"p{p}", "PCSX2", "inis", "PCSX2.ini")
        if os.path.isfile(ini + ".rbk0"):
            os.replace(ini + ".rbk0", ini)
        zrig.ini_set(ini, "Framerate", "TurboScalar", tscale or None)
        zrig.ini_set(ini, "EmuCore/Speedhacks", "vuThread", "true" if opt("MTVU", "") == "1" else None)
    ini0 = zrig.env("INI0")
    if ini0:
        ini = os.path.join(run, f"p{peers[0]}", "PCSX2", "inis", "PCSX2.ini")
        shutil.copy2(ini, ini + ".rbk0")
        for kv in ini0.split(";"):
            m = re.fullmatch(r"([^:]+):([^=]+)=(.*)", kv)
            if not m:
                print(f"rbk.py: INI0 entry '{kv}' is not Section:Key=Value")
                return 2
            zrig.ini_set(ini, m[1], m[2], m[3])
        print(f"p{peers[0]} INI0: {ini0.replace(';', ' ')}", flush=True)
    clamp = ";".join(x for x in (
        select and f"117f566,{select},2000,3600",
        brief and f"6f2b50,{brief},{int(brief) + 1},600",
        battle and f"838f66,{battle},{int(battle) + 1},{60 * int(ttime)}") if x)
    trace = opt("TRACE", "1") != "0"
    env0 = zrig.env("ENV0")
    penv = zrig.env("PCSX2_ENV")

    def assigns(s):
        return dict(a.split("=", 1) for a in (s or "").split() if "=" in a)

    threads = []
    for i, p in enumerate(peers):
        for f in (emulog(p), os.path.join(out, f"trace-p{p}.txt")):
            if os.path.exists(f):
                os.remove(f)
        e = {"ZDXSV_GGPO": ggpo, "ZDXSV_RBK": f"{i}/{n}", "ZDXSV_RBK_TIME": ttime,
             "ZDXSV_RBK_COUNT": zrig.env("COUNT", "1"), "ZDXSV_RBK_GAUGE": zrig.env("GAUGE", "1"),
             "ZDXSV_NET_TAIL": zrig.env("TAIL", "60")}
        if turbo:
            e["ZDXSV_RBK_TURBO"] = "1"
        if clamp:
            e["ZDXSV_EE_CLAMP"] = clamp
        if seed:
            e["ZDXSV_RAND_INPUT"] = str(int(seed) + i)
        if trace:
            e.update(ZDXSV_PW_HASH="1", ZDXSV_NET_TRACE=os.path.join(out, f"trace-p{p}.txt"))
        if zrig.env("PWDUMP"):
            e["ZDXSV_PW_DUMP"] = os.path.join(out, f"pw-p{p}.bin")
        e.update(assigns(penv))
        if i == 0:
            e.update(assigns(env0))
        kw = dict(state=os.path.join(os.environ["RBKSTATES"], f"rbk-p{p}.p2s"), extra_env=e)
        if zrig.env("SHOWN"):
            kw.update(nogui=True, renderer="3")
        else:
            kw["headless"] = True
        if i == 0 and zrig.env("SPEED0"):
            kw["speed"] = os.environ["SPEED0"]
        threads.append(threading.Thread(target=launch.launch, args=([p],), kwargs=kw))
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if zrig.env("STALL"):
        so = open(os.path.join(out, "stall.txt"), "w", encoding="utf-8")

        def say(l):
            so.write(l + "\n")
            so.flush()

        if zrig.WIN:
            subprocess.Popen(["powershell", "-NoProfile", "-File", os.path.join(HERE, "stall.ps1"), "-Peer",
                              f"p{peers[1]}", "-Ms", os.environ["STALL"]], stdout=so, stderr=subprocess.STDOUT)
        else:
            threading.Thread(target=zrig.stall, args=(f"p{peers[1]}", float(os.environ["STALL"]), say),
                             daemon=True).start()
    # each pcsx2 logs `rbk env` (1st vsync) with the knobs it received; stop at once on a mismatch.
    # ENV0/PCSX2_ENV may override knobs on purpose: those peers are only shown.
    for i, p in enumerate(peers):
        want = f"pos={i}/{n} rand={int(seed) + i if seed else ''} turbo={'1' if turbo else ''} clamp={clamp} ggpo={ggpo}"
        want = re.sub(r"=( |$)", r"=-\1", want)
        got = ""
        for _ in range(30):
            g = lines(emulog(p), r"rbk env ")
            if g:
                got = re.search(r"rbk env (.*)", g[0])[1].strip()
                break
            time.sleep(1)
        if penv or (i == 0 and env0):
            print(f"p{p} rbk env (not checked, ENV0/PCSX2_ENV): {got or 'none'}", flush=True)
        elif got != want:
            print(f"FAIL p{p} start check\n  want: {want}\n  got:  {got or 'no rbk env line in 30 s'}")
            return 1
    print(f"start check ok: {n} peers got the intended knobs (seed {seed or 'none'})", flush=True)
    maxs = float(zrig.env("MAXS", "900"))
    while time.time() - t0 < maxs:
        time.sleep(1)
        if zrig.count_name("pcsx2") == 0:
            break
    print(f"{round(time.time() - t0)} s: {zrig.count_name('pcsx2')} pcsx2 left", flush=True)
    ok = 0
    for p in peers:
        l = emulog(p)
        for x in lines(l, r"ZdxsvGgpo: (rbk position|net armed|rbk exit|net report)"):
            print(x[:160], flush=True)
        if not lines(l, r"rbk exit \(net battle end\)"):
            print(f"FAIL p{p}: no session end", flush=True)
            ok = 1
        w = len(lines(l, "MTVU speedhack is enabled"))
        if w:
            print(f"FAIL p{p}: {w} state saves/loads with MTVU on", flush=True)
            ok = 1
        if opt("MTVU", "") == "1" and not lines(l, "ZdxsvGgpo: MTVU speedhack off"):
            print(f"FAIL p{p}: MTVU=1 not turned off", flush=True)
            ok = 1
    pw = os.path.join(out, "pwcheck.txt")
    if not trace:
        print("TRACE=0: sync not checked", flush=True)
    else:
        a = [zrig.PY, os.path.join(zrig.TOOLS, "pwcheck.py")] + (["--own"] if opt("OWN", "1") == "1" else [])
        with open(pw, "w", encoding="utf-8") as f:
            subprocess.call(a + [os.path.join(out, f"trace-p{p}.txt") for p in peers], stdout=f)
        for x in lines(pw, r"^(common|player|rng)"):
            print(x[:160], flush=True)
    if relays:
        for _ in range(15):
            if all(r.poll() is not None for r in relays):
                break
            time.sleep(1)
        for i in range(n - 1):
            for j in range(i + 1, n):
                for x in lines(os.path.join(out, f"relay-{i}{j}.txt"), "relay end"):
                    print(x, flush=True)
    if trace:
        s, c = 0, 0
        for x in lines(pw, r"^(player|rng:|common)"):
            f = x.split()
            if f[0] == "player" and len(f) > 3 and int(re.match(r"-?\d*", f[1])[0] or 0) < n:
                s += float(f[3])
            elif f[0] == "rng:" and len(f) > 2:
                s += float(f[2])
            elif f[0] == "common" and len(f) > 2:
                c = float(f[2])
        if not (c > 0 and s == 0):
            print(f"FAIL coordinates (players < {n}) or RNG differ, or no frames", flush=True)
            ok = 1
    if zrig.env("STALL"):
        st = lines(os.path.join(out, "stall.txt"), ".")
        for x in st[-2:]:
            print(x, flush=True)
        m = [re.search(r"sync [0-9]* \(max [0-9.]* ms\)", x)[0]
             for x in lines(emulog(peers[0]), r"sync [0-9]* \(max [0-9.]* ms\)")]
        s = m[-1] if m else ""
        print(f"p{peers[0]} {s or 'no sync count'}", flush=True)
        f = s.split()
        if not (f and int(f[1]) >= 2 and float(f[3]) < 25):
            print("FAIL STALL: no time sync waits, or one longer than a frame", flush=True)
            ok = 1
        if not turbo and not zrig.env("LAT"):
            osd = lines(emulog(peers[0]), "osd frame")
            ahead = [int(x) for x in re.findall(r"\bP (-?\d+)", osd[-1])] if osd else []
            print(f"p{peers[0]} frames ahead at the end: {ahead}", flush=True)
            if not ahead or max(ahead) > MAX_AHEAD_AFTER_STALL:
                print("FAIL STALL: time sync did not bring the peer back", flush=True)
                ok = 1
    if zrig.env("MAXWAIT"):
        for p in peers:
            osd = lines(emulog(p), "osd frame")
            m = re.search(r"Wait (\d+)", osd[-1]) if osd else None
            print(f"p{p} prediction limit waits: {m[1] if m else 'none'}", flush=True)
            if not m or int(m[1]) > int(zrig.env("MAXWAIT")):
                print("FAIL MAXWAIT", flush=True)
                ok = 1
    if zrig.env("MAXLAG"):
        for p in peers:
            stamp = r"^\[\s*([0-9.]+)\]"
            marks = [(float(m[1]), int(m[2])) for x in lines(emulog(p), r"osd lag \dP \d")
                     if (m := re.search(stamp + r".*osd lag \dP (\d)", x))]
            end = [float(m[1]) for x in lines(emulog(p), r"net end msg of frame") if (m := re.search(stamp, x))]
            worst = None
            if marks and end:
                half = (marks[0][0] + end[0]) / 2
                worst = max([lv for t, lv in marks if t <= half][-1:] + [lv for t, lv in marks if half < t < end[0]])
            print(f"p{p} OSD Lag in the 2nd half of the battle: max {worst}", flush=True)
            if worst is None or worst > int(zrig.env("MAXLAG")):
                print("FAIL MAXLAG", flush=True)
                ok = 1
    if zrig.env("MAXLOSS"):
        for p in peers:
            counts = [int(c) for x in lines(emulog(p), "peer report") for pair in re.findall(r"loss_\d+=(\d+)/(\d+)", x) for c in pair]
            print(f"p{p} GGPO packet loss (peer report): {counts or 'none'}", flush=True)
            if not counts or max(counts) > int(zrig.env("MAXLOSS")):
                print("FAIL MAXLOSS", flush=True)
                ok = 1
    print(f"rbk N={n} seed={seed or 'none'}: {round(time.time() - t0)} s, {'PASS' if ok == 0 else 'FAIL'}", flush=True)
    return ok


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
