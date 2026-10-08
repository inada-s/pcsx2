#!/usr/bin/env python3
"""Launch several local PCSX2 (zdxsv) instances side by side, like flycast's run.py. (AI-assisted)

    python run.py rom                  # N instances boot the game (online play / lobby testing)
    python run.py state                # N instances boot from STATE (a .p2s save state)
    python run.py rbk_test             # GGPO battle between the instances, no server: ZDXSV_RBK from
                                       # RBKSTATES/rbk-p1.p2s (downloaded when missing)
    python run.py rbk_test_random      # same with seeded random pad input (SEED)

For checking things by hand: the game runs with its own rules (time, rounds) and nothing is cut short.

Settings are environment variables (see the block below). ROM (the game's .iso) and BIOS (its directory)
have no default, e.g.
    ROM=/path/to/zgundam.iso BIOS=/path/to/ps2-bios python run.py rom
    N=2 python run.py rom
    N=4 SEED=7 python run.py rbk_test_random
    GDXSV=192.168.1.8 python run.py rom    # point the game's server hosts at a local zdxsv
    FRAME_LATENCY=0 python run.py rom      # maximum frame latency 0 (optimal frame pacing)
    LAT=100 JITTER=20 LOSS=0.05 N=4 python run.py rbk_test_random   # 100 ms one-way +-20 ms, 5 % loss

rbk_test ends with a sync check (tools/zdxsv/pwcheck.py on the players' work RAM hashes): IN SYNC or DESYNC.

Each instance gets its own data dir work/pcsx2-<i> (-datapath), so inis, memory cards, save states
and logs never mix with your normal PCSX2 setup. Instance 1's log is echoed to this terminal.
Closing instance 1 or Ctrl+C closes every instance.
"""
import configparser
import ctypes
import functools
import hashlib
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
from ctypes import wintypes
from pathlib import Path
from typing import List

print = functools.partial(print, flush=True)
ROOT = Path(__file__).resolve().parent
os.chdir(ROOT)

N = int(os.getenv("N", 2))
PCSX2 = Path(os.getenv("PCSX2", ROOT / "bin" / "pcsx2-qtx64-avx2.exe"))
# No defaults. Path() gives native separators: PCSX2 on Windows rejects ROM=G:/... as "does not exist".
ROM = str(Path(os.environ["ROM"])) if os.getenv("ROM") else ""
BIOS = Path(os.environ["BIOS"]) if os.getenv("BIOS") else None
BIOS_FILE = os.getenv("BIOS_FILE", "SCPH-50000_JPN_Con_0190_20030822_v10_[D9407AE8].rom0")
# Copied into an instance's memcards/ when it has no Mcd001.ps2 yet. Unset = PCSX2 makes an empty card.
MEMCARD = os.getenv("MEMCARD", "")
# Server address for the game's zdxsv hosts. Empty = keep the fork's default (zdxsv.net).
GDXSV = os.getenv("GDXSV", "")
STATE = os.getenv("STATE", "")
# Maximum frame latency ([EmuCore/GS] VsyncQueueSize): frames the emulation may run ahead of the
# display. 2 = PCSX2 default; 0 = optimal frame pacing (waits for the display every frame, least lag).
FRAME_LATENCY = int(os.getenv("FRAME_LATENCY", 2))
VOLUME = int(os.getenv("VOLUME", 6))  # % of the game's volume (PCSX2 default 100); also used for fast forward
# Instances whose pad 1 is bound to the first game controller (SDL-0, mapping below): "all" (default),
# one 1-based instance number, or 0 (none: PCSX2's keyboard defaults). PCSX2 reads the pad even when
# its window is in the background, so with "all" one controller drives every instance.
PAD = os.getenv("PAD", "all")
# Optional: take [InputSources], [Pad1..8], [Hotkeys] from this ini instead of the mapping below.
INPUT_INI = os.getenv("INPUT_INI", "")
# PCSX2's automatic mapping for an SDL game controller on pad 1 (DualShock2).
SDL_PAD1 = {
    "Up": "DPadUp", "Right": "DPadRight", "Down": "DPadDown", "Left": "DPadLeft",
    "Triangle": "FaceNorth", "Circle": "FaceEast", "Cross": "FaceSouth", "Square": "FaceWest",
    "Select": "Back", "Start": "Start", "L1": "LeftShoulder", "L2": "+LeftTrigger",
    "R1": "RightShoulder", "R2": "+RightTrigger", "L3": "LeftStick", "R3": "RightStick",
    "LUp": "-LeftY", "LRight": "+LeftX", "LDown": "+LeftY", "LLeft": "-LeftX",
    "RUp": "-RightY", "RRight": "+RightX", "RDown": "+RightY", "RLeft": "-RightX",
    "LargeMotor": "LargeMotor", "SmallMotor": "SmallMotor",
}
WORK = ROOT / "work"

