#!/bin/bash
# ENTRY1 stand-in for entry1.sh controls: client $SKIP's (default 2) first entry1 sends
# no input (= the entry flake: no lobby send), later ones run drive.py entry1.
#   ENTRY1="bash tests/zdxsv/entryfault.sh" SKIP=2 OUT=.. bash tests/zdxsv/rbkprep.sh   -> relaunch, PASS
#   + ENTRY_TRIES=1                                                         -> FAIL client 2 entry
here=$(cd "$(dirname "$0")" && pwd -W)
. "$here/env.sh"
i=$1
if [ "$i" = "${SKIP:-2}" ] && [ ! -f "$OUT/skipped" ]; then touch "$OUT/skipped"; echo "p$i entry1 skipped (fault)"; exit 0; fi
exec env MEMGATE_RESERVE_MB=150 "$PY" "$here/drive.py" entry1 "$i"
