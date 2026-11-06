# Network pins for every rig script that starts PCSX2 (dot-source it before Start-Process), so a run
# never reaches the production server, also with a release exe (its built-in server host).
. "$PSScriptRoot\env.ps1"  # IP from local.env
# Fast login (on by default) posts the card's login key to the login API:
# off unless ZDXSV_FAST_LOGIN is set, and the API URL is local unless ZDXSV_LOGIN_API_URL is set.
if (-not $env:ZDXSV_FAST_LOGIN) { $env:ZDXSV_FAST_LOGIN = '0' }
if (-not $env:ZDXSV_LOGIN_API_URL) { $env:ZDXSV_LOGIN_API_URL = 'http://127.0.0.1:9881/lbs/login' }
# Sign-in (Settings, or fast login with no token at DNAS when ZDXSV_AUTH_AT_DNAS=1): local, URL logged only.
if (-not $env:ZDXSV_AUTH_URL) { $env:ZDXSV_AUTH_URL = 'http://127.0.0.1:9881/auth' }
$env:ZDXSV_AUTH_NO_BROWSER = '1'
if (-not $env:ZDXSV_AUTH_AT_DNAS) { $env:ZDXSV_AUTH_AT_DNAS = '0' }
# Online without DEV9 resolves every name to the server host: pinned to the box.
# DEV9=1 (or no IP): empty server host, the DEV9 path with the ini's hosts.
$env:ZDXSV_SERVER_HOST = if ($env:DEV9 -eq '1') { '' } elseif ($env:ZDXSV_SERVER_HOST) { $env:ZDXSV_SERVER_HOST } elseif ($env:IP) { $env:IP } else { '' }
