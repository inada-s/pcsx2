"""Client steps shared by m4.py, rbkprep.py, m4z.py (imported; prints the same lines the rigs check).
  fresh_boot(i): launch, fast boot + fast login (login API on $API) to 戦場選択 (ID 1 picked by fast boot;
    FB_LOBBY=0: fast boot stops at the user_id selection and drive.py idlogin picks ID 1).
  fresh_boot_all(ns): fresh_boot for all of them at once (4 clients ~30 s, one at a time ~2 min).
  fresh_entry(i): map -> Lobby 06 (press checked, retried once), side entry; waits for i entries in fake.log.
  speed_check(i): pine.py fps over 3 s; nearer a higher speed than SPEED (fps > 60*(SPEED+0.5)) -> False.
    Slower is printed, not failed (4 clients can be CPU-bound at 4x).
  entry2p_retry(i, route): 戦場選択 -> Lobby 02 (1v1) and its side dialog, see its doc.
Env: API, CARDS ("id1 id2 .."), RUN, OUT, PCSX2_ENV ("K=V .."), PCSX2_ENV_Pn, SPEED, SHOW ("1 3": windowed),
  LAUNCH_ARGS (more launch.py args), BOOT_WAIT (s from launch to the fast boot end, default 60),
  ENTRY_WAIT (s for the Lobby 06 press, default 20), DRIVE_RESERVE_MB (memgate reserve for drive.py, default 150).
"""
import atexit
import os
import re
import shutil
import subprocess
import threading
import time

import launch
import tokenlogin
import zrig

HERE = os.path.dirname(os.path.abspath(__file__))


def out(name):
    return os.path.join(os.environ["OUT"], name)


def count(path, rx):
    try:
        with open(path, "rb") as f:
            return sum(1 for l in f if re.search(rx, l.decode("utf-8", "replace")))
    except OSError:
        return 0


def assigns(s):
    return dict(a.split("=", 1) for a in (s or "").split() if "=" in a)


def say(line):
    if line:
        print(line, flush=True)


def drive(*args, quiet=False):
    """drive.py ARGS; prints (unless QUIET) and returns its last line (memgate lines dropped). Every python start
    waits for free memory (memgate.pth); with 3-4 pcsx2 up the default 800 MB held drive.py past the game's 116 s
    lobby timeout."""
    e = dict(os.environ, MEMGATE_RESERVE_MB=zrig.env("DRIVE_RESERVE_MB", "150"))
    r = zrig.run([zrig.PY, os.path.join(HERE, "drive.py")] + [str(a) for a in args], env=e)
    lines = [l for l in (r.stdout + r.stderr).splitlines() if "memgate" not in l]
    last = lines[-1] if lines else ""
    if not quiet:
        say(last)
    return last


def fresh_boot(i, say=print):
    tb = time.time()
    lobby = zrig.env("FB_LOBBY", "1")
    end = "user_id selection" if lobby == "0" else "lobby"
    e = {"ZDXSV_TLS": "0", "ZDXSV_FAST_BOOT": "1", "ZDXSV_FAST_BOOT_LOBBY": lobby, "ZDXSV_FAST_LOGIN": "1",
         "ZDXSV_LOGIN_API_URL": f"http://127.0.0.1:{os.environ['API']}/lbs/login"}
    e.update(assigns(zrig.env("PCSX2_ENV")))
    e.update(assigns(zrig.env(f"PCSX2_ENV_P{i}")))
    p = launch.parse(zrig.env("LAUNCH_ARGS").split())
    if p is None:
        say(f"FAIL launch p{i}")
        return False
    card = os.path.join(os.environ["RUN"], f"Mcd001-{os.environ['CARDS'].split()[i - 1]}.ps2")
    token, err = tokenlogin.card_token(os.path.join(os.environ["TRACE"], "zdxsv.db"), card, stand_in=True)
    if err:
        say(f"FAIL token p{i}: {err}")
        return False
    kw = p[1]
    kw.update(speed=zrig.env("SPEED", "1"), extra_env=e, quiet=True, memcard=card, token=token)
    if str(i) not in zrig.env("SHOW").split():
        kw["headless"] = True
    pid = launch.launch([i], **kw).get(i)
    if not pid:
        say(f"FAIL launch p{i}")
        return False
    say(f"p{i} pid {pid} slot {28010 + i}")
    log = os.path.join(os.environ["RUN"], f"p{i}", "PCSX2", "logs", "emulog.txt")
    hit = []
    for _ in range(int(zrig.env("BOOT_WAIT", "60"))):
        try:
            hit = zrig.log_lines(log, r"Zdxsv: fast boot: ")
        except OSError:
            hit = []
        if any(re.search(f"Zdxsv: fast boot: ({end} at|gave up|stopped)", l) for l in hit):
            break
        time.sleep(1)
    if not any(f"Zdxsv: fast boot: {end} at" in l for l in hit):
        say(f"FAIL client {i} boot: {hit[-1] if hit else ''}")
        return False
    if lobby == "0":
        say(drive("idlogin", i, quiet=True))
    fake = out("fake.log")
    say(f"client {i}: in {round(time.time() - tb)} s, {count(fake, 'login api')} logins, "
        f"{count(fake, 'conn from')} lobby conns")
    return True