# rbk_test: the menus up to the battle fast-forward, the battle runs at normal speed.
RBKSTATES = Path(os.getenv("RBKSTATES", WORK / "rbkstates"))
SEED = int(os.getenv("SEED", 1))  # rbk_test_random: instance i gets seed SEED + i
DELAY = int(os.getenv("DELAY", 2))  # GGPO input delay (frames)
# Network between the rbk_test instances: set LAT (one-way ms) to send each pair's GGPO packets through
# tools/zdxsv/udprelay.py with that delay, +-JITTER ms (uniform, reorders packets) and LOSS (0..1) per packet.
LAT = os.getenv("LAT", "")
JITTER = float(os.getenv("JITTER", 0))
LOSS = float(os.getenv("LOSS", 0))
RELAY_PORT = 7200  # remote position p of instance me at RELAY_PORT + 8 * me + p (ZDXSV_GGPO relay=)
# rbk_test sync check (default on): every instance hashes the players' work RAM per GGPO frame into
# work/pcsx2-<i>/trace.txt; tools/zdxsv/pwcheck.py compares them after the battle. SYNC_CHECK=0: off.
SYNC_CHECK = os.getenv("SYNC_CHECK", "1") == "1"

X_OFFSET = 0
Y_OFFSET = 50
W = 640
H = 480

ZDXSV_HOSTS = ["www01.kddi-mmbb.jp", "gate1.jp.dnas.playstation.org", "ca1202.mmcp6", "ca1203.mmcp6"]


def data_root(idx: int) -> Path:
    # PCSX2 puts its data in <datapath>/PCSX2.
    return WORK / f"pcsx2-{idx + 1}" / "PCSX2"


def log_path(idx: int) -> Path:
    return WORK / f"pcsx2-{idx + 1}" / "emulog.txt"


