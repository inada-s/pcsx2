"""Check pcsx2 GGPO replays (ZDXSV_GGPO replay=, .zdxr; format: ReplayWrite in pcsx2/ZdxsvGgpo.cpp).

  python replay_check.py A.zdxr [B.zdxr ...] [--frames N]

Per file: header keys, state = a zip holding a pcsx2 save state, input bytes = frames * players * input_size.
Several files of one battle (one per peer): same players / frames (+-tail), and the inputs of every
common frame byte-identical (all peers log the same synced inputs). --frames N: frames must be N.
Exit 1 on any failure.
"""
import io
import sys
import zipfile


def load(path):
    data = open(path, "rb").read()
    end = data.index(b"\n\n")
    lines = data[:end].decode("utf-8").split("\n")
    if lines[0] != "ZDXSV-REPLAY 1":
        raise ValueError(f"bad magic {lines[0]!r}")
    h = dict(l.split("=", 1) for l in lines[1:])
    body = data[end + 2:]
    ss, isz = int(h["state_size"]), int(h["input_size"])
    players, frames = int(h["players"]), int(h["frames"])
    state, inputs = body[:ss], body[ss:]
    return h, state, inputs, players, frames, isz


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
        h, state, inputs, players, frames, isz = load(path)
        names = zipfile.ZipFile(io.BytesIO(state)).namelist()
        ok_zip = {"pcsx2 savestate version.id", "eememory.bin"} <= {n.lower() for n in names}
        distinct = len({inputs[i:i + isz] for i in range(0, len(inputs), isz)})
        print(f"{path}: position={h['position']} players={players} frames={frames} delay={h['delay']} close={h['close']} "
              f"state={len(state)} bytes ({len(names)} zip entries) inputs={len(inputs)} bytes, {distinct} distinct"
              f"{' battle_code=' + h['battle_code'] if 'battle_code' in h else ''}")
        if not ok_zip:
            fails.append(f"{path}: state zip has no save state entries: {names[:5]}")
        if len(inputs) != frames * players * isz:
            fails.append(f"{path}: input bytes {len(inputs)} != {frames}*{players}*{isz}")
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
            if h.get("battle_code") != h0.get("battle_code"):
                fails.append(f"{path}: battle_code {h.get('battle_code')} != {h0.get('battle_code')}")
    for f in fails:
        print("FAIL", f)
    print("PASS" if not fails else f"{len(fails)} FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
