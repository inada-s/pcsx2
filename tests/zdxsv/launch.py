"""Start pcsx2 instance(s) N with data dir $RUN/pN/PCSX2 (PINE slot 28010+N); prints "pN pid <pid> slot S".
New dirs are cloned from p1 (inis, bios, memcards); p1 is set up per README.md.
  -N "1 2"         instances (default 1)
  -Memcard <file>  copies that card to memcards/Mcd001.ps2 first
  -StateFile <p2s> boots from that save state. A lobby state resumes online: DEV9 adopts the PS2's open
                   lobby connection, a stand-in (fake_lobby.py) answers.
  -LobbyState      ZDXSV_LOBBY_STATE=1 (off by default): needed to save AND load lobby states
  -Speed X         normal emulation speed X (ini [Framerate] NominalScalar, 1 = 60 fps, max 10; written
                   every launch). Routes wait on frames (pine.py wait), so they hold at any speed.
  -Headless        GS Null renderer (11) + -nogui, display window hidden; else -Renderer R (default -1, Auto).
  -NoGui
  -Eth             DEV9 Ethernet adapter on (ini [DEV9/Eth] EthEnable, written every launch; else off). Only
                   clients that play as a PS2 (no-DEV9 path off) need it.
Env defaults for pcsx2 (also when imported: launch()):
- ZDXSV_GGPO unset -> 0 (lobby battles on the battle server); ZDXSV_GGPO=default leaves GGPO on.
- ZDXSV_FAST_BOOT unset -> 0 (fast boot presses through the menus the routes drive); =default leaves it.
- Network pins, so a run never reaches the production server (also a release exe's built-in host):
  ZDXSV_FAST_LOGIN unset -> 1 (no DEV9 refuses DNAS: boottime.py seeds a token); ZDXSV_LOGIN_API_URL unset -> http://127.0.0.1:9881/lbs/login;
  ZDXSV_SERVER_HOST = $IP (online without DEV9, every name resolves to the box); DEV9=1 or no IP: empty
  (the DEV9 path with the ini's hosts, as a PS2). server_host() is the value a run gets.
  ZDXSV_AUTH_URL unset -> http://127.0.0.1:9881/auth; ZDXSV_AUTH_NO_BROWSER=1 always (sign-in URLs only logged).
  ZDXSV_AUTH_AT_DNAS unset -> 0 (no sign-in at DNAS without a token: back to the network menu).
- ZDXSV_PINE_ONLINE unset -> 1: PINE keeps answering while online (the routes drive the lobby and battles).
"""
import os
import re
import shutil
import subprocess
import sys

import zrig


def server_host(e=None):
    e = os.environ if e is None else e
    return "" if e.get("DEV9") == "1" else e.get("ZDXSV_SERVER_HOST") or e.get("IP") or ""


def pcsx2_env(base=None):
    e = dict(os.environ if base is None else base)
    for k in ("ZDXSV_GGPO", "ZDXSV_FAST_BOOT"):
        if not e.get(k):
            e[k] = "0"
        elif e[k] == "default":
            del e[k]
    e["ZDXSV_FAST_LOGIN"] = e.get("ZDXSV_FAST_LOGIN") or "1"
    e["ZDXSV_LOGIN_API_URL"] = e.get("ZDXSV_LOGIN_API_URL") or "http://127.0.0.1:9881/lbs/login"
    e["ZDXSV_SERVER_HOST"] = server_host(e)
    e["ZDXSV_AUTH_URL"] = e.get("ZDXSV_AUTH_URL") or "http://127.0.0.1:9881/auth"
    e["ZDXSV_AUTH_NO_BROWSER"] = "1"
    e["ZDXSV_AUTH_AT_DNAS"] = e.get("ZDXSV_AUTH_AT_DNAS") or "0"
    e["ZDXSV_PINE_ONLINE"] = e.get("ZDXSV_PINE_ONLINE") or "1"
    return e


def exe_path():
    exe = os.environ.get("PCSX2_EXE")
    if zrig.WIN:
        exe = exe or os.path.join(zrig.HERE, "..", "..", "bin", "pcsx2-qtx64.exe")
    else:
        zrig.need("PCSX2_EXE")
    if exe.endswith(".app"):
        exe = os.path.join(exe, "Contents", "MacOS", os.path.basename(exe)[:-4])
    return os.path.abspath(exe)