def prepare_instance(idx: int):
    """Write the settings each run needs into the instance's PCSX2.ini (other keys are kept)."""
    root = data_root(idx)
    (root / "inis").mkdir(parents=True, exist_ok=True)
    (root / "memcards").mkdir(parents=True, exist_ok=True)
    if MEMCARD and Path(MEMCARD).is_file() and not (root / "memcards" / "Mcd001.ps2").exists():
        shutil.copy(MEMCARD, root / "memcards" / "Mcd001.ps2")

    ini_path = root / "inis" / "PCSX2.ini"
    if not ini_path.exists():
        # Let PCSX2 write its full defaults (SettingsVersion, pad bindings, zdxsv network) and exit.
        # A hand-made partial ini makes it ask "Settings failed to load ... reset?" instead.
        subprocess.run([str(PCSX2), "-datapath", str(root.parent), "-testconfig"], cwd=PCSX2.parent, timeout=60)
        if not ini_path.exists():
            raise SystemExit(f"PCSX2 -testconfig did not create {ini_path}")
    ini = configparser.ConfigParser(interpolation=None, strict=False)
    ini.optionxform = str  # PCSX2 keys are case sensitive
    if ini_path.exists():
        ini.read(ini_path, encoding="utf-8-sig")

    def put(section: str, key: str, value):
        if not ini.has_section(section):
            ini.add_section(section)
        ini.set(section, key, str(value).lower() if isinstance(value, bool) else str(value))

    put("UI", "SetupWizardIncomplete", False)
    put("UI", "ConfirmShutdown", False)
    put("Folders", "Bios", BIOS)
    put("Filenames", "BIOS", BIOS_FILE)
    put("Logging", "EnableFileLogging", True)
    # PINE slot per instance, as zdxsv/launch.ps1 (28010 + instance number).
    put("EmuCore", "EnablePINE", True)
    put("EmuCore", "PINESlot", 28011 + idx)
    put("EmuCore/GS", "VsyncQueueSize", FRAME_LATENCY)
    put("SPU2/Output", "StandardVolume", VOLUME)
    put("SPU2/Output", "FastForwardVolume", VOLUME)
    if PAD == "all" or PAD == str(idx + 1):
        if INPUT_INI:
            src = configparser.ConfigParser(interpolation=None, strict=False)
            src.optionxform = str
            src.read(INPUT_INI, encoding="utf-8-sig")
            for sec in src.sections():
                if sec in ("InputSources", "Hotkeys") or (sec.startswith("Pad") and sec[3:].isdigit()):
                    if ini.has_section(sec):
                        ini.remove_section(sec)  # take the bindings as a whole, no leftovers
                    ini.add_section(sec)
                    for k, v in src.items(sec):
                        ini.set(sec, k, v)
        else:
            put("InputSources", "SDL", True)
            for button, sdl in SDL_PAD1.items():
                put("Pad1", button, f"SDL-0/{sdl}")
    if GDXSV:
        put("DEV9/Eth/Hosts", "Count", len(ZDXSV_HOSTS))
        for i, host in enumerate(ZDXSV_HOSTS):
            sec = f"DEV9/Eth/Hosts/Host{i}"
            put(sec, "Url", host)
            put(sec, "Desc", "zdxsv")
            put(sec, "Address", GDXSV)
            put(sec, "Enabled", True)

    with open(ini_path, "w", encoding="utf-8") as f:
        ini.write(f)


# --- window placement (Win32) ---
user32 = ctypes.windll.user32 if os.name == "nt" else None


def find_window(pid: int):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd) and user32.GetWindowTextLengthW(hwnd) > 0:
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def place_window(proc: subprocess.Popen, idx: int):
    """Tile the display windows 2 per row, like flycast's run.py."""
    if not user32:
        return
    for _ in range(120):
        if proc.poll() is not None:
            return
        hwnd = find_window(proc.pid)
        if hwnd:
            time.sleep(1)  # let Qt finish its own initial geometry first
            x = X_OFFSET + W * (idx % 2)
            y = Y_OFFSET + H * (idx // 2)
            user32.SetWindowPos(hwnd, None, x, y, W, H, 0x0004)  # SWP_NOZORDER
            return
        time.sleep(0.5)


# --- per-mode arguments and environment ---

def rbk_state(idx: int) -> Path:
    # N=2 plays p1 + p3 (as rbk.sh), N=4 plays p1..p4. A position without its own state uses
    # rbk-p1.p2s: ZDXSV_RBK=i/N, not the state, picks the battle position (one state serves all).
    p = {2: [1, 3], 4: [1, 2, 3, 4]}[N][idx]
    own = RBKSTATES / f"rbk-p{p}.p2s"
    return own if own.is_file() else RBKSTATES / "rbk-p1.p2s"


def rbk_env(idx: int, random_input: bool) -> dict:
    # No rule overrides (ZDXSV_RBK_TIME/COUNT/GAUGE, ZDXSV_EE_CLAMP): the battle keeps the rules saved
    # in the state.
    env = {
        "ZDXSV_GGPO": f"net=1,players={N},delay={DELAY}" + (f",relay={RELAY_PORT}" if LAT else ""),
        "ZDXSV_RBK": f"{idx}/{N}",
        "ZDXSV_LOBBY_STATE": "1",
    }
    if random_input:
        env["ZDXSV_RAND_INPUT"] = str(SEED + idx)
    if SYNC_CHECK:
        env["ZDXSV_PW_HASH"] = "1"
        env["ZDXSV_NET_TRACE"] = str(trace_path(idx))
    return env


def trace_path(idx: int) -> Path:
    return WORK / f"pcsx2-{idx + 1}" / "trace.txt"


def start_relays() -> List[subprocess.Popen]:
    """One udprelay.py per pair of instances (LAT set): i's packets to j go out through it and back."""
    relays = []
    for i in range(N):
        for j in range(i + 1, N):
            cmd = [sys.executable, "-u", str(ROOT / "tools" / "zdxsv" / "udprelay.py"),
                   "--a", str(RELAY_PORT + 8 * i + j), "--b", str(RELAY_PORT + 8 * j + i),
                   "--p1", str(7001 + i), "--p2", str(7001 + j), "--delay", LAT, "--jitter", str(JITTER),
                   "--loss", str(LOSS), "--seed", str(10 * i + j + SEED), "--seconds", "3600", "--idle", "10"]
            out = open(WORK / f"relay-{i + 1}{j + 1}.txt", "w")
            relays.append(subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT))
    print(f"relays: {len(relays)} (one-way {LAT} ms, jitter {JITTER} ms, loss {LOSS})")
    return relays


