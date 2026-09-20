param([ValidateSet('arcade','simulation')][string]$Mode = 'arcade', [Parameter(ValueFromRemainingArguments=$true)][string[]]$GameArgs)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$location = Join-Path $repo 'install-location.txt'
$runtime = if (Test-Path $location) { (Get-Content -LiteralPath $location -Raw).Trim() } else { Join-Path $repo 'runtime' }
$exe = Join-Path $runtime 'gt2game.exe'
$data = Join-Path $runtime $Mode
if (!(Test-Path $exe) -or !(Test-Path $data)) { throw 'Run INSTALL.bat and select your disc first.' }
New-Item -ItemType Directory -Force -Path (Join-Path $runtime "saves\$Mode") | Out-Null
Push-Location $runtime
try { & $exe $data --settings (Join-Path $runtime "saves\$Mode\settings.txt") --card (Join-Path $runtime "saves\$Mode\card1.mcd") @GameArgs; exit $LASTEXITCODE }
finally { Pop-Location }
