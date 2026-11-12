param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot

. (Join-Path $PSScriptRoot 'existing-install.ps1')
$data = Find-Gt2ExistingData $repo
if (!(Test-Gt2ExistingData $data)) {
    # The folder picker may attach an existing installation without importing again.
    $install = @{ Target = 'PC'; ReuseExisting = $true }
    if ([Console]::IsInputRedirected) { $install.HdMedia = 'Original'; $install.NoBios = $true }
    & (Join-Path $PSScriptRoot 'install-player.ps1') @install
    $data = Find-Gt2ExistingData $repo
}
Update-Gt2ExistingData -PackageRoot $repo -Data $data
$exe = Join-Path $data 'gt2game.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'GT2 was not installed. Run INSTALL-PC.bat and select your disc.' }
Push-Location $data
try { & $exe --player @GameArgs; exit $LASTEXITCODE }
finally { Pop-Location }
