# Machine settings for the PowerShell rig scripts: env.sh for PowerShell (dot-source it).
# The environment wins over local.env next to this file.
$envFile = Join-Path $PSScriptRoot 'local.env'
if (Test-Path $envFile) {
  foreach ($l in Get-Content $envFile -Encoding UTF8) {
    if ($l -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$' -and -not (Test-Path "env:$($Matches[1])")) {
      Set-Item "env:$($Matches[1])" $Matches[2]
    }
  }
}
# exit here would only leave this file: each caller ends on $envFail (exit 2)
$envFail = if (-not $env:RUN) { "FAIL: set RUN (environment or tests/zdxsv/local.env, see README.md)" }
$py = if ($env:ZDXSV_PY) { $env:ZDXSV_PY } else { 'python' }
