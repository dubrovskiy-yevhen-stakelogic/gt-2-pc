param([string]$Runtime,[string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (!$Runtime) {
    $location = Join-Path $root 'install-location.txt'
    if (Test-Path -LiteralPath $location) { $Runtime = (Get-Content -LiteralPath $location -Raw).Trim() }
    else { $Runtime = Join-Path $root 'runtime' }
}
if (!(Test-Path -LiteralPath $Runtime)) { throw 'Install the original discs first with INSTALL.bat.' }
if (!$BuildDir) {
    foreach ($folder in @('tools','build_install','build_update')) {
        $candidate = Join-Path $root $folder
        if (Test-Path -LiteralPath (Join-Path $candidate 'gt2media.exe')) { $BuildDir = $candidate; break }
    }
}
if (!$BuildDir) { throw 'Run INSTALL.bat from the updated source or player package first.' }
Write-Host '1. HD pictures, menu text and HUD'
Write-Host '2. HD pictures, text/HUD and full-screen movies (slow; large temporary files)'
Write-Host '3. Original media; prepare only the PlayStation intro'
$choice = Read-Host 'Choose 1, 2 or 3 (Enter = 1)'
$mode = switch ($choice) { '' { 'Menus' }; '1' { 'Menus' }; '2' { 'MenusAndMovies' }; '3' { 'Original' }; default { throw 'Invalid choice.' } }
Write-Host 'BIOS is optional: it is only used to capture the console startup. The game and HD media work without it.'
$bios = ''
if ((Read-Host 'Add the original PlayStation startup from your PS1 BIOS dump? (y/N)') -match '^(y|yes)$') {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select your PS1 BIOS dump'
    $dialog.Filter = 'PS1 BIOS|*.bin;*.rom|All files|*.*'
    try { if ($dialog.ShowDialog() -ne 'OK') { throw 'No BIOS selected.' }; $bios = $dialog.FileName }
    finally { $dialog.Dispose() }
}
if ($mode -eq 'Original' -and !$bios) { Write-Host 'No changes selected.'; return }
& (Join-Path $PSScriptRoot 'prepare-hd.ps1') -Runtime $Runtime -BuildDir $BuildDir -HdMedia $mode -Bios $bios
Write-Host 'PC assets are ready. To update Quest, run scripts/install-quest.ps1 with this Runtime and the updated APK.'
