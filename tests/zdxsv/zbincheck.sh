#!/bin/bash
# zbincheck.sh <exe>: FAIL (exit 1) unless the go binary was built from the zdxsv checkout as it is now
# (vcs.revision == HEAD, not vcs.modified, clean src/). Called by stack.sh and m4z.sh before any client starts.
# ZREV=<rev prefix>: expect that build instead (a control on an old build); ZREV=any: no check.
# Env (env.sh): ZDXSV (zdxsv checkout), GOEXE (go binary, default go).
exe=$1
. "$(dirname "$0")/env.sh"
zdxsv_need ZDXSV
[ "${ZREV:-}" = any ] && exit 0
[ -f "$exe" ] || { echo "FAIL zbincheck: no $exe"; exit 1; }
info=$("$GOEXE" version -m "$exe" 2>&1 | tr -d '\r')
rev=$(echo "$info" | sed -n 's/.*vcs\.revision=//p')
mod=$(echo "$info" | sed -n 's/.*vcs\.modified=//p')
[ -n "$rev" ] || { echo "FAIL zbincheck: $exe has no vcs.revision (built without git info?)"; exit 1; }
if [ -n "${ZREV:-}" ]; then
  case "$rev" in "$ZREV"*) echo "zbincheck: $exe = $rev (ZREV)"; exit 0;; esac
  echo "FAIL zbincheck: $exe is $rev, ZREV=$ZREV"; exit 1
fi
head=$(git -C "$ZDXSV" rev-parse HEAD)
dirty=$(git -C "$ZDXSV" status --porcelain -- src go.mod go.sum | head -3)
if [ "$rev" != "$head" ] || [ "$mod" = true ] || [ -n "$dirty" ]; then
  echo "FAIL zbincheck: $exe built from ${rev:0:9}$([ "$mod" = true ] && echo +modified), checkout $ZDXSV is ${head:0:9}$([ -n "$dirty" ] && echo +modified) ($(git -C "$ZDXSV" branch --show-current))"
  echo "  rebuild: (cd $ZDXSV && $GOEXE build -o $exe ./src/zdxsv)  [commit edits first], or ZREV=${rev:0:9} for this old build"
  exit 1
fi
echo "zbincheck: $exe = ${head:0:9} (checkout HEAD)"
