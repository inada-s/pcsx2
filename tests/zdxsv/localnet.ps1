# Network pins for every rig script that starts PCSX2 (dot-source it before Start-Process), so a run
# never reaches the production server, also with a release exe (its built-in server host).
. "$PSScriptRoot\env.ps1"  # IP from local.env
# Fast login (on by default) posts the card's login key to the login API:
# off unless ZDXSV_FAST_LOGIN is set, and the API URL is local unless ZDXSV_LOGIN_API_URL is set.
if (-not $env:ZDXSV_FAST_LOGIN) { $env:ZDXSV_FAST_LOGIN = '0' }
if (-not $env:ZDXSV_LOGIN_API_URL) { $env:ZDXSV_LOGIN_API_URL = 'http://127.0.0.1:9881/lbs/login' }
# Online without DEV9 (on by default) resolves every name to the build's server host: pinned to the box.
# ZDXSV_NODEV9=0: the DEV9 path with the ini's hosts.
if (-not $env:ZDXSV_NODEV9) { $env:ZDXSV_NODEV9 = if ($env:IP) { $env:IP } else { '0' } }
