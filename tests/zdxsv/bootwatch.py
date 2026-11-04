"""Boot-to-lobby time of one running instance (boottime.py starts it).

Polls game RAM over PINE until the lobby's user_id selection screen (ID選択)
is up and prints the time from T0 (epoch seconds of the launch) to it.
  bootwatch.py --t0 <epoch> --out <dir> [--pid N --emulog path] [--limit 5] [--maxs 200] [--route keys]
--pid + --emulog: also the time from the VM start (emulog "Opening CDVD"), and
--limit applies to it; the launch script's own start-up is not counted.
--route: drive.py keys sent from the start (route names expand, e.g. top,C).
--route-end: after the user_id screen, keep watching until the route's keys are all sent (FAIL if not by --maxs).
Exit 0 = screen up within --limit s, 1 = later or never, 2 = wrong game.
"""
import argparse, os, re, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pine import Pine
from drive import ROUTES, route_text
from zrig import process_start

SERIAL = "SLPS-25419"
# Lobby connection object pointer (a gp-relative global).
NET_OBJ_PTR = 0x6D7D54
# Byte in that object: network step, 4 from the user_id list (lobby 0x6131) on.
NET_STEP = 0xA1F
STEP_USER_ID_LIST = 4
FPS = 59.94


def vm_start(created, emulog):
    """Epoch seconds of the VM start: emulog times count from the process start."""
    try:
        with open(emulog, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"\[\s*([0-9.]+)\] Opening CDVD", line)
                if m:
                    return created + float(m.group(1))
    except OSError:
        pass
    return None


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
    ap.add_argument("--route-end", action="store_true")
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--emulog", default="")
    a = ap.parse_args()
    deadline = a.t0 + a.maxs
    created = process_start(a.pid) if a.pid else None
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
        keys = ",".join(route_text(k, 1) if k in ROUTES else k for k in a.route.split(","))
        route = threading.Thread(target=p.seq, args=(keys,), daemon=True)
        route.start()
        emit("route " + a.route)
    last = None
    while time.time() < deadline:
        obj = p.r32(NET_OBJ_PTR)
        step = p.r8(obj + NET_STEP) if 0x100000 <= obj < 0x2000000 - NET_STEP else None
        if (obj != 0, step) != last:
            emit("net obj %08x step %s" % (obj, step))
            last = (obj != 0, step)
        if step == STEP_USER_ID_LIST:
            now = time.time()
            t = now - a.t0
            f = p.frame()
            vm0 = vm_start(created, a.emulog) if created and a.emulog else None
            if a.pid and vm0 is None:
                print("FAIL no VM start (pid %d, emulog %s)" % (a.pid, a.emulog))
                return 1
            tv = now - vm0 if vm0 else t
            res = "PASS" if tv <= a.limit else "FAIL"
            vm = " = %.2f s from the VM start (process start +%.2f s)" % (tv, vm0 - created) if vm0 else ""
            print("%s user_id screen at %.2f s from the launch%s (frame %d = %.2f s emulated), limit %.1f s"
                  % (res, t, vm, f, f / FPS, a.limit))
            if a.route and a.route_end:
                while route.is_alive() and time.time() < deadline:
                    s = p.r8(obj + NET_STEP)
                    if s != step:
                        emit("net obj %08x step %s" % (obj, s))
                        step = s
                    time.sleep(0.02)
                if route.is_alive():
                    print("FAIL route not done in %.0f s" % a.maxs)
                    return 1
                emit("route done")
            return 0 if res == "PASS" else 1
        time.sleep(0.02)
    print("FAIL no user_id screen in %.0f s" % a.maxs)
    return 1


if __name__ == "__main__":
    sys.exit(main())
