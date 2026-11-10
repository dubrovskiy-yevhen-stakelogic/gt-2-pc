param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$location = Join-Path $repo 'install-location.txt'
$data = if (Test-Path -LiteralPath $location) { (Get-Content -LiteralPath $location -Raw -Encoding UTF8).Trim() } else { Join-Path $repo 'runtime' }
$exe = Join-Path $data 'gt2game.exe'
if (Test-Path -LiteralPath $exe) {
    Push-Location $data
    try { & $exe --player @GameArgs; exit $LASTEXITCODE }
    finally { Pop-Location }
}
& (Join-Path $repo 'INSTALL-PC.bat')
exit $LASTEXITCODE
