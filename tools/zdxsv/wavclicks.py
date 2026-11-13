"""Clicks in a ZDXSV_AUDIO_WAV capture, per stretch of output between two output resumes.

usage: wavclicks.py [--jump J] FILE.wav   (reads FILE.wav.gaps.txt next to it)
A click: a sample that differs from the previous one of its channel by more than J (default 0.5,
full scale 1.0), or a sample at or past full scale. Prints one line per stretch (start frame, frames,
peak, clicks, level of its first and last sample: a cut from or to silence), then the totals of the
stretches shorter than 5 s (output resumed for a frame step or a pause) and of the longer ones (play).
"""
import struct
import sys
from array import array


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV")
    fmt, channels, rate = struct.unpack_from("<HHI", data, 20)
    bits = struct.unpack_from("<H", data, 34)[0]
    if fmt != 3 or bits != 32 or data[36:40] != b"data":
        sys.exit(f"{path}: not the 32-bit float WAV ZDXSV_AUDIO_WAV writes")
    size = struct.unpack_from("<I", data, 40)[0]
    samples = array("f")
    samples.frombytes(data[44:44 + min(size, len(data) - 44) // (4 * channels) * 4 * channels])
    return samples, channels, rate


def main():
    args = sys.argv[1:]
    jump = 0.5
    if args[:1] == ["--jump"]:
        jump = float(args[1])
        args = args[2:]
    if len(args) != 1:
        sys.exit(__doc__)
    samples, ch, rate = read_wav(args[0])
    frames = len(samples) // ch
    starts = []
    with open(args[0] + ".gaps.txt") as f:
        for line in f:
            if line.strip():
                starts.append(int(line.split()[0]))
    starts = sorted(set(s for s in starts if s < frames) | {0}) + [frames]

    short = {"n": 0, "frames": 0, "clicks": 0, "peak": 0.0, "edge_max": 0.0, "edges_over": 0}
    long_ = dict(short)
    for i in range(len(starts) - 1):
        a, b = starts[i], starts[i + 1]
        peak = 0.0
        clicks = 0
        for c in range(ch):
            seg = samples[a * ch + c:b * ch:ch]
            prev = seg[0]
            for x in seg:
                ax = abs(x)
                if ax > peak:
                    peak = ax
                if ax >= 1.0 or abs(x - prev) > jump:
                    clicks += 1
                prev = x
        first = max(abs(samples[a * ch + c]) for c in range(ch))
        last = max(abs(samples[(b - 1) * ch + c]) for c in range(ch))
        print(f"{a:10d} {b - a:8d} peak {peak:.3f} clicks {clicks:5d} first {first:.3f} last {last:.3f}")
        t = short if (b - a) < rate * 5 else long_
        t["n"] += 1
        t["frames"] += b - a
        t["clicks"] += clicks
        t["peak"] = max(t["peak"], peak)
        t["edge_max"] = max(t["edge_max"], first, last)
        t["edges_over"] += (first > 0.1) + (last > 0.1)
    for name, t in (("short (<5 s)", short), ("long", long_)):
        print(f"{name}: {t['n']} stretches, {t['frames'] / rate:.2f} s, peak {t['peak']:.3f}, clicks {t['clicks']}, "
              f"first/last max {t['edge_max']:.3f}, first/last over 0.1: {t['edges_over']}")


if __name__ == "__main__":
    main()
