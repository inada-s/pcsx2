"""PINE client for this pcsx2 (TCP 127.0.0.1:<PINESlot> on Windows; elsewhere the Unix socket
$TMPDIR/pcsx2.sock on macOS, $XDG_RUNTIME_DIR/pcsx2.sock on Linux, else /tmp, `.<slot>` after it unless 28011).

Pad input uses the fork's opcode 0x30 (pad u8, bind u8, value u8); the rest is stock PINE.

  pine.py --slot 28011 status
  pine.py --slot 28011 seq "Down*7,C,w3000"     # same syntax as pcsx2ctl.ps1
  pine.py --slot 28011 r32 0x00100000
  pine.py --slot 28011 save 1 | load 1
"""
import argparse
import os
import socket
import struct
import sys
import time

# pcsx2/SIO/Pad/PadDualshock2.h Inputs
BIND = {
    "Up": 0, "Right": 1, "Down": 2, "Left": 3, "T": 4, "C": 5, "X": 6, "Z": 7,
    "Select": 8, "Start": 9, "L1": 10, "L2": 11, "R1": 12, "R2": 13, "L3": 14, "R3": 15,
}
# pcsx2ctl.ps1 key names -> pad binds
ALIAS = {"Return": "Start", "S": "T", "Backspace": "Select", "Q": "L1", "E": "R1", "1": "L2", "3": "R2"}
FPS = 60  # NTSC vsyncs per game second (pine.py fps at 1x: 60.0)
WALL = os.environ.get("PINE_WALL") == "1"
TRACE = os.environ.get("PINE_TRACE")
STALL_S = 120  # wait(): frame count not moving this long = emulator hung or paused
# Game RAM values for @name=V (SLPS-25419): (pointer or None, address
# or offset). scene = top scene G[0] (1 title, 2 main menu, 7 通信対戦); net = network menu state (1 warning,
# 2 NET file, 3 connection menu, 4 ネットワーク接続, 5 terms, 6 netconf list, 0x21 connect); step = lobby
# connection step (4 = ID選択); menu = main menu cursor; lstate = server menu screen (via the network menu:
# 8 TOP, 9 ID選択, 10 after the ID pick, 11 戦場選択; via fast boot: 30, 31, 0, 1).
PROBE = {
    "scene": (0x6D7A8C, 0), "net": (None, 0xCD1360), "nsub": (None, 0xCD1361),
    "step": (0x6D7D54, 0xA1F), "phase": (None, 0x6D7D84), "menu": (0x6D7954, 0x25C),
    "lstate": (None, 0xCE28B0),
}
# f32 game RAM for Key^name>=V: 戦場選択 map cursor in screen px (y grows down), (270, 190) when the map opens
FPROBE = {"curx": 0xC56504, "cury": 0xC56508}
UNTIL_MS = 60000  # @name=V default limit, game time


def shrink(path, scale):
    """Resize a fresh snapshot in place (the GS thread may still hold the file)."""
    from PIL import Image
    for _ in range(20):
        try:
            im = Image.open(path)
            im.load()
            im.resize((int(im.width * scale), int(im.height * scale))).save(path)
            return path
        except OSError:
            time.sleep(0.3)
    return path