def _ini(path, slot, renderer, speed, eth):
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        lines = [l.rstrip("\r\n") for l in f]
    out, gs, fr, e9 = [], False, False, False
    for l in lines:
        if re.match(r"(NominalScalar|EthEnable) *=", l):
            continue
        if l.startswith("PINESlot ="):
            l = f"PINESlot = {slot}"
        elif l.startswith("Renderer = "):
            l = f"Renderer = {renderer}"
        gs |= l == "[EmuCore/GS]"
        out.append(l)
        if l == "[Framerate]":
            fr = True
            out.append(f"NominalScalar = {speed}")
        if l == "[DEV9/Eth]":
            e9 = True
            out.append(f"EthEnable = {eth}")
    if not gs:
        out += ["", "[EmuCore/GS]", f"Renderer = {renderer}"]
    if not fr:
        out += ["", "[Framerate]", f"NominalScalar = {speed}"]
    if not e9:
        out += ["", "[DEV9/Eth]", f"EthEnable = {eth}"]
    with open(path + ".new", "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    os.replace(path + ".new", path)
    # Each startup check of a tagged build spends one of GitHub's 60 unauthenticated API calls per hour per IP.
    zrig.ini_set(path, "AutoUpdater", "CheckAtStartup", "false")


def _hide_window(pid):
    zrig.ps("Add-Type -Namespace W -Name U -MemberDefinition '[DllImport(\"user32.dll\")] public static extern bool ShowWindow(System.IntPtr h, int c);'\n"
            f"for ($t = 0; $t -lt 60 -and (Get-Process -Id {pid}).MainWindowHandle -eq 0; $t++) {{ Start-Sleep -Milliseconds 500 }}\n"
            f"[void][W.U]::ShowWindow((Get-Process -Id {pid}).MainWindowHandle, 0)")


def launch(ns=(1,), memcard=None, state=None, speed="1", renderer="-1", nogui=False, headless=False,
           eth=False, lobby_state=False, extra_env=None, quiet=False, token=None):
    """Start instances NS; returns {i: pid}. Exits 1 on a bad probe, 75 when another rig holds the lock.
    TOKEN: written to inis/zdxsv_auth_token.txt (fast login); a new instance never takes p1's."""
    zrig.need("RUN", "ISO")
    e = dict(os.environ)
    e.update(extra_env or {})
    if lobby_state:
        e["ZDXSV_LOBBY_STATE"] = "1"
    e = pcsx2_env(e)
    root = os.path.abspath(os.environ["RUN"])
    exe = exe_path()
    tree = os.path.dirname(exe)
    while os.path.dirname(tree) != tree and not os.path.isdir(os.path.join(tree, "pcsx2", "Zdxsv")):
        tree = os.path.dirname(tree)
    stale = ["--exe", exe]
    if not os.path.isdir(os.path.join(tree, "pcsx2", "Zdxsv")):
        # an exe copied out of its tree (A/B): this tree's mtimes say nothing about its build
        tree, stale = os.path.normpath(os.path.join(zrig.HERE, "..", "..")), []
    lint = zrig.run([zrig.PY, os.path.join(zrig.HERE, "probelint.py")] + stale + [tree])
    if lint.returncode:
        print((lint.stdout + lint.stderr).rstrip(), flush=True)
        sys.exit(1)
    if not zrig.lock_check():
        sys.exit(75)
    pids = {}
    for i in ns:
        i = int(i)
        d = os.path.join(root, f"p{i}")
        pd = os.path.join(d, "PCSX2")
        if not os.path.isdir(os.path.join(pd, "inis")):
            os.makedirs(pd, exist_ok=True)
            for sub in ("inis", "bios", "memcards"):
                shutil.copytree(os.path.join(root, "p1", "PCSX2", sub), os.path.join(pd, sub), dirs_exist_ok=True)
            tf = os.path.join(pd, "inis", "zdxsv_auth_token.txt")
            if os.path.exists(tf):
                os.remove(tf)
        slot = 28010 + i
        _ini(os.path.join(pd, "inis", "PCSX2.ini"), slot, "11" if headless else renderer, speed,
             "true" if eth else "false")
        if memcard:
            shutil.copyfile(memcard, os.path.join(pd, "memcards", "Mcd001.ps2"))
        if token:
            with open(os.path.join(pd, "inis", "zdxsv_auth_token.txt"), "w") as f:
                f.write(token + "\n")
        a = [exe, "-datapath", d, "-batch"]
        if nogui or headless:
            a.append("-nogui")
        if state:
            a += ["-statefile", os.path.abspath(state)]
        a += ["--", os.path.normpath(os.environ["ISO"])]  # pcsx2 refuses a game path with / on Windows
        p = subprocess.Popen(a, env=e, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=not zrig.WIN)
        if headless and zrig.WIN:
            _hide_window(p.pid)
        pids[i] = p.pid
        if not quiet:
            print(f"p{i} pid {p.pid} slot {slot}", flush=True)
    return pids


def parse(argv):
    """launch.py arguments -> (instances, launch() keywords); None on an unknown argument."""
    kw, ns, i = {}, "1", 0
    flags = {"-LobbyState": "lobby_state", "-NoGui": "nogui", "-Headless": "headless", "-Eth": "eth"}
    vals = {"-Memcard": "memcard", "-StateFile": "state", "-Speed": "speed", "-Renderer": "renderer"}
    while i < len(argv):
        a = argv[i]
        if a == "-N":
            ns, i = argv[i + 1], i + 1
        elif a in flags:
            kw[flags[a]] = True
        elif a in vals:
            kw[vals[a]], i = argv[i + 1], i + 1
        else:
            print(f"FAIL launch.py: unknown argument {a}")
            return None
        i += 1
    return ns.split(), kw


def main(argv):
    p = parse(argv)
    if p is None:
        return 2
    launch(p[0], **p[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
