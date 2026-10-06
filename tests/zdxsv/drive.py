"""Menu routes for Gundam vs. Z (PS2) over PINE, one instance per slot (28010+N).

  drive.py <route> <N> [<N> ...]   runs the route on each instance in parallel threads
  routes: top (attract/title -> server TOP page), fresh (blank card -> TOP), login (TOP -> 戦場選択 with ID 1),
          register (TOP -> 新規登録), newlogin, entry (戦場選択 -> side entry in Lobby 06),
          mash (in battle), after (result screen -> 作戦後部屋)
4-player battle: top 1 2 3 4; login 1 2 3 4; entry 1 2 3 4; mash 1 2 3 4 (x2); after 1 2 3 4.
Headless (launch.ps1 -Headless): same routes blind, mash x3, then after; result ~65 s later.
Each route ends with a snap to $RUN/pN/<route>.png (half size).
Timings measured with 4 instances at 60 fps on one 4-core host.
"""
import os
import sys
import threading
import time

from pine import Pine, shrink

RUN = os.environ.get("RUN", ".")  # rig data root (env.sh)

# map cursor to シアトル (Lobby 02) from anywhere on the map: clamp top-left, fixed steps (see entry2p)
WALK2P = ",".join(["Up:3000,Left:6000,w500"] + ["Right:350,w600"] * 15 + ["Down:400,w600"] + ["Left:350,w600"] * 5)

ROUTES = {
    # attract -> main menu -> 通信対戦 -> warning -> NET file はい / slot 1 / はい -> autosave note
    # -> connection menu (default 前回と同じ接続 = dial-up on these cards: Up*2 = ブロードバンド接続)
    # -> ネットワーク接続 -> terms (Up = 進む) -> netconf list (Down*2 = せってい1) -> connect -> TOP
    # Start from a fresh boot (f1500): the attract phase later on shifts the first Start press.
    # Steps verified one by one on p3/p4 (trace), the whole string not yet run in one go.
    "top": "f1500,Return,w3000,Down*7,C,w7000,C,w4000,C,w4000,C,w4000,C,w6000,C,w9000,Up*2,C,w6000,C,w9000,Up,C,w6000,Down*2,C,w30000",
    # Blank (0xFF) card -> TOP, replaces "top" for a new player: NET file not found -> 新規作成 はい
    # -> slot 1 -> format はい -> 2 notes (formatted, NET saved; the 2nd times out if left) -> connection menu (default ブロードバンド) -> ネットワーク接続 -> terms
    # -> empty netconf list -> ネットワーク設定の編集 はい (Sony netcnf GUI) -> 新規作成 -> slot 1 -> guide
    # (Right = next page): Ethernet, PPPoE 必要ではない, DHCP auto, DNS auto, name せってい1, save, OK,
    # test いいえ, OK, X -> 終了する -> game reboots (title/attract) -> "top" without Up*2.
    "fresh": "f1500,Return,w3000,Down*7,C,w7000,C,w5000,C,w4000,C,w4000,Up,C,w6000,C,w5000,C,w8000,C,w6000,C,w9000,"
             "Up,C,w6000,C,w5000,Up,C,w12000,C,w4000,C,w5000,Right,w3000,C,w3000,Right,w3000,C,w3000,"
             "Down,C,w2000,Right,w3000,C,w2000,Right,w3000,C,w2000,Right,w3000,Right,w3000,C,w5000,C,w4000,"
             "C,w4000,C,w3000,X,w4000,Left,C,w45000,"
             "Return,w3000,Down*7,C,w7000,C,w4000,C,w5000,C,w5000,C,w5000,C,w6000,C,w6000,C,w9000,Up,C,w6000,Down*2,C,w30000",
    # TOP (cursor on ログイン) -> ID select -> ID 1 -> 戦場選択
    "login": "C,w12000,C,w8000,C,w6000",
    # TOP -> 新規登録 -> save ID+password to NET file -> ユーザ作成完了 -> 戻る -> TOP (new key on the card)
    # A C lost under load (3 logins at once) left the key unsaved: check the shot, run it alone.
    "register": "Right,w800,C,w10000,C,w6000,C,w4000,C,w8000",
    # TOP with a fresh key -> skip Capcom reg -> HN: kana column {h} x3 on the soft keyboard
    # -> Start x2 (1st confirms the kana, 2nd = 決定, cursor on 登録) -> 登録 -> はい -> 戦場選択
    "newlogin": "C,w14000,Right,C,w10000,C,w4000,Right*{h},C,C,C,Return,w1500,Return,w3000,C,w4000,Up,C,w9000",
    # 戦場選択 -> map cursor to ジャブロー2 (Lobby 06: starts at 2 AEUG + 2 Titans entries, zdxsv
    # model/lobby.go CanBattleStart) -> lobby -> パートナー自動選抜 -> side {s}: p1/p2 連邦・エゥーゴ, p3/p4 ジオン・ティターンズ
    # p4's Down was dropped once (landed in オペレーションリスト; fix X,w3000,Down,C,...): check shots.
    "entry": "C,w5000,Up:400,w800,Down:900,Right:900,w500,C,w8000,Down,w500,C,w4000,{s}C,w4000",
    # 1v1: map cursor to シアトル (Lobby 02: starts at 1 AEUG + 1 Titans). The cursor keeps its spot between
    # visits and hit areas sit off the drawn labels: clamp top-left, then fixed steps (grid scan: Lobby 02
    # at row 1 cols 5-6; route shot-checked, "Lobby 02" shown). Lobby menu + side dialog as in Lobby 06.
    "entry2p": "C,w5000," + WALK2P + ",w500,C,w8000,Down,w500,C,w4000,{s}C,w4000",
    # entry2p in halves for entry2p.sh (m4z.sh): map2p ends with the Lobby 02 press (0x6305 00 02); the
    # press missed in 3 of 15 entries (no 0x6305, client left on the map) -> remap2p = walk
    # again from the clamp, no leading C; lobby2p = the lobby menu + side dialog
    "map2p": "C,w5000," + WALK2P + ",w500,C",
    "remap2p": WALK2P + ",w500,C",
    "lobby2p": "w8000,Down,w500,C,w4000,{s}C,w4000",
    # entry in two halves around the lobby answer (m4.sh: after a state load the PS2's first lobby
    # send can lag ~1 min; a timed rest then lands on another menu, e.g. 0x6401)
    "entry1": "C,w5000,Up:400,w800,Down:900,Right:900,w500,C,w2000",
    "entry2": "w4000,Down,w500,C,w4000,{s}C,w4000",
    # in battle: move + all face buttons, ~3 min (a round is 150 s)
    "mash": ",".join(["Up:400,Z,C,Left:300,X,T,Z,Right:300,C,Z"] * 40),
    # whole round (~6 min): mash ends ~70 s into the fight (after the round restart, K3 #2)
    "mashlong": ",".join(["Up:400,Z,C,Left:300,X,T,Z,Right:300,C,Z"] * 70),
    # every pad bind + chords (record A/B = f(pad) table): ~3 min
    "mashall": ",".join(["Down:300,L1,R1,L2,R2,Select,L3,R3,Up+Z:300,Left+C,X+T,Right+L1:300,Z+C,"
                         "Down+R1,L2+R2,Up+Left:300"] * 20),
    # result screen (再戦 / 対戦終了) -> 対戦終了 -> セーブしないで終了? いいえ (= save) -> record opponents' IDs?
    # いいえ -> 作戦後部屋; lobby logs BattleResult per player. Each step shot-checked, not run in one go.
    # Dialogs time out (~30 s) to their default (いいえ / はい), so late presses still end in the room.
    "after": "Right,w300,C,w40000,C,w60000,Down,w300,C,w40000",
}


