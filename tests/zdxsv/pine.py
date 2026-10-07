"""PINE client for this pcsx2 (TCP 127.0.0.1:<PINESlot> on Windows).

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
STALL_S = 120  # wait(): frame count not moving this long = emulator hung or paused


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
    def __init__(self, slot=28011, host="127.0.0.1", timeout=5.0):
        self.sock = socket.create_connection((host, slot), timeout=timeout)

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
        for _ in range(int(wait_s * 5)):
            time.sleep(0.2)
            if os.path.exists(src) and os.path.getsize(src) > 0:
                break
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
        emulation speed (launch.ps1 -Speed). PINE_WALL=1: wall-clock sleep (old)."""
        if WALL:
            time.sleep(ms / 1000)
            return
        n = max(1, round(ms * FPS / 1000))
        last = self.frame()
        done, still = 0, time.time()
        while done < n:
            # <= 10x speed (600 vsync/s): sleep about half the time the rest takes, 2 ms at the end
            time.sleep(min(0.1, max(0.002, (n - done) / 1200)))
            f = self.frame()
            if f > last:  # a state load / rollback that moves the counter back adds nothing
                done += min(f - last, n - done)
                still = time.time()
            elif time.time() - still > STALL_S:
                raise TimeoutError("frame count stuck at %d for %d s" % (f, STALL_S))
            last = f

    def press(self, name, hold_ms=120, port=0):
        bs = [BIND[ALIAS.get(n, n)] for n in name.split("+")]  # A+B = chord
        for b in bs:
            self.pad(b, 255, port)
        self.wait(hold_ms)
        for b in bs:
            self.pad(b, 0, port)

    def seq(self, text, hold_ms=120, gap_ms=350, port=0):
        """Comma list: Key, Key+Key (chord), Key*N, Key:holdMS, wMS (wait), fN (wait until frame count >= N).
        ms = game time (vsyncs at 60/s, see wait)."""
        for tok in [t.strip() for t in text.split(",") if t.strip()]:
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
