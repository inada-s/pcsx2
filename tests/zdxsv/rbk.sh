#!/bin/bash
# rbk test (flycast run.py rbk_test / rbk_test_random): N pcsx2 resumed at once from the post-entry
# states (rbkprep.sh), no lobby or battle server: pcsx2 ZDXSV_RBK=i/N answers the battle start and
# connect itself, menus run turbo, the battle runs over GGPO (net=1), each pcsx2 exits at
# its session end. Exit 0 = every client logged its session end and pwcheck found the peers in sync.
#   OUT=<dir> bash tests/zdxsv/rbk.sh [N=4] [seed]   (no seed: host pad = no input)
# Env: RBKSTATES (dir of rbk-p1..p4.p2s), TIME (rule 作戦時間 s, menu 90..270, 30 works too; default 90),
# COUNT (連続対戦数, default 1; 0 = rematch picked by input), GAUGE (戦力ゲージ, default 1: first shoot-down ends the battle, recorded 600), MAXS (default 900),
# SELECT (出撃準備 timer frames via ZDXSV_EE_CLAMP, default 300; SELECT= off: N=2 seed 1 5381 -> 7781 frames, 71 -> 99 s), BATTLE (battle timer frames, u16 0x838f66, default off = TIME; 600 frames saves 1200 frames), BRIEF (pre-play 600-frame countdown, u32 0x6f2b50 from select end, default 1: play start frame 1581 -> 1299, BRIEF= off), TAIL (frames run after the end msg, ZDXSV_NET_TAIL, default 60; pcsx2 300), PS (default 1: ZDXSV_ZDS_PS play-start barrier, needed with SELECT, s631; PS= off), TURBO (default 1: battle turbo too, frame-identical; TURBO= nominal), PWDUMP=1 (player-work dumps $OUT/pw-p*.bin for pwdiff.py), PCSX2_ENV (extra env for all), ENV0 (extra env for position 0 only: controls), DELAY (GGPO input delay, default 2 as flycast's local rbk test: 85 s vs 133 s at 0, rollback frames ~100 vs ~1300 per peer), GGPO (ZDXSV_GGPO value, default net=1,players=N,zd=1,zdp=1,zds=1,delay=DELAY).
# TSCALE (turbo cap, default 4; TSCALE= = pcsx2 default 2). N=4 plays p1..p4 (CPU-bound on a 4-core host: ~67 fps). N=2 plays p1 (position 0) + p3 (position 1).
# OWN (default 1): pwcheck --own also compares each machine's own player (pcsx2 7b1b1945 masks the own-only
# words; else N=2 sees each player on one machine only). OWN= = old check (own player left out).
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/riglock.sh"  # one rig at a time
zdxsv_need RBKSTATES
OUT=${OUT:?OUT=dir for logs}
N=${1:-4}
SEED=${2:-}
case $N in 2) P=(1 3);; 4) P=(1 2 3 4);; *) echo "N=2 or 4"; exit 2;; esac
GGPO=${GGPO:-net=1,players=$N,delay=${DELAY-2}}
mkdir -p "$OUT"
# LAT (one-way ms; set = a udprelay.py per peer pair, as m4relay.sh), JITTER (ms, 0), LOSS (0..1, 0)
rpids=
if [ -n "${LAT:-}" ]; then
  GGPO="$GGPO,relay=7200"
  # a relay left from the last run holds its port (bind 10048, the run never starts)
  powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name like 'python%'\" | ? { \$_.CommandLine -like '*udprelay.py*' } | % { Stop-Process -Id \$_.ProcessId -Force }"
  for i in $(seq 0 $((N - 2))); do
    for j in $(seq $((i + 1)) $((N - 1))); do
      $PY -u "$TOOLS/udprelay.py" --a $((7200 + 8 * i + j)) --b $((7200 + 8 * j + i)) --p1 $((7001 + i)) --p2 $((7001 + j)) \
        --delay "$LAT" --jitter "${JITTER:-0}" --loss "${LOSS:-0}" --seed $((10 * i + j + ${SEED:-0})) --seconds 1200 --idle 10 \
        > "$OUT/relay-$i$j.txt" 2>&1 &
      rpids="$rpids $!"
    done
  done
fi
cleanup() {
  [ -n "$rpids" ] && kill $rpids 2>/dev/null
  powershell -NoProfile -Command "Get-Process pcsx2* -EA 0 | Stop-Process -Force"
  for p in "${P[@]}"; do cp "$RUN/p$p/PCSX2/logs/emulog.txt" "$OUT/emulog-p$p.txt" 2>/dev/null; done
}
trap 'cleanup; rig_release' EXIT
t0=$SECONDS
turbo=${TURBO-1}
select=${SELECT-300}
ps=${PS-1}
battle=${BATTLE-}
brief=${BRIEF-1}
# TSCALE: turbo speed cap (ini [Framerate] TurboScalar; pcsx2 default 2.0, max 10). Seed 1: N=2 2x 54 s
# (battle 99 fps), 4x 43 s (120 fps), 10x 42 s; N=4 CPU-bound 67 fps: 76 -> 73 s (menus only).
tscale=${TSCALE-4}
for p in "${P[@]}"; do
  ini=$RUN/p$p/PCSX2/inis/PCSX2.ini
  sed -i '/^TurboScalar *=/d' "$ini"
  [ -n "$tscale" ] || continue
  grep -aq '^\[Framerate\]' "$ini" || printf '\r\n[Framerate]\r\n' >> "$ini"
  sed -i "s/^\[Framerate\]\r\?$/&\nTurboScalar = $tscale\r/" "$ini"
