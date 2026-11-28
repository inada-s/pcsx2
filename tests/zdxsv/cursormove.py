#!/usr/bin/env python3
"""Hidden fullscreen cursor shows while the mouse moves ([UI] ZdxsvCursorShowOnMove).

usage: cursormove.py [--off|--relative]   (RUN, ISO, PCSX2_EXE as launch.py; Windows; no network)
Boots the game fullscreen with [UI] HideMouseCursor = true in instance p9, the mouse on the primary screen
centre, and reads the cursor pcsx2 set after 6 s still, then while and after the mouse moves.
exit 0: still -> hidden; moved -> shown within 0.5 s and through 4 s of moves; 4.5 s after the last move -> hidden.
Then a move, pause (TogglePause = Space): shown 4.5 s later; resume: hidden within 0.5 s.
--off: ZdxsvCursorShowOnMove = false (control); exit 0 when the cursor stays hidden except while paused.
--relative: pad 1 right stick bound to the mouse X axis (relative mouse mode); exit 0 as --off.
"""
import ctypes
import ctypes.wintypes as wt
import os
import shutil
import sys
import time

import launch
import zrig

N = 9
BOOT_S = 25


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("mi", MOUSEINPUT)]


def user32():
    u = ctypes.WinDLL("user32", use_last_error=True)
    u.LoadCursorW.restype = wt.HANDLE
    u.LoadCursorW.argtypes = [wt.HINSTANCE, ctypes.c_void_p]
    u.GetCursor.restype = wt.HANDLE
    u.WindowFromPoint.restype = wt.HWND
    u.WindowFromPoint.argtypes = [wt.POINT]
    return u


def shown(u, arrow, pid):
    """The cursor the window under the mouse set: the arrow (shown) or none (hidden). Read on that window's
    input queue, not GetCursorInfo: on a box without a mouse the system cursor is always hidden."""
    pt = wt.POINT()
    u.GetCursorPos(ctypes.byref(pt))
    hwnd = u.WindowFromPoint(pt)
    owner = wt.DWORD()
    tid = u.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if owner.value != pid:
        raise RuntimeError(f"window under the mouse is not pcsx2 (pid {owner.value})")
    me = ctypes.windll.kernel32.GetCurrentThreadId()
    if not u.AttachThreadInput(me, tid, True):
        raise OSError(ctypes.get_last_error(), "AttachThreadInput")
    try:
        h = u.GetCursor()
    finally:
        u.AttachThreadInput(me, tid, False)
    if os.environ.get("CURSORMOVE_TRACE"):
        print(f"cursor={h} arrow={arrow} pt=({pt.x},{pt.y})", flush=True)
    return h == arrow


def move(u, dx):
    inp = INPUT(type=0, mi=MOUSEINPUT(dx=dx, dy=0, dwFlags=0x0001))
    if u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT)) != 1:
        raise OSError(ctypes.get_last_error(), "SendInput")


def main():
    relative = "--relative" in sys.argv[1:]
    off = "--off" in sys.argv[1:]
    zrig.need("RUN", "ISO")
    if not zrig.WIN:
        print("SKIP: Windows only")
        return 0
    u = user32()
    arrow = u.LoadCursorW(None, ctypes.c_void_p(32512))
    root = os.path.abspath(os.environ["RUN"])
    pd = os.path.join(root, f"p{N}", "PCSX2")
    if not os.path.isdir(os.path.join(pd, "inis")):
        for sub in ("inis", "bios", "memcards"):
            shutil.copytree(os.path.join(root, "p1", "PCSX2", sub), os.path.join(pd, sub), dirs_exist_ok=True)
        tf = os.path.join(pd, "inis", "zdxsv_auth_token.txt")
        if os.path.exists(tf):
            os.remove(tf)
    ini = os.path.join(pd, "inis", "PCSX2.ini")
    zrig.ini_set(ini, "UI", "HideMouseCursor", "true")
    zrig.ini_set(ini, "UI", "ZdxsvCursorShowOnMove", "false" if off else "true")
    zrig.ini_set(ini, "Pad1", "RRight", "Pointer-0/X+" if relative else None)
    zrig.ini_set(ini, "Hotkeys", "TogglePause", "Keyboard/Space")
    u.SetCursorPos(u.GetSystemMetrics(0) // 2, u.GetSystemMetrics(1) // 2)
    pid = launch.launch(ns=(N,), extra_args=["-fullscreen"])[N]
    try:
        time.sleep(BOOT_S)
        if not zrig.alive(pid):
            print("FAIL: pcsx2 exited")
            return 1
        time.sleep(6)
        still = shown(u, arrow, pid)
        move(u, 40)
        time.sleep(0.5)
        after_move = shown(u, arrow, pid)
        # Past the hide delay counted from the first move: each move restarts it.
        kept = True
        for i in range(16):
            move(u, -10 if i % 2 else 10)
            time.sleep(0.25)
            kept = kept and shown(u, arrow, pid)
        time.sleep(4.5)
        idle = shown(u, arrow, pid)
        move(u, 40)
        time.sleep(0.5)
        zrig.ctl(pid, "Space")
        time.sleep(4.5)
        paused = shown(u, arrow, pid)
        zrig.ctl(pid, "Space")
        time.sleep(0.5)
        resumed = shown(u, arrow, pid)
        print(f"still={still} moved={after_move} moving={kept} idle={idle} paused={paused} resumed={resumed} off={off} relative={relative}")
        if off or relative:
            ok = not (still or after_move or kept or idle or resumed) and paused
        else:
            ok = not still and after_move and kept and not idle and paused and not resumed
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        zrig.kill(pid)


if __name__ == "__main__":
    sys.exit(main())