class Pine:
    def __init__(self, slot=28011, host="127.0.0.1", timeout=5.0, connect_s=30.0):
        # launch.py returns before the server listens: retry a refused or missing socket
        self.slot, self.traced = slot, None
        end = time.time() + connect_s
        while True:
            try:
                self.sock = self._connect(slot, host, timeout)
                return
            except (ConnectionRefusedError, FileNotFoundError):
                if time.time() > end:
                    raise
                time.sleep(0.2)

    @staticmethod
    def _connect(slot, host, timeout):
        if os.name == "nt":
            return socket.create_connection((host, slot), timeout=timeout)
        d = os.environ.get("TMPDIR" if sys.platform == "darwin" else "XDG_RUNTIME_DIR") or "/tmp"
        path = os.path.join(d, "pcsx2.sock") + ("" if slot == 28011 else ".%d" % slot)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect(path)
        except OSError:
            s.close()
            raise
        return s

    def call(self, op, args=b""):
        msg = struct.pack("<IB", 5 + len(args), op) + args
        self.sock.sendall(msg)
        hdr = self._recv(4)
        (size,) = struct.unpack("<I", hdr)
        body = self._recv(size - 4)
        if body[0] != 0:
            raise RuntimeError("PINE op 0x%x failed" % op)
        return body[1:]

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("PINE closed")
            buf += chunk
        return buf

    def r8(self, a): return self.call(0, struct.pack("<I", a))[0]
    def r16(self, a): return struct.unpack("<H", self.call(1, struct.pack("<I", a)))[0]
    def r32(self, a): return struct.unpack("<I", self.call(2, struct.pack("<I", a)))[0]
    def w8(self, a, v): self.call(4, struct.pack("<IB", a, v))
    def w32(self, a, v): self.call(6, struct.pack("<II", a, v))
    def save(self, slot): self.call(9, bytes([slot]))
    def load(self, slot): self.call(0xA, bytes([slot]))
    def status(self): return ["running", "paused", "shutdown"][struct.unpack("<I", self.call(0xF))[0]]
    def frame(self): return struct.unpack("<I", self.call(0x32))[0]
    def title(self): return self.call(0xB)[4:].split(b"\0")[0].decode("utf-8", "replace")

    def snap(self, path, wait_s=10):
        """GS screenshot to path (png); waits for the file. pcsx2 writes only a bare name into its
        snapshots folder and returns that path; the file is moved to path."""
        p = ("pine-%d-%s" % (os.getpid(), os.path.basename(path))).encode()
        if os.path.exists(path):
            os.remove(path)
        src = self.call(0x31, struct.pack("<H", len(p)) + p)[4:].split(b"\0")[0].decode()
        size = 0
        for _ in range(int(wait_s * 5)):
            time.sleep(0.2)
            s = os.path.getsize(src) if os.path.exists(src) else 0
            if s > 0 and s == size:  # macOS: a file still being written can be renamed
                break
            size = s
        else:
            raise TimeoutError("no snapshot " + src)
        for _ in range(50):  # the GS thread may still hold the file
            try:
                os.replace(src, path)
                return path
            except PermissionError:
                time.sleep(0.2)
        raise TimeoutError("snapshot still in use " + src)

    def pad(self, bind, value, port=0):
        self.call(0x30, bytes([port, bind, value]))

    def wait(self, ms):
        """Wait ms of game time: ms*60/1000 vsyncs (g_FrameCount), so routes keep their timing at any
        emulation speed (launch.py -Speed). PINE_WALL=1: wall-clock sleep (old)."""
        if WALL:
            time.sleep(ms / 1000)
            return
        n = max(1, round(ms * FPS / 1000))
        last = self.frame()
        done, still = 0, time.time()
        while done < n:
            # <= 10x speed (600 vsync/s): sleep about half the time the rest takes, 2 ms at the end
            time.sleep(min(0.1, max(0.002, (n - done) / 1200)))
            self.trace()
            f = self.frame()
            if f > last:  # a state load / rollback that moves the counter back adds nothing
                done += min(f - last, n - done)
                still = time.time()
            elif time.time() - still > STALL_S:
                raise TimeoutError("frame count stuck at %d for %d s" % (f, STALL_S))
            last = f

    def probe(self, name):
        """u8 of a PROBE name, or u8:0xADDR; None while its pointer is not set up or the VM is not up
        (PINE refuses reads before the game runs)."""
        try:
            if name.startswith("u8:"):
                return self.r8(int(name[3:], 0))
            ptr, off = PROBE[name]
            if ptr is None:
                return self.r8(off)
            base = self.r32(ptr)
            return self.r8(base + off) if 0x100000 <= base < 0x2000000 - off else None
        except RuntimeError:
            return None

    def trace(self, tok=None):
        """PINE_TRACE=<file>: append slot, frame, the seq token and every PROBE value when one changes or a
        token starts."""
        if not TRACE:
            return
        v = tuple(self.probe(k) for k in PROBE)
        if v == self.traced and tok is None:
            return
        self.traced = v
        with open(TRACE, "a") as f:
            f.write("%d %d %s %s\n" % (self.slot, self.frame(), tok or "-", " ".join("%s=%s" % kv for kv in zip(PROBE, v))))

    def until(self, name, value, ms=UNTIL_MS, ne=False):
        """Wait until probe(name) == value (ne: != value), at most ms of game time (vsyncs, as wait)."""
        n = max(1, round(ms * FPS / 1000))
        start = last = self.frame()
        done, still = 0, time.time()
        while True:
            v = self.probe(name)
            if (v != value) if ne else (v == value):
                return self.frame() - start
            if done >= n:
                raise TimeoutError("@%s%s%d: still %s after %d vsyncs" % (name, "!=" if ne else "=", value, v, n))
            time.sleep(0.005)
            f = self.frame()
            if f > last:
                done += f - last
                still = time.time()
            elif time.time() - still > STALL_S:
                raise TimeoutError("frame count stuck at %d for %d s" % (f, STALL_S))
            last = f

    def press_until(self, key, name, value, every_ms=1000, hold_ms=120, port=0, ne=False):
        """Press key, again every every_ms of game time, until probe(name) == value (ne: != value; UNTIL_MS in all)."""
        for _ in range(max(1, UNTIL_MS // every_ms)):
            if (self.probe(name) != value) if ne else (self.probe(name) == value):
                return
            try:
                self.press(key, hold_ms, port)
            except RuntimeError:
                pass
            try:
                self.until(name, value, every_ms, ne)
                return
            except TimeoutError as e:
                if "stuck" in str(e):
                    raise
        raise TimeoutError("%s~%s%s%d: still %s after %d ms" % (key, name, "!=" if ne else "=", value, self.probe(name), UNTIL_MS))

    def fprobe(self, name):
        return struct.unpack("<f", struct.pack("<I", self.r32(FPROBE[name])))[0]

    def hold_until(self, key, name, op, value, ms=5000, port=0):
        """Hold key until FPROBE name >= value (op ">") or <= value (op "<"), at most ms of game time."""
        def done():
            v = self.fprobe(name)
            return v >= value if op == ">" else v <= value
        end = self.frame() + ms * FPS // 1000
        if done():
            return
        b = BIND[ALIAS.get(key, key)]
        self.pad(b, 255, port)
        try:
            while not done():
                if self.frame() >= end:
                    raise TimeoutError("%s^%s%s=%g: still %g after %d ms" % (key, name, op, value, self.fprobe(name), ms))
                time.sleep(0.002)
        finally:
            self.pad(b, 0, port)

    def press(self, name, hold_ms=120, port=0):
        bs = [BIND[ALIAS.get(n, n)] for n in name.split("+")]  # A+B = chord
        for b in bs:
            self.pad(b, 255, port)
        self.wait(hold_ms)
        for b in bs:
            self.pad(b, 0, port)

    def seq(self, text, hold_ms=120, gap_ms=350, port=0):
        """Comma list: Key, Key+Key (chord), Key*N, Key:holdMS, wMS (wait), fN (wait until frame count >= N).
        @name=V[/MS] (wait until game RAM probe name == V, see PROBE and until; V decimal or 0x).
        Key~name=V[/MS]: press Key, again every MS, until name == V. Key~name![/MS]: the same until name leaves
        the value it had before the first press. Key^name>=V[/MS], Key^name<=V[/MS]: hold Key until FPROBE name
        reaches V (default 5000 ms). Up|Down^name=V[/MS]: hold Up while name < V (until >= V), else Down
        (until <= V).
        ms = game time (vsyncs at 60/s, see wait)."""
        for tok in [t.strip() for t in text.split(",") if t.strip()]:
            self.trace(tok)
            if tok[0] == "@":
                name, _, rest = tok[1:].partition("=")
                v, _, ms = rest.partition("/")
                self.until(name, int(v, 0), int(ms or UNTIL_MS))
                continue
            if "^" in tok and "|" in tok:
                keys, _, cond = tok.partition("^")
                up, _, down = keys.partition("|")
                name, _, rest = cond.partition("=")
                v, _, ms = rest.partition("/")
                if self.fprobe(name) < float(v):
                    self.hold_until(up, name, ">", float(v), int(ms or 5000), port)
                else:
                    self.hold_until(down, name, "<", float(v), int(ms or 5000), port)
                continue
            if "^" in tok:
                key, _, cond = tok.partition("^")
                op = ">" if ">=" in cond else "<"
                name, _, rest = cond.partition(op + "=")
                v, _, ms = rest.partition("/")
                self.hold_until(key, name, op, float(v), int(ms or 5000), port)
                continue
            if "~" in tok and "!" in tok:
                key, _, cond = tok.partition("~")
                name, _, ms = cond.partition("!")
                self.press_until(key, name, self.probe(name), int(ms.lstrip("/") or 1000), hold_ms, port, ne=True)
                continue
            if "~" in tok:
                key, _, cond = tok.partition("~")
                name, _, rest = cond.partition("=")
                v, _, ms = rest.partition("/")
                self.press_until(key, name, int(v, 0), int(ms or 1000), hold_ms, port)
                continue
            if tok[0] == "w" and tok[1:].isdigit():
                self.wait(int(tok[1:]))
                continue
            if tok[0] == "f" and tok[1:].isdigit():
                while self.frame() < int(tok[1:]):
                    time.sleep(0.1)
                continue
            name, _, n = tok.partition("*")
            name, _, hold = name.partition(":")  # Key:ms = hold time (the map cursor moves while held)
            for _ in range(int(n or 1)):
                self.press(name, int(hold or hold_ms), port)
                self.wait(gap_ms)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slot", type=int, default=28011)
    ap.add_argument("--hold", type=int, default=120)
    ap.add_argument("--gap", type=int, default=350)
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    p = Pine(a.slot)
    c = a.cmd
    if c == "status":
        print(p.status(), p.title())
    elif c == "seq":
        p.seq(a.args[0], a.hold, a.gap)
    elif c in ("r8", "r16", "r32"):
        print(hex(getattr(p, c)(int(a.args[0], 0))))
    elif c in ("w8", "w32"):
        getattr(p, c)(int(a.args[0], 0), int(a.args[1], 0))
    elif c == "frame":
        print(p.frame())
    elif c == "watch":  # watch [secs]: frame + every PROBE value on each change (route mapping)
        t0, seen = time.time(), None
        while time.time() - t0 < (float(a.args[0]) if a.args else 60):
            v = tuple(p.probe(k) for k in PROBE)
            if v != seen:
                print(p.frame(), " ".join("%s=%s" % kv for kv in zip(PROBE, v)), flush=True)
                seen = v
            time.sleep(0.01)
    elif c == "fps":
        f0, t0 = p.frame(), time.time()
        time.sleep(float(a.args[0]) if a.args else 5)
        print("%.1f" % ((p.frame() - f0) / (time.time() - t0)))
    elif c == "snap":  # snap <png> [scale]
        path = p.snap(a.args[0])
        if len(a.args) > 1:
            shrink(path, float(a.args[1]))
        print(path)
    elif c in ("save", "load"):
        getattr(p, c)(int(a.args[0]))
    else:
        sys.exit("unknown cmd " + c)


if __name__ == "__main__":
    main()