done
clamp="${select:+117f566,$select,2000,3600}${brief:+;6f2b50,$brief,$((brief + 1)),600}${battle:+;838f66,$battle,$((battle + 1)),$((60 * ${TIME:-90}))}"
for i in $(seq 0 $((N - 1))); do
  p=${P[$i]}
  rm -f "$RUN/p$p/PCSX2/logs/emulog.txt" "$OUT/trace-p$p.txt"
  env ZDXSV_GGPO="$GGPO" ZDXSV_RBK=$i/$N ZDXSV_RBK_TIME=${TIME:-90} ZDXSV_RBK_COUNT=${COUNT:-1} ZDXSV_RBK_GAUGE=${GAUGE:-1} ${turbo:+ZDXSV_RBK_TURBO=1} ${clamp:+ZDXSV_EE_CLAMP=$clamp} ${ps:+ZDXSV_ZDS_PS=1} ZDXSV_NET_TAIL=${TAIL:-60} ${SEED:+ZDXSV_RAND_INPUT=$((SEED + i))} \
    ZDXSV_PW_HASH=1 ZDXSV_NET_TRACE=$OUT/trace-p$p.txt ${PWDUMP:+ZDXSV_PW_DUMP=$OUT/pw-p$p.bin} $PCSX2_ENV $([ "$i" -eq 0 ] && echo "$ENV0") \
    powershell -NoProfile -Command "& '$here/launch.ps1' -N $p -Headless -LobbyState -StateFile $RBKSTATES/rbk-p$p.p2s" 2>&1 | tail -1 &
  lpids="$lpids $!"
done
# parallel launches: serial ones put p4 ~7.5 s behind p1. Only these: a bare wait also waits for
# the LAT relays (start check after the battle; a failed launch hung to the relay's 1200 s).
wait $lpids
# Start check: each pcsx2 logs `rbk env` (pcsx2 ZdxsvGgpo, 1st vsync) with the knobs it received; stop at
# once on a mismatch (glued env vars once ran a whole rig without input/turbo). ENV0/PCSX2_ENV may
# override knobs on purpose: those peers are only shown.
for i in $(seq 0 $((N - 1))); do
  p=${P[$i]}
  l=$RUN/p$p/PCSX2/logs/emulog.txt
  want="pos=$i/$N rand=${SEED:+$((SEED + i))} turbo=${turbo:+1} ps=${ps:+1} clamp=${clamp} ggpo=$GGPO"
  want=$(echo "$want" | sed 's/=\( \|$\)/=-\1/g')
  got=
  for t in $(seq 30); do
    got=$(grep -a -o "rbk env .*" "$l" 2>/dev/null | head -1 | tr -d '\r' | cut -c9-)
    [ -n "$got" ] && break
    sleep 1
  done
  if [ -n "$PCSX2_ENV" ] || { [ "$i" -eq 0 ] && [ -n "$ENV0" ]; }; then
    echo "p$p rbk env (not checked, ENV0/PCSX2_ENV): ${got:-none}"
  elif [ "$got" != "$want" ]; then
    printf 'FAIL p%s start check\n  want: %s\n  got:  %s\n' "$p" "$want" "${got:-no rbk env line in 30 s}"
    exit 1
  fi
done
echo "start check ok: $N peers got the intended knobs (seed ${SEED:-none})"
while [ $((SECONDS - t0)) -lt "${MAXS:-900}" ]; do
  sleep 1
  [ "$(tasklist | grep -ci pcsx2)" -eq 0 ] && break
done
echo "$((SECONDS - t0)) s: $(tasklist | grep -ci pcsx2) pcsx2 left"
ok=0
for p in "${P[@]}"; do
  l=$RUN/p$p/PCSX2/logs/emulog.txt
  grep -a "ZdxsvGgpo: \(rbk position\|net armed\|rbk exit\|net report\)" "$l" | cut -c1-160
  grep -a -q "rbk exit (net battle end)" "$l" || { echo "FAIL p$p: no session end"; ok=1; }
done
traces=(); for p in "${P[@]}"; do traces+=("$OUT/trace-p$p.txt"); done
$PY "$TOOLS/pwcheck.py" $([ "${OWN-1}" = 1 ] && echo --own) "${traces[@]}" > "$OUT/pwcheck.txt"
grep '^common\|^player' "$OUT/pwcheck.txt" | cut -c1-160
[ -n "$rpids" ] && { for t in $(seq 15); do kill -0 $rpids 2>/dev/null || break; sleep 1; done; grep -h 'relay end' "$OUT"/relay-*.txt | tr -d '\r'; }
# players >= N have no player work in this battle (their slots differ by start state)
awk -v n=$N '$1=="player" && $2+0 < n {s += $4} $1=="common" {c = $3} END {exit !(c > 0 && s == 0)}' "$OUT/pwcheck.txt" \
  || { echo "FAIL player work differs (players < $N) or no frames"; ok=1; }
echo "rbk N=$N seed=${SEED:-none}: $((SECONDS - t0)) s, $([ $ok = 0 ] && echo PASS || echo FAIL)"
exit $ok
