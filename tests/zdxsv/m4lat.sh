#!/bin/bash
# Pad input latency + frame pacing in a 4-client battle: m4.sh with p1 windowed and
# measured (ZDXSV_INPUT_LATENCY on p1 only, presses start GODELAY s after mash begins),
# p2-p4 headless and mashing. Stops everything once p1 reported.
#   OUT=dir LOWLAT=0|1 SPEC='held=1,count=12' bash tests/zdxsv/m4lat.sh   -> search (pad buffer byte)
#   OUT=dir LOWLAT=0|1 SPEC='addr=0x...,count=40' bash tests/zdxsv/m4lat.sh
# Needs a pcsx2 build with ZdxsvInputLatency go=/held=.
here=$(cd "$(dirname "$0")" && pwd)
. "$here/riglock.sh"  # one rig at a time
OUT=${OUT:?OUT=dir}
mkdir -p "$OUT"
ini=$RUN/p1/PCSX2/inis/PCSX2.ini
v=true; [ "${LOWLAT:-1}" = 0 ] && v=false
sed -i "/^ZdxsvLowLatencyVsync =/d; s/^\[EmuCore\/GS\]\r\?$/&\nZdxsvLowLatencyVsync = $v/" "$ini"
grep -a "ZdxsvLowLatencyVsync" "$ini"
GO=$OUT/go; rm -f "$GO"
SHOW=1 MASHP="2 3 4" GO=$GO GODELAY=${GODELAY:-30} \
  PCSX2_ENV_P1="ZDXSV_INPUT_LATENCY=btn=${BTN:-down},back=${BACK:-up},go=$GO,seed=${SEED:-1},out=$OUT/lat.csv,${SPEC:?SPEC}" \
  OUT=$OUT bash "$here/m4.sh" > "$OUT/m4.out" 2>&1 &
m4=$!
log=$RUN/p1/PCSX2/logs/emulog.txt
t0=$SECONDS
while [ $((SECONDS - t0)) -lt "${MAXS:-540}" ]; do
  sleep 10
  grep -a -q "ZdxsvLatency: \(search done\|present->present\)" "$log" 2>/dev/null && break
  grep -q "^FAIL" "$OUT/m4.out" && break
done
echo "$((SECONDS - t0)) s"; grep "^FAIL\|^PASS\|client" "$OUT/m4.out" | head -12
grep -a "ZdxsvLatency" "$log" | cut -c1-200 | tail -${TAILN:-40}
kill $m4 2>/dev/null
taskkill //F //IM pcsx2-qtx64.exe > /dev/null 2>&1
taskkill //F //IM zdxsv.exe > /dev/null 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_Process | Where-Object { \$_.CommandLine -match 'fake_lobby|drive\.py|m4\.sh' -and \$_.CommandLine -notmatch 'powershell|m4lat' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }"
cp "$log" "$OUT/p1-emulog.txt"