def report_sync():
    """pwcheck over the instances' traces: in sync = 0 mismatches for every player."""
    traces = [str(trace_path(i)) for i in range(N)]
    missing = [t for t in traces if not Path(t).is_file()]
    if missing:
        print(f"sync check: no trace {missing}")
        return
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "zdxsv" / "pwcheck.py"), "--own", *traces],
                       capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.startswith(("common", "player"))]
    print("\n".join(lines))
    common = next((int(l.split()[2]) for l in lines if l.startswith("common")), 0)
    bad = sum(int(l.split()[3]) for l in lines if l.startswith("player") and int(l.split()[1][:-1]) < N)
    print(f"sync check: {'IN SYNC' if common and not bad else 'DESYNC' if bad else 'no common frames'}"
          f" ({common} frames, {bad} mismatching player-frames)")


MODES = ("rom", "state", "rbk_test", "rbk_test_random")


def mode_args(mode: str, idx: int):
    """(extra pcsx2 args, extra env) for one instance."""
    if mode == "rom":
        return [], {}
    if mode == "state":
        return ["-statefile", STATE], {}
    return ["-statefile", str(rbk_state(idx))], rbk_env(idx, mode == "rbk_test_random")


RBK_STATE_URL = os.getenv("RBK_STATE_URL", "https://storage.googleapis.com/zdxsv/misc/rbk-p1.p2s")
# A save state holds the whole machine: a download is used only if it is the known file.
RBK_STATE_SHA256 = os.getenv("RBK_STATE_SHA256", "bd6db1b5ed73e3f8321161601a89846ea5fce125b3cf194e6cf938ef30f72fdb")


def download_rbk_state():
    """Fetch rbk-p1.p2s (serves every position) when RBKSTATES has none, like flycast's download_state."""
    dest = RBKSTATES / "rbk-p1.p2s"
    if dest.is_file():
        return
    RBKSTATES.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {RBK_STATE_URL} -> {dest}")
    tmp = dest.with_suffix(".part")  # no half-written state if the download fails
    try:
        urllib.request.urlretrieve(RBK_STATE_URL, tmp)
    except OSError as e:
        tmp.unlink(missing_ok=True)
        raise SystemExit(f"download failed: {e}")
    digest = hashlib.sha256(tmp.read_bytes()).hexdigest()
    if digest != RBK_STATE_SHA256.lower():
        tmp.unlink()
        raise SystemExit(f"download rejected: sha256 {digest}, expected {RBK_STATE_SHA256} (RBK_STATE_SHA256)")
    tmp.replace(dest)


