param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GameArgs)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot

function Get-GameData {
    $location = Join-Path $repo 'install-location.txt'
    if (Test-Path -LiteralPath $location) {
        $saved = (Get-Content -LiteralPath $location -Raw -Encoding UTF8).Trim()
        if ($saved) { return $saved }
    }
    return Join-Path $repo 'runtime'
}

$data = Get-GameData
if (!(Test-Path -LiteralPath (Join-Path $data 'gt2game.exe'))) {
    # First run, or the data folder moved: import the disc, then start the game.
    $install = @('-Target', 'PC')
    if ([Console]::IsInputRedirected) { $install += @('-HdMedia', 'Original', '-NoBios') }
    & (Join-Path $PSScriptRoot 'install-player.ps1') @install
    $data = Get-GameData
}
$exe = Join-Path $data 'gt2game.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'GT2 was not installed. Run INSTALL-PC.bat and select your disc.' }
# A launcher update replaces the program files, not the data folder. Keep the
# game executable there current without asking for the disc again.
foreach ($name in @('gt2game.exe','gt2install.exe','openxr_loader.dll')) {
    $source = Join-Path $repo "tools/$name"
    $target = Join-Path $data $name
    if (!(Test-Path -LiteralPath $source)) { continue }
    if ((Test-Path -LiteralPath $target) -and
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) { continue }
    try { Copy-Item -LiteralPath $source -Destination $target -Force }
    catch { Write-Warning "Could not update $name in the game folder; it may be running." }
}
Push-Location $data
try { & $exe --player @GameArgs; exit $LASTEXITCODE }
finally { Pop-Location }
