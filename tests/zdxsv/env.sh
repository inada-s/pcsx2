# Machine settings of the rig scripts (sourced by riglock.sh and the scripts that run alone).
# Each comes from the environment, else from local.env next to this file: KEY=value lines,
# not committed (.gitignore). The settings are listed in README.md.
_zenv=$(dirname "${BASH_SOURCE[0]}")
if [ -f "$_zenv/local.env" ]; then
  while IFS= read -r _l || [ -n "$_l" ]; do
    _l=${_l%$'\r'}
    case $_l in *=*) ;; *) continue;; esac
    _k=${_l%%=*}
    case $_k in ''|[0-9]*|*[!A-Za-z0-9_]*) continue;; esac
    [ -n "${!_k+x}" ] || export "$_k=${_l#*=}"  # the environment wins over local.env
  done < "$_zenv/local.env"
fi
# python with PIL (windowed snapshots); not $PY, which a CI runner may set to a python without it
PY=${ZDXSV_PY:-python}
GOEXE=${GOEXE:-go}
TOOLS=$(cd "$_zenv/../../tools/zdxsv" && pwd -W)  # pwcheck.py, udprelay.py
# zdxsv_need VAR...: FAIL (exit 2) unless each is set
zdxsv_need() {
  local v
  for v; do [ -n "${!v:-}" ] || { echo "FAIL $0: set $v (environment or tests/zdxsv/local.env, see README.md)"; exit 2; }; done
}
zdxsv_need RUN