def check(mode: str):
    problems = []
    if not PCSX2.is_file():
        problems.append(f"PCSX2 not found: {PCSX2} (build it with ./build-local.sh)")
    if not ROM:
        problems.append("set ROM to the game's .iso")
    elif not Path(ROM).is_file():
        problems.append(f"ROM not found: {ROM}")
    if BIOS is None:
        problems.append("set BIOS to the directory of the BIOS file")
    elif not (BIOS / BIOS_FILE).is_file():
        problems.append(f"BIOS not found: {BIOS / BIOS_FILE}")
    if mode == "state" and not Path(STATE).is_file():
        problems.append(f"STATE not found: '{STATE}'")
    if mode.startswith("rbk_test"):
        if N not in (2, 4):
            problems.append("rbk_test needs N=2 or N=4")
        else:
            missing = [str(rbk_state(i)) for i in range(N) if not rbk_state(i).is_file()]
            if missing:
                problems += [f"rbk state not found: {m}" for m in missing]
                problems.append(
                    "rbk_test starts from post-entry save states rbk-p1..p4.p2s (rbk-p1.p2s alone serves all\n"
                    f"positions; downloaded from {RBK_STATE_URL} when missing). Or set RBKSTATES.")
    if problems:
        raise SystemExit("\n".join(problems))


def tail(proc: subprocess.Popen, path: Path):
    for _ in range(60):  # pcsx2 creates the log a moment after start
        if path.exists() or proc.poll() is not None:
            break
        time.sleep(0.5)
    if not path.exists():
        return
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        while proc.poll() is None:
            line = f.readline()
            if line:
                print(line, end="")
            else:
                time.sleep(0.2)


def kill(proc: subprocess.Popen):
    if proc.poll() is None:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


def report_rbk():
    """Show from each log whether the GGPO session ran (start, connection, end)."""
    for i in range(N):
        text = log_path(i).read_text(encoding="utf-8", errors="replace") if log_path(i).exists() else ""
        print(f"--- instance {i + 1}")
        for l in text.splitlines():
            if "ZdxsvGgpo: " in l and any(k in l for k in (
                    "rbk position", "net armed", "net player", "net battle end frames", "rbk exit")):
                print(l[:160])


def exec_mode(mode: str):
    procs: List[subprocess.Popen] = []
    rbk = mode.startswith("rbk_test")
    relays: List[subprocess.Popen] = []
    try:
        if rbk and LAT:
            WORK.mkdir(parents=True, exist_ok=True)
            relays = start_relays()
        for i in range(N):
            prepare_instance(i)
            args, extra_env = mode_args(mode, i)
            log = log_path(i)
            log.unlink(missing_ok=True)
            trace_path(i).unlink(missing_ok=True)
            cmd = [str(PCSX2), "-datapath", str(data_root(i).parent), "-batch", "-nogui",
                   "-logfile", str(log), *args, "--", ROM]
            print(" ".join(f"{k}={v}" for k, v in extra_env.items()), subprocess.list2cmdline(cmd))
            p = subprocess.Popen(cmd, cwd=PCSX2.parent, env={**os.environ, **extra_env})
            procs.append(p)
            threading.Thread(target=place_window, args=(p, i), daemon=True).start()
            if i == 0:
                threading.Thread(target=tail, args=(p, log), daemon=True).start()

        # Until instance 1 is closed (rbk: or every instance has exited at its session end).
        while procs[0].poll() is None or (rbk and any(p.poll() is None for p in procs)):
            time.sleep(1)
    except KeyboardInterrupt:
        print("interrupted")
    finally:
        for p in procs + relays:
            kill(p)
        for i, p in enumerate(procs):
            print(f"instance {i + 1} exit {p.poll()}  log: {log_path(i)}")
    if rbk:
        report_rbk()
        if SYNC_CHECK:
            report_sync()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    mode = sys.argv[1]
    if mode not in MODES:
        raise SystemExit(f"unknown mode {mode}: {', '.join(MODES)}")
    if mode.startswith("rbk_test"):
        download_rbk_state()
    check(mode)
    exec_mode(mode)


if __name__ == "__main__":
    main()
