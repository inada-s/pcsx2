# sourced by m4.sh, m4z.sh: the speed a client really runs at, measured (a client that runs faster than SPEED, e.g. from an instance ini's
# NominalScalar left from an earlier run, gives a result that is not for SPEED).
# speed_check <i>: pine.py fps over 3 s; nearer a higher speed than SPEED (fps > 60*(SPEED+0.5)) -> FAIL, rc 1.
# Slower is printed, not failed (N=4 can be CPU-bound at 4x).
speed_check() {
  local f
  f=$("$PY" "$here/pine.py" --slot $((28010 + $1)) fps 3 2>&1 | tail -1 | tr -d '\r')
  awk -v i="$1" -v f="$f" -v s="${SPEED:-1}" 'BEGIN {
    if (f !~ /^[0-9.]+$/) { print "p" i " fps ? (" f ")"; exit 0 }
    printf "p%s fps %s = %.1fx (SPEED %s)\n", i, f, f / 60, s
    if (f > 60 * (s + 0.5)) { printf "FAIL p%s runs at %.1fx, not SPEED=%s: no %sx result from this run\n", i, f / 60, s, s; exit 1 } }'
}
