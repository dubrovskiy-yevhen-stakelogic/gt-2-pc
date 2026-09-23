[CmdletBinding()]
param(
    [string[]]$DiscImage,
    [ValidateSet('Quest','PC','PCVR')][string]$Target = 'Quest',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime'),
    [string]$Adb,
    [string]$Serial,
    [ValidateSet('Original','Menus','MenusAndMovies')][string]$HdMedia = 'Menus',
    [string]$Bios,
    [string]$CaptureCore,
    [switch]$NoBios,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $root 'release-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -ne '0.5.0') { throw 'Unexpected release version.' }
$seen = @{}
foreach ($entry in $manifest.files) {
    if ($entry.path -match '(^|[\\/])\.\.([\\/]|$)|^[\\/]|:' -or $seen.ContainsKey($entry.path)) { throw 'Invalid release manifest path.' }
    $seen[$entry.path] = $true
    $path = Join-Path $root $entry.path
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Release file failed verification: $($entry.path)" }
}
foreach ($required in @('GT2-VR-0.5.0.apk','tools/gt2game.exe','tools/openxr_loader.dll','tools/gt2install.exe','tools/gt2checks.exe','scripts/install-player.ps1','scripts/install.ps1','scripts/install-quest.ps1','scripts/platform-tools.ps1','scripts/transfer-saves.ps1')) {
    if (!$seen.ContainsKey($required)) { throw "Required release file is missing from the manifest: $required" }
}
if ($VerifyOnly) { Write-Host 'Release files verified.'; return }
if ($Bios -and $NoBios) { throw 'Choose either -Bios or -NoBios.' }
if (!$Bios -and !$NoBios -and !$DiscImage) {
    Write-Host 'PlayStation startup is optional. The game works without a BIOS.'
    Write-Host 'Existing prepared intros are retained; disable them in the VR menu if desired.'
    $choice = Read-Host 'Prepare the original startup from your own PS1 BIOS? (y/N)'
    if ($choice -match '^(y|yes)$') {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Optional PS1 BIOS dump (512 KiB)'
        $dialog.Filter = 'BIOS files (*.bin;*.rom)|*.bin;*.rom|All files (*.*)|*.*'
        try { if ($dialog.ShowDialog() -eq 'OK') { $Bios = $dialog.FileName } }
        finally { $dialog.Dispose() }
    }
}
if ($Bios -and (!(Test-Path -LiteralPath $Bios -PathType Leaf) -or (Get-Item -LiteralPath $Bios).Length -ne 524288)) {
    throw 'Select a 512 KiB BIOS dump, or use -NoBios to play without the console startup.'
}
. (Join-Path $PSScriptRoot 'platform-tools.ps1')
$adbPath = $null
if ($Target -eq 'Quest') {
    $adbPath = Get-Adb
    $devices = @(& $adbPath devices)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot query the headset.' }
    $connected = @($devices | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if (!$Serial) {
        if ($connected.Count -ne 1) { throw 'Connect one Quest and accept USB debugging inside the headset.' }
        $Serial = $connected[0]
    }
    if ($Serial -notin $connected) { throw 'Selected Quest is not connected and authorized.' }
}
& (Join-Path $PSScriptRoot 'install.ps1') -DiscImage $DiscImage -InstallDir $InstallDir -BuildDir (Join-Path $root 'tools') -NoBuild -HdMedia $HdMedia -Bios $Bios -CaptureCore $CaptureCore
if ($Target -eq 'Quest') {
    $modes = @('arcade','simulation' | Where-Object { Test-Path -LiteralPath (Join-Path $InstallDir "$_/disc.raw2352") })
    if ($modes.Count -eq 0) { throw 'No prepared discs.' }
    & (Join-Path $PSScriptRoot 'install-quest.ps1') -Adb $adbPath -Serial $Serial -Apk (Join-Path $root 'GT2-VR-0.5.0.apk') -Runtime $InstallDir -Mode $modes[0] -BothDiscs:($modes.Count -eq 2)
} else { Write-Host "PC game ready: $InstallDir. Use PLAY.bat for desktop, PLAY-PCVR-META.bat for Meta Link, PLAY-PCVR-STEAMVR.bat for SteamVR, or PLAY-PCVR-VD.bat for Virtual Desktop (VDXR). PLAY-PCVR.bat selects automatically." }
