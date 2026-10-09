"""Check pcsx2 GGPO replays (ZDXSV_GGPO replay=, <battle_code>.pb; schema: pcsx2/Zdxsv/replay.proto).

  python replay_check.py A.pb [B.pb ...] [--frames N]

Per file: the fields, input bytes = frames * players * input_size.
Several files of one battle (one per peer): same players / frames (+-tail), and the inputs of every
common frame byte-identical (all peers log the same synced inputs); state_hashes, load_frames and RNG B
(load_rngs & 0xffff) equal where both files have them, except from a game end (the earliest file's) to the next
play start, and from a round end to the tick's next 6: peers enter the game end and the round load on different
frames (play_start_frames, game_end_frames, round_end_frames; not a desync).
State hashes are judged from the play start, as pwcheck.py (earlier differing frames are printed).
--frames N: frames must be N.
Exit 1 on any failure.
"""
import sys

# replay.proto BattleLogFile: field number -> key
FIELDS = {3: "battle_code", 4: "version", 5: "game_disk", 11: "users", 18: "round_data", 20: "start_at", 21: "end_at", 24: "close",
          40: "players", 41: "position", 42: "delay", 43: "battle_info", 45: "rx0", 46: "hle0",
          47: "input_size", 48: "frames", 49: "inputs", 51: "lobby_answers", 52: "hashes", 53: "start_rng", 54: "load_frames",
          55: "load_rngs", 57: "play_start_frames", 58: "game_end_frames", 59: "round_end_frames"}
PACKED = ("hle0", "load_frames", "load_rngs", "play_start_frames", "game_end_frames", "round_end_frames")


