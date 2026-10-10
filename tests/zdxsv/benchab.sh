#!/bin/bash
# Speed A/B on one build: env knobs are the switch (each speed change keeps a control env, e.g.
# ZDXSV_RERUN_VU1=1). Runs rbk.sh N=2 seed 1 alternating A, B, A, B, .. (R runs per side) and prints,
# per part of the battle report, each side's mean and min-max over every client log (R x 2 samples);
# `differs` only when the two ranges do not overlap. Parts: rerun frame mean (wall), spu2 / vu1 ms per
# rerun frame, save / load ms mean, emulate mean.
#   OUT=<dir> bash tests/zdxsv/benchab.sh "<A env>" "<B env>" [R=2]   (e.g. "" "ZDXSV_RERUN_REVERB=1")
# Env: as rbk.sh; defaults TRACE=0 LAT=40 JITTER=8. PCSX2_ENV is kept, the side's env is appended.
# Logs: $OUT/a1, b1, a2, .. (rbk.sh OUT), summary $OUT/benchab.txt. A failed rbk.sh run stops it (exit 1).
OUT=${OUT:?OUT=dir for logs}
A=$1 B=$2 R=${3:-2}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
export TRACE=${TRACE-0} LAT=${LAT-40} JITTER=${JITTER-8}
base=${PCSX2_ENV:-}
for r in $(seq 1 "$R"); do
  for s in a b; do
    e=$A; [ $s = b ] && e=$B
    echo "== $s$r: ${e:-(no env)}"
    PCSX2_ENV="$base $e" OUT="$OUT/$s$r" bash "$here/rbk.sh" 2 1 > "$OUT/$s$r.txt" 2>&1
    rc=$?
    [ $rc = 0 ] || { tail -5 "$OUT/$s$r.txt"; echo "FAIL $s$r: rbk.sh rc $rc (log $OUT/$s$r.txt)"; exit 1; }
  done
done
# last battle report of each client log -> "side part value"
for f in "$OUT"/[ab][0-9]*/emulog-p*.txt; do
  s=$(basename "$(dirname "$f")"); s=${s:0:1}
  grep -a "ZdxsvGgpo: net battle end" "$f" | tr -d '\r' | awk -v s="$s" '
    { for (i = 1; i < NF; i++) {
        if ($i == "rerun" && $(i+1) == "frame" && $(i+2) == "mean") print s, "rerun_frame_mean", $(i+3)
        if ($i == "emulate" && $(i+1) == "mean") print s, "emulate_mean", $(i+2)
        if ($i == "save" && $(i+1) == "ms" && $(i+2) == "mean") print s, "save_ms_mean", $(i+3)
        if ($i == "load" && $(i+1) == "ms" && $(i+2) == "mean") print s, "load_ms_mean", $(i+3)
        if ($i == "spu2" && $(i+1) == "ms") { v = $(i+2); sub(/,$/, "", v); print s, "spu2_ms", v }
        if ($i == "vu1" && $(i+1) == "ms") { v = $(i+2); sub(/,$/, "", v); print s, "vu1_ms", v } } }'
done | awk -v A="${A:-(no env)}" -v B="${B:-(no env)}" '
  { k = $2; v = $3 + 0; n[$1, k]++; sum[$1, k] += v
    if (!(($1, k) in lo) || v < lo[$1, k]) lo[$1, k] = v
    if (!(($1, k) in hi) || v > hi[$1, k]) hi[$1, k] = v
    if (!(k in seen)) { seen[k] = 1; order[++m] = k } }
  END {
    printf "A: %s\nB: %s\n", A, B
    printf "%-17s %-26s %-26s %s\n", "part", "A mean [min-max] n", "B mean [min-max] n", "verdict"
    for (j = 1; j <= m; j++) { k = order[j]
      if (!n["a", k] || !n["b", k]) { printf "%-17s missing on a side\n", k; continue }
      d = (hi["a", k] < lo["b", k] || hi["b", k] < lo["a", k]) ? "differs" : "overlap"
      printf "%-17s %-26s %-26s %s\n", k,
        sprintf("%.3f [%.3f-%.3f] %d", sum["a", k] / n["a", k], lo["a", k], hi["a", k], n["a", k]),
        sprintf("%.3f [%.3f-%.3f] %d", sum["b", k] / n["b", k], lo["b", k], hi["b", k], n["b", k]), d } }' \
  | tee "$OUT/benchab.txt"