def route_text(route, n):
    # HN kana: column n-1 of row 1 (p2 い, p3 う, p4 え, p5 お, p6 た)
    s = "Right,w500," if n > 2 else ""
    return ROUTES[route].format(h=n - 1, s=s)


def run(route, n, log):
    p = Pine(28010 + n)
    p.seq(route_text(route, n))
    path = os.path.join(RUN, "p%d" % n, "%s.png" % route)  # GS snapshot ignores forward-slash paths
    try:
        shrink(p.snap(path, 3), 0.5)
    except TimeoutError:  # Null renderer (launch.ps1 -Headless): no GS snapshot
        path = "no snap"
    log.append("p%d %s frame %d -> %s" % (n, route, p.frame(), path))


def trace(route, n, start=0, end=9999):
    """Run the route from token `start` one token at a time, small snap after each wait: pN/trace_XX.png."""
    p = Pine(28010 + n)
    for i, tok in enumerate(route_text(route, n).split(",")):
        if i < start or i >= end:
            continue
        p.seq(tok)
        if tok[0] in "wf":
            path = os.path.join(RUN, "p%d" % n, "trace_%02d.png" % i)
            shrink(p.snap(path), 0.25)
            print(i, tok, p.frame(), path, flush=True)


def main():
    if sys.argv[1] == "trace":  # trace <route> <N> [start token] [end token]
        return trace(sys.argv[2], int(sys.argv[3]), *[int(x) for x in sys.argv[4:6]])
    if sys.argv[1] == "seq":  # seq <N> <tokens> <name>: ad-hoc keys (route search), snap pN/<name>.png
        ROUTES[sys.argv[4]] = sys.argv[3]
        sys.argv[1:] = [sys.argv[4], sys.argv[2]]
    route, ns = sys.argv[1], [int(x) for x in sys.argv[2:]]
    log = []
    ts = [threading.Thread(target=run, args=(route, n, log)) for n in ns]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    print("\n".join(log))


if __name__ == "__main__":
    main()