def fresh_boot_all(ns):
    """fresh_boot for each client in NS at once; each one's lines printed together, in order."""
    lines = {i: [] for i in ns}
    ok = {}

    def one(i):
        ok[i] = fresh_boot(i, say=lines[i].append)

    ts = [threading.Thread(target=one, args=(i,)) for i in ns]
    for t in ts:
        t.start()
    for i, t in zip(ns, ts):
        t.join()
        for l in lines[i]:
            print(l, flush=True)
    return all(ok.get(i) for i in ns)


def _map_pressed(a0):
    for _ in range(4 * int(zrig.env("ENTRY_WAIT", "20"))):
        if count(out("fake.log"), r"C->S cat 01 cmd 0x6305") > a0:
            return True
        time.sleep(0.25)
    return False


def fresh_entry(i):
    fake = out("fake.log")
    a0 = count(fake, r"C->S cat 01 cmd 0x6305")
    drive("map", i)
    if not _map_pressed(a0):
        print(f"p{i}: no Lobby 06 press, C again", flush=True)
        drive("mappress", i)
        if not _map_pressed(a0):
            print(f"FAIL client {i} entry (no 0x6305)")
            return False
    drive("lobby2p", i)
    for _ in range(15):
        if count(fake, r"entry .* side") >= i:
            break
        time.sleep(2)
    n = count(fake, r"entry .* side")
    print(f"client {i}: {n} entered", flush=True)
    if n < i:
        print(f"FAIL client {i} entry")
        return False
    return True


def speed_check(i):
    r = zrig.run([zrig.PY, os.path.join(HERE, "pine.py"), "--slot", str(28010 + i), "fps", "3"])
    lines = (r.stdout + r.stderr).strip().splitlines()
    f = lines[-1].strip() if lines else ""
    s = zrig.env("SPEED", "1")
    if not re.fullmatch(r"[0-9.]+", f):
        print(f"p{i} fps ? ({f})", flush=True)
        return True
    fps = float(f)
    print(f"p{i} fps {f} = {fps / 60:.1f}x (SPEED {s})", flush=True)
    if fps > 60 * (float(s) + 0.5):
        print(f"FAIL p{i} runs at {fps / 60:.1f}x, not SPEED={s}: no {s}x result from this run")
        return False
    return True


def lobby_enters(lobby):
    """0x6305 EnterLobby requests into LOBBY ("00 02") in OUT/lobby.log."""
    try:
        with open(out("lobby.log"), "rb") as f:
            lines = [l.decode("utf-8", "replace") for l in f]
    except OSError:
        return 0
    # the body dump is on the next line, or on the same line after an escaped newline (JSON-quoted message)
    body = f"00000000  {lobby} "
    return sum(1 for a, b in zip(lines, lines[1:] + [""])
               if "C->S [Q] ID:0x6305" in a and (b.startswith(body) or "\\n" + body in a))


