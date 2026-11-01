"""Boot-to-lobby time of one running instance (boottime.sh starts it).

Polls game RAM over PINE until the lobby's user_id selection screen (ID選択)
is up and prints the time from T0 (epoch seconds of the launch) to it.
  boottime.py --t0 <epoch> --out <dir> [--limit 5] [--maxs 200] [--route keys]
--route: drive.py keys sent from the start (route names expand, e.g. top,C).
Exit 0 = screen up within --limit s, 1 = later or never, 2 = wrong game.
"""
import argparse, os, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import Pine
from drive import ROUTES

SERIAL = "SLPS-25419"
# Lobby connection object pointer (a gp-relative global).
NET_OBJ_PTR = 0x6D7D54
# Byte in that object: network step, 4 from the user_id list (lobby 0x6131) on.
NET_STEP = 0xA1F
STEP_USER_ID_LIST = 4
FPS = 59.94


def connect(deadline):
    while True:
        try:
            return Pine(int(os.environ.get("PINE_SLOT", "28011")))
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--t0", type=float, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=float, default=5.0)
    ap.add_argument("--maxs", type=float, default=200.0)
    ap.add_argument("--route", default="")
    a = ap.parse_args()
    deadline = a.t0 + a.maxs
    p = connect(deadline)
    lock = threading.Lock()
    raw = p.call

    def call(op, args=b""):
        with lock:
            return raw(op, args)

    p.call = call
    log = open(os.path.join(a.out, "boottime.txt"), "w")
    log.write("launch %s.%03d\n" % (time.strftime("%H:%M:%S", time.localtime(a.t0)), a.t0 % 1 * 1000))

    def emit(text):
        line = "%8.3f s  frame %6d  %s" % (time.time() - a.t0, p.frame(), text)
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    serial = ""
    while time.time() < deadline and not serial.startswith("SLPS"):
        try:
            serial = p.call(0xC)[4:].split(b"\0")[0].decode("ascii", "replace")
        except (RuntimeError, OSError):
            pass
        time.sleep(0.05)
    emit("game %s" % serial)
    if serial != SERIAL:
        print("FAIL game %r is not %s" % (serial, SERIAL))
        return 2
    if a.route:
        keys = ",".join(ROUTES.get(k, k) for k in a.route.split(","))
        threading.Thread(target=p.seq, args=(keys,), daemon=True).start()
        emit("route " + a.route)
    last = None
    while time.time() < deadline:
        obj = p.r32(NET_OBJ_PTR)
        step = p.r8(obj + NET_STEP) if 0x100000 <= obj < 0x2000000 - NET_STEP else None
        if (obj != 0, step) != last:
            emit("net obj %08x step %s" % (obj, step))
            last = (obj != 0, step)
        if step == STEP_USER_ID_LIST:
            t = time.time() - a.t0
            f = p.frame()
            res = "PASS" if t <= a.limit else "FAIL"
            print("%s user_id screen at %.2f s (frame %d = %.2f s emulated), limit %.1f s" % (res, t, f, f / FPS, a.limit))
            return 0 if res == "PASS" else 1
        time.sleep(0.02)
    print("FAIL no user_id screen in %.0f s" % a.maxs)
    return 1


if __name__ == "__main__":
    sys.exit(main())