def varint(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return v, i


def fields(b):
    """(field, wiretype, value) of a protobuf message; value = int or bytes."""
    i = 0
    while i < len(b):
        tag, i = varint(b, i)
        f, wt = tag >> 3, tag & 7
        if wt == 0:
            v, i = varint(b, i)
        elif wt == 2:
            n, i = varint(b, i)
            v, i = b[i:i + n], i + n
            if i > len(b):
                raise ValueError(f"field {f} past the end")
        elif wt in (1, 5):
            n = 8 if wt == 1 else 4
            v, i = int.from_bytes(b[i:i + n], "little"), i + n
        else:
            raise ValueError(f"wire type {wt} of field {f}")
        yield f, wt, v


def load(path):
    h = {"users": [], "lobby_answers": [], "round_data": [], **{k: [] for k in PACKED}}
    for f, wt, v in fields(open(path, "rb").read()):
        k = FIELDS.get(f)
        if k == "round_data":
            # BattleLogRound.win_team: int32, -1 = a 10-byte varint
            w = next((x for rf, _, x in fields(v) if rf == 1), 0)
            h[k].append(w - (1 << 64) if w >= 1 << 63 else w)
        elif k == "lobby_answers":
            h[k].append(v)
        elif k == "users":
            u = {FIELDS_USER.get(uf, uf): uv for uf, _, uv in fields(v)}
            h["users"].append({k2: v2.decode("utf-8") if isinstance(v2, bytes) else v2 for k2, v2 in u.items()})
        elif k in PACKED:
            if wt == 2:
                j = 0
                while j < len(v):
                    x, j = varint(v, j)
                    h[k].append(x)
            else:
                h[k].append(v)
        elif k:
            h[k] = v.decode("utf-8") if isinstance(v, bytes) and k in ("battle_code", "game_disk", "close", "battle_info") else v
    if not h.get("version"):
        raise ValueError("no log_file_version: not a replay file")
    inputs = h.pop("inputs", b"")
    return h, inputs, h.get("players", 0), h.get("frames", 0), h.get("input_size", 0)


FIELDS_USER = {1: "user_id", 2: "user_name", 3: "pilot_name", 12: "pos"}


def cuts(*hs):
    """[game end, next play start) over the files: from the earliest file's end (fields 57, 58); and
    [round end, tick 6) (field 59 pairs), to the latest file's tick 6."""
    starts = sorted({s for h in hs for s in h["play_start_frames"]})
    last = max(h.get("frames", 0) for h in hs)
    spans = [[e, next((s for s in starts if s > e), last)] for h in hs for e in h["game_end_frames"]]
    spans += [list(h["round_end_frames"][i:i + 2]) for h in hs for i in range(0, len(h["round_end_frames"]) - 1, 2)]
    out = []
    for a, b in sorted(spans):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def cut_at(cut, f):
    return any(a <= f < b for a, b in cut)


def main():
    args = sys.argv[1:]
    want = None
    if "--frames" in args:
        i = args.index("--frames")
        want = int(args[i + 1])
        del args[i:i + 2]
    fails = []
    reps = []
    for path in args:
        h, inputs, players, frames, isz = load(path)
        distinct = len({inputs[i:i + isz] for i in range(0, len(inputs), isz)})
        print(f"{path}: position={h.get('position', 0)} players={players} frames={frames} delay={h.get('delay', 0)} close={h.get('close', '')} "
              f"lobby_answers={len(h['lobby_answers'])} inputs={len(inputs)} bytes, {distinct} distinct"
              f"{' battle_code=' + h['battle_code'] if 'battle_code' in h else ''}")
        if len(inputs) != frames * players * isz:
            fails.append(f"{path}: input bytes {len(inputs)} != {frames}*{players}*{isz}")
        hashes = h.get("hashes", b"")
        if hashes or "start_rng" in h:
            loads = " ".join(f"{f}:{r:08x}" for f, r in zip(h["load_frames"], h["load_rngs"]))
            print(f"  state_hashes={len(hashes) // 4} start_rng={h.get('start_rng', 0):08x} loads={loads} "
                  f"play_starts={h['play_start_frames']} game_ends={h['game_end_frames']} round_ends={h['round_end_frames']}"
                  f" round_data={h['round_data']}")
        if hashes and len(hashes) != frames * 4:
            fails.append(f"{path}: state_hashes {len(hashes)} bytes != {frames}*4")
        if want is not None and frames != want:
            fails.append(f"{path}: frames {frames} != {want}")
        reps.append((path, h, inputs, players, frames, isz))
    if len(reps) > 1:
        p0, h0, in0, pl0, fr0, isz = reps[0]
        for path, h, inp, pl, fr, _ in reps[1:]:
            if pl != pl0:
                fails.append(f"{path}: players {pl} != {pl0}")
                continue
            n = min(fr, fr0)
            row = pl * isz
            diff = [f for f in range(n) if inp[f * row:(f + 1) * row] != in0[f * row:(f + 1) * row]]
            print(f"{path} vs {p0}: {n} common frames, {len(diff)} differ{' first ' + str(diff[0]) if diff else ''}")
            if diff:
                fails.append(f"{path} vs {p0}: {len(diff)} of {n} frames differ, first {diff[0]}")
            # state hashes and load frames + RNG B are the same on every machine (RNG A is not), outside the cut
            cut = cuts(h, h0)
            if cut:
                print(f"{path} vs {p0}: not judged (round end -> tick 6, game end -> next play start): {cut}")
            hs, hs0 = h.get("hashes", b""), h0.get("hashes", b"")
            if hs and hs0:
                # as pwcheck: from the play start (the scene steps before it follow local load timing)
                start = max(x["play_start_frames"][0] for x in (h, h0)) if h["play_start_frames"] and h0["play_start_frames"] else 0
                hd = [f for f in range(min(len(hs), len(hs0)) // 4) if hs[f * 4:f * 4 + 4] != hs0[f * 4:f * 4 + 4] and not cut_at(cut, f)]
                pre = [f for f in hd if f < start]
                hd = [f for f in hd if f >= start]
                print(f"{path} vs {p0}: state hashes {len(hd)} differ{' first ' + str(hd[0]) if hd else ''} from play start {start}"
                      f"; before it {len(pre)} differ (not judged){' first ' + str(pre[0]) if pre else ''}")
                if hd:
                    fails.append(f"{path} vs {p0}: state hashes differ from frame {hd[0]} ({len(hd)} frames)")
            ld = [(f, r & 0xffff) for f, r in zip(h["load_frames"], h["load_rngs"]) if not cut_at(cut, f)]
            ld0 = [(f, r & 0xffff) for f, r in zip(h0["load_frames"], h0["load_rngs"]) if not cut_at(cut, f)]
            lf, lf0 = h["load_frames"], h0["load_frames"]
            k = min(len(ld), len(ld0))
            if ld[:k] != ld0[:k]:
                fails.append(f"{path} vs {p0}: load frames / RNG B differ: {lf} {lf0}")
            if h["round_data"] != h0["round_data"]:
                fails.append(f"{path} vs {p0}: round_data (win_team per round) differ: {h['round_data']} {h0['round_data']}")
            if h.get("battle_code") != h0.get("battle_code"):
                fails.append(f"{path}: battle_code {h.get('battle_code')} != {h0.get('battle_code')}")
    for f in fails:
        print("FAIL", f)
    print("PASS" if not fails else f"{len(fails)} FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