def entry2p_retry(i, route="map2p"):
    """Client I from 戦場選択 into Lobby 02 (1v1) and its side dialog. ROUTE: first try's route (map2p after
    login; remap2p when the map is already up, e.g. after a battle).
    Entry flake (3 of 15 entries): no 0x6305 EnterLobby after the map press, the client stays on the map
    polling 0x6303/04/08 and the 0x640E check fails ~4 min into the rig. Here: route, wait ENTRY2P_WAIT (12 s)
    for this client's 0x6305 `00 02`, else remap2p (walk again), ENTRY_TRIES (3) presses;
    a 0x6305 into another lobby fails (no exit route).
    ENTRY2P_FAULT=N: client N's first press is left out (the flake's state: on the map, no 0x6305)."""
    lobby = out("lobby.log")
    n0, a0 = lobby_enters("00 02"), count(lobby, r"C->S \[Q\] ID:0x6305")
    wait = int(zrig.env("ENTRY2P_WAIT", "12"))
    for k in range(1, int(zrig.env("ENTRY_TRIES", "3")) + 1):
        if k == 1 and zrig.env("ENTRY2P_FAULT") == str(i):
            drive("seq", i, zrig.env("ROUTE_MAP2P_NOC", "C,w5000,Up:3000,Left:6000,w500"), "map2pfault")
            print(f"p{i} map2p press left out (fault)", flush=True)
        else:
            drive(route, i)
        for _ in range(4 * wait):
            if lobby_enters("00 02") > n0:
                drive("lobby2p", i)
                return True
            if count(lobby, r"C->S \[Q\] ID:0x6305") > a0:
                print(f"client {i}: entered another lobby than 02", flush=True)
                return False
            time.sleep(0.25)
        print(f"client {i}: no 0x6305 Lobby 02 {wait} s after {route} (try {k}), walk again", flush=True)
        route = "remap2p"
    return False


def trace_log():
    """TRACE's lobby trace: lobby-trace.log (an older m4z output) or lobby.log."""
    t = os.environ["TRACE"]
    p = os.path.join(t, "lobby-trace.log")
    return p if os.path.isfile(p) else os.path.join(t, "lobby.log")


def servers(fake_args=(), relays=()):
    """Battle server (+ a `zdxsv relay` per RELAYS (addr, session, token)) and fake_lobby.py --trace TRACE on
    API; all stopped at exit with every pcsx2 / fake_lobby / drive / udprelay, emulogs copied to OUT."""
    zrig.need("ZDXSV_DIR", "TRACE", "CARDS", "IP", "OUT")
    run, o = os.path.abspath(os.environ["RUN"]), os.environ["OUT"]
    os.makedirs(o, exist_ok=True)
    os.environ.setdefault("API", "9881")
    os.environ.setdefault("BATTLEREG", os.path.join(run, "battlereg" + zrig.EXE))
    os.environ.update({"ZDXSV_BATTLE_ADDR": ":8210", "ZDXSV_BATTLE_RPC_ADDR": "127.0.0.1:3080",
                       "ZDXSV_BATTLE_PUBLIC_ADDR": f"{os.environ['IP']}:8210"})
    z = zrig.env("ZBIN") or os.path.join(os.environ["ZDXSV_DIR"], "bin", "zdxsv" + zrig.EXE)
    procs = []

    def start(args, log, cwd=None, env=None):
        with open(os.path.join(o, log), "wb") as f:
            procs.append(subprocess.Popen(args, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=f,
                                          stderr=subprocess.STDOUT))

    def cleanup():
        for p in procs:
            p.kill()
        zrig.kill_name("pcsx2")
        zrig.kill_cmd(r"(fake_lobby|drive|udprelay)[.]py")
        for i in (1, 2, 3, 4):
            try:
                shutil.copyfile(os.path.join(run, f"p{i}", "PCSX2", "logs", "emulog.txt"),
                                os.path.join(o, f"emulog-p{i}.txt"))
            except OSError:
                pass
            tf = os.path.join(run, f"p{i}", "PCSX2", "inis", "zdxsv_auth_token.txt")
            if os.path.exists(tf):
                os.remove(tf)

    atexit.register(cleanup)
    start([z, "battle"], "battle.log", cwd=run)
    for k, (addr, sess, tok) in enumerate(relays, 1):
        start([z, "relay", sess, tok], "relay.log" if k == 1 else f"relay{k}.log", cwd=run,
              env=dict(os.environ, ZDXSV_LOBBY_RELAY_ADDR=addr))
    time.sleep(2)
    start([zrig.PY, "-u", os.path.join(HERE, "fake_lobby.py"), "--trace", trace_log(), "--api", os.environ["API"],
           "--db", os.path.join(os.environ["TRACE"], "zdxsv.db"), "--battle", f"{os.environ['IP']}:8210",
           *fake_args], "fake.log")
    time.sleep(2)
    for i in (1, 2, 3, 4):
        try:
            os.remove(os.path.join(run, f"p{i}", "PCSX2", "logs", "emulog.txt"))
        except OSError:
            pass
    return procs
