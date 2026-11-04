"""Shared layer of the rig scripts: settings (env + local.env), OS calls, the rig lock.

Windows (native python) or macOS/Linux, stdlib only. `import zrig` loads local.env into os.environ
(the environment wins); zrig.need("RUN") fails the script (exit 2) when a setting is missing.
"""
import atexit
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WIN = os.name == "nt"
EXE = ".exe" if WIN else ""
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "zdxsv"))  # pwcheck.py, udprelay.py
PGA = ["-a"] if sys.platform == "darwin" else []  # macOS pgrep hides its own ancestors (this script) without -a


def _load_env():
    p = os.path.join(HERE, "local.env")
    if not os.path.isfile(p):
        return
    with open(p, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            k, sep, v = line.partition("=")
            if sep and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k) and k not in os.environ:
                os.environ[k] = v


_load_env()
# python with PIL (windowed snapshots); not $PY, which a CI runner may set to a python without it
PY = os.environ.get("ZDXSV_PY") or sys.executable
GOEXE = os.environ.get("GOEXE", "go")


def env(k, default=""):
    return os.environ.get(k) or default


def export(assigns):
    """Put "K=V K2=V2" (a rig's optional env argument) into os.environ."""
    for a in (assigns or "").split():
        k, sep, v = a.partition("=")
        if sep:
            os.environ[k] = v


def log_lines(path, *needles):
    """Lines of a log (bytes, any encoding) containing one of NEEDLES (regex), the timestamp kept."""
    with open(path, "rb") as f:
        lines = [l.decode("utf-8", "replace").rstrip("\r\n") for l in f]
    return [l for l in lines if any(re.search(n, l) for n in needles)]


def need(*names):
    for n in names:
        if not os.environ.get(n):
            print(f"FAIL {os.path.basename(sys.argv[0])}: set {n} (environment or tests/zdxsv/local.env, see README.md)", flush=True)
            sys.exit(2)


def run(args, **kw):
    """Run, capture text output; never raises on a missing program (rc 127)."""
    try:
        return subprocess.run(args, capture_output=True, text=True, errors="replace", **kw)
    except OSError as e:
        return subprocess.CompletedProcess(args, 127, "", str(e))


def ps(cmd):
    return run(["powershell", "-NoProfile", "-Command", cmd]).stdout.replace("\r", "")


# --- processes ---

def _procs():
    """[(pid, ppid, name, cmdline)] of every process."""
    out = []
    if WIN:
        txt = ps("Get-CimInstance Win32_Process | % { \"$($_.ProcessId)`t$($_.ParentProcessId)`t$($_.Name)`t$($_.CommandLine)\" }")
        for line in txt.splitlines():
            f = line.split("\t", 3)
            if len(f) == 4 and f[0].isdigit():
                out.append((int(f[0]), int(f[1] or 0), f[2], f[3]))
        return out
    txt = run(["ps", "-axww", "-o", "pid=,ppid=,comm=,args="]).stdout
    if txt:
        for line in txt.splitlines():
            m = re.match(r"\s*(\d+)\s+(\d+)\s+(\S+)\s+(.*)", line)
            if m:
                out.append((int(m[1]), int(m[2]), os.path.basename(m[3]), m[4]))
        return out
    # ps is denied under some runners: pgrep for pid + args (ppid: ppid())
    for line in run(["pgrep"] + PGA + ["-lf", "."]).stdout.splitlines():
        p, _, a = line.partition(" ")
        if p.isdigit():
            out.append((int(p), 0, os.path.basename(a.split(" ")[0]), a))
    names = {}
    for line in run(["pgrep"] + PGA + ["-l", "."]).stdout.splitlines():
        p, _, n = line.partition(" ")
        if p.isdigit():
            names[int(p)] = n
    out = [(p, q, names.get(p, n), a) for p, q, n, a in out]
    return out


def alive(pid):
    pid = int(pid or 0)
    if pid <= 0:
        return False
    if WIN:
        return any(p == pid for p, *_ in _procs())
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def name_of(pid):
    for p, _, n, _ in _procs():
        if p == int(pid):
            return n
    return ""


def waitgone(pid, secs):
    t = time.time()
    while time.time() - t < secs and alive(pid):
        time.sleep(0.5)


def kill(*pids):
    for p in pids:
        p = int(p)
        if WIN:
            run(["taskkill", "/F", "/PID", str(p)])
        else:
            try:
                os.kill(p, signal.SIGKILL)
            except OSError:
                pass


def pids_name(*names):
    """Pids whose executable name starts with one of NAMES (any case), never this process."""
    low = [n.lower() for n in names]
    return [p for p, _, n, _ in _procs() if p != os.getpid() and any(n.lower().startswith(x) for x in low)]


def kill_name(*names):
    """Kill and wait up to 10 s for the pids to be gone: taskkill /F returns before the files they hold are free."""
    ps_ = pids_name(*names)
    kill(*ps_)
    end = time.time() + 10
    while ps_ and time.time() < end and set(ps_) & set(pids_name(*names)):
        time.sleep(0.2)
    return ps_


def count_name(*names):
    return len(pids_name(*names))


def pids_cmd(rx, notrx=None):
    """Pids whose command line matches regex RX (and not NOTRX), never this process or its parents."""
    me = {os.getpid(), os.getppid()}
    return [p for p, _, _, a in _procs()
            if p not in me and re.search(rx, a) and not (notrx and re.search(notrx, a))
            and "Get-CimInstance" not in a]


def kill_cmd(rx, notrx=None):
    ps_ = pids_cmd(rx, notrx)
    kill(*ps_)
    return ps_


def ppid(pid):
    """Parent pid, 0 when unknown (ps can be denied: libproc on macOS, /proc on Linux)."""
    pid = int(pid)
    if sys.platform == "darwin":
        import ctypes
        buf = ctypes.create_string_buffer(136)  # struct proc_bsdinfo; pbi_ppid at offset 16
        lib = ctypes.CDLL("/usr/lib/libproc.dylib")
        if lib.proc_pidinfo(pid, 3, ctypes.c_uint64(0), buf, 136) == 136:  # PROC_PIDTBSDINFO
            return int.from_bytes(buf.raw[16:20], "little")
        return 0
    if not WIN:
        try:
            with open(f"/proc/{pid}/stat") as f:
                return int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            return 0
    return next((q for p, q, _, _ in _procs() if p == pid), 0)


def process_start(pid):
    """Epoch seconds of the process creation, None when unknown."""
    import ctypes
    pid = int(pid)
    if WIN:
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return None
        t = [ctypes.c_ulonglong() for _ in range(4)]
        ok = k.GetProcessTimes(h, *[ctypes.byref(x) for x in t])
        k.CloseHandle(h)
        return t[0].value / 1e7 - 11644473600 if ok else None
    if sys.platform == "darwin":
        buf = ctypes.create_string_buffer(136)  # struct proc_bsdinfo: pbi_start_tvsec/usec at 120/128
        if ctypes.CDLL("/usr/lib/libproc.dylib").proc_pidinfo(pid, 3, ctypes.c_uint64(0), buf, 136) != 136:
            return None
        return int.from_bytes(buf.raw[120:128], "little") + int.from_bytes(buf.raw[128:136], "little") / 1e6
    try:
        with open(f"/proc/{pid}/stat") as f:
            ticks = int(f.read().rsplit(")", 1)[1].split()[19])
        with open("/proc/stat") as f:
            btime = next(int(l.split()[1]) for l in f if l.startswith("btime "))
        return btime + ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError, StopIteration):
        return None


def ancestors(pid=None):
    """Parent chain of PID (default this process), nearest first, without pid 0/1."""
    if WIN:
        pp = {p: q for p, q, _, _ in _procs()}
        get = lambda p: pp.get(p, 0)
    else:
        get = ppid
    a, p = [], get(pid or os.getpid())
    while p > 1 and p not in a:
        a.append(p)
        p = get(p)
    return a


def stop(pid):
    if WIN:
        ps("Add-Type 'using System; using System.Runtime.InteropServices; public static class Nt { [DllImport(\"ntdll.dll\")] public static extern int NtSuspendProcess(IntPtr h); }'; "
           f"[void][Nt]::NtSuspendProcess((Get-Process -Id {pid}).Handle)")
    else:
        os.kill(int(pid), signal.SIGSTOP)


def cont(pid):
    if WIN:
        ps("Add-Type 'using System; using System.Runtime.InteropServices; public static class Nr { [DllImport(\"ntdll.dll\")] public static extern int NtResumeProcess(IntPtr h); }'; "
           f"[void][Nr]::NtResumeProcess((Get-Process -Id {pid}).Handle)")
    else:
        os.kill(int(pid), signal.SIGCONT)


def lanip():
    """First IPv4 address that is not loopback or link-local."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # no packet: picks the route's source address
        ip = s.getsockname()[0]
        if not ip.startswith(("127.", "169.254.")):
            return ip
    except OSError:
        pass
    finally:
        s.close()
    if WIN:
        return ps("(Get-NetIPAddress -AddressFamily IPv4 | ? { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.*' } | select -First 1).IPAddress").strip()
    for m in re.finditer(r"inet (\d+\.\d+\.\d+\.\d+)", run(["ifconfig"]).stdout):
        if not m[1].startswith(("127.", "169.254.")):
            return m[1]
    return "127.0.0.1"


def remote(*pids):
    """Remote ip:port of the TCP connections of these processes."""
    out = []
    for p in pids:
        if WIN:
            out += ps(f"Get-NetTCPConnection -OwningProcess {p} -EA 0 | % {{ $_.RemoteAddress + ':' + $_.RemotePort }}").split()
        else:
            for line in run(["lsof", "-nP", "-a", "-p", str(p), "-iTCP"]).stdout.splitlines()[1:]:
                f = line.split()
                if len(f) > 8 and "->" in f[8]:
                    out.append(f[8].split("->", 1)[1])
    return out


# --- pcsx2 window control (pcsx2ctl.ps1 / pcsx2ctl.js) ---

def ctl(pid, seq, stdout=None):
    """pcsx2ctl -Seq SEQ to that pcsx2 (pid 0: the first one); returns the rc."""
    if WIN:
        a = ["powershell", "-NoProfile", "-File", os.path.join(HERE, "pcsx2ctl.ps1")]
        if int(pid):
            a += ["-ProcId", str(pid)]
        return subprocess.call(a + ["-Seq", seq], stdout=stdout, stderr=stdout and subprocess.STDOUT)
    if not int(pid):
        ps_ = pids_name("pcsx2")
        pid = ps_[0] if ps_ else 0
    a = ["osascript", "-l", "JavaScript", os.path.join(HERE, "pcsx2ctl.js"), "-pid", str(pid)]
    for p, _, _, cmd in _procs():
        m = re.search(r" -datapath (.*) -batch", cmd)
        if p == int(pid) and m:
            a += ["-data", m[1]]
    return subprocess.call(a + ["-seq", seq], stdout=stdout, stderr=stdout and subprocess.STDOUT)


def ctl_check(ini, seq, stdout=None):
    if WIN:
        return subprocess.call(["powershell", "-NoProfile", "-File", os.path.join(HERE, "pcsx2ctl.ps1"), "-Check", "-Ini", ini, "-Seq", seq], stdout=stdout, stderr=stdout and subprocess.STDOUT)
    return subprocess.call(["osascript", "-l", "JavaScript", os.path.join(HERE, "pcsx2ctl.js"), "-check", "-ini", ini, "-seq", seq], stdout=stdout, stderr=stdout and subprocess.STDOUT)


# --- rig lock ---
# One pcsx2/zdxsv rig at a time: rigs share fixed ports (7001.., 8200.., 28010..) and kill pcsx2*/zdxsv*
# at start, so a 2nd rig wrecks the 1st. $RUN/rig.lock = "<owner pid> <time> <what>". A nested rig
# (RIG_OWNER exported by the outer one) and a process under the owner pass.

ORPHANS = ("pcsx2", "zdxsv", "dnas", "zproxy")
ORPHAN_CMD = r"python.*(udprelay|fake_lobby|drive)[.]py"


def _lock_path():
    need("RUN")
    return os.path.join(os.environ["RUN"], "rig.lock")


def _read_lock():
    try:
        with open(_lock_path(), encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return 0, ""
    line = lines[0] if lines else ""
    return (int(line.split(" ", 1)[0]) if line[:1].isdigit() else 0), line


def _rig_alive(pid):
    if not alive(pid):
        return False
    n = name_of(pid).lower()
    return not n or n.startswith(("python", "bash"))


def lock_check():
    """True unless another live rig holds the lock (a caller under its owner passes)."""
    owner, line = _read_lock()
    if not owner or str(owner) == env("RIG_OWNER") or not _rig_alive(owner):
        return True
    mine = {os.getpid(), *ancestors()}
    if owner in mine:
        return True
    print(f"rig busy: {line}\n  wait for it, or end it: kill {owner} (then its children)", flush=True)
    return False


def take(what=None):
    """Take the rig lock for this script (exit 75 when a live rig holds it); released at exit."""
    me = os.getpid()
    what = what or " ".join([os.path.basename(sys.argv[0])] + sys.argv[1:])
    outer = env("RIG_OWNER")
    if outer and outer.isdigit() and _rig_alive(int(outer)):
        return
    owner, line = _read_lock()
    if owner and owner != me and _rig_alive(owner):
        print(f"rig busy: {line}\n  wait for it, or end it: kill {owner} (then its children)", flush=True)
        print(f"FAIL {os.path.basename(sys.argv[0])}: rig busy", flush=True)
        sys.exit(75)
    orph = sorted(set(pids_name(*ORPHANS) + pids_cmd(ORPHAN_CMD)))
    if orph:
        print(f"riglock: killing orphans of a dead rig ({line}): {' '.join(map(str, orph))}", flush=True)
        kill(*orph)
        for p in orph:
            waitgone(p, 20)
    os.makedirs(os.environ["RUN"], exist_ok=True)
    with open(_lock_path(), "w", encoding="utf-8") as f:
        f.write(f"{me} {time.strftime('%m-%d %H:%M:%S')} {what}\n")
    os.environ["RIG_OWNER"] = str(me)
    atexit.register(release)
    for s in (signal.SIGTERM,) + ((signal.SIGHUP,) if not WIN else ()):
        signal.signal(s, lambda *_: sys.exit(143))


def release():
    if _read_lock()[0] == os.getpid():
        try:
            os.remove(_lock_path())
        except OSError:
            pass


def ini_set(path, section, key, value=None):
    """Drop KEY from [SECTION] of a pcsx2 ini; VALUE set: write `KEY = VALUE` right after the header (added
    when missing). Line ends kept as found (CRLF on Windows)."""
    with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
        lines = f.read().splitlines(keepends=True)
    nl = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
    out, cur, done = [], None, False
    for l in lines:
        s = l.rstrip("\r\n")
        if s.startswith("["):
            cur = s
        if cur == f"[{section}]" and re.match(rf"{re.escape(key)} *=", s):
            continue
        out.append(l)
        if s == f"[{section}]" and value is not None and not done:
            out.append(f"{key} = {value}{nl}")
            done = True
    if value is not None and not done:
        if out and not out[-1].endswith("\n"):
            out.append(nl)
        out += [nl, f"[{section}]{nl}", f"{key} = {value}{nl}"]
    with open(path + ".new", "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write("".join(out))
    os.replace(path + ".new", path)


def stall(peer, ms, say=print):
    """Suspend the pcsx2 of data dir PEER (p3) for MS every 5 s, 8 times from 40 s."""
    p = None
    for _ in range(120):
        ps_ = pids_cmd(rf"-datapath [^ ]*[/\\\\]{peer}( |$)")
        if ps_:
            p = ps_[0]
            break
        time.sleep(0.5)
    if not p:
        say(f"stall: no pcsx2 for {peer}")
        return
    say(f"stall: {peer} pid {p}, first stall in 40 s")
    time.sleep(40)
    for k in range(8):
        if not alive(p):
            say(f"stall: {peer} exited after {k} stalls")
            return
        t = time.time()
        stop(p)
        time.sleep(ms / 1000)
        cont(p)
        say(f"stall {k} {round((time.time() - t) * 1000)} ms")
        time.sleep(5)
