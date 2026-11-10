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
    [string]$Gt4Bundle,
    [string]$Python = 'python',
    [switch]$NoBios,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $root 'release-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw 'Unexpected release version.' }
$apkName = "GT2-VR-$($manifest.version).apk"
$seen = @{}
foreach ($entry in $manifest.files) {
    if ($entry.path -match '(^|[\\/])\.\.([\\/]|$)|^[\\/]|:' -or $seen.ContainsKey($entry.path)) { throw 'Invalid release manifest path.' }
    $seen[$entry.path] = $true
    $path = Join-Path $root $entry.path
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Release file failed verification: $($entry.path)" }
}
$required = @('tools/gt2game.exe','tools/openxr_loader.dll','tools/gt2install.exe','tools/gt2checks.exe','scripts/install-player.ps1','scripts/install.ps1','scripts/launch.ps1','scripts/platform-tools.ps1','scripts/transfer-saves.ps1')
if ($Target -eq 'Quest') { $required += @($apkName,'scripts/install-quest.ps1') }
foreach ($requiredFile in $required) {
    if (!$seen.ContainsKey($requiredFile)) { throw "Required release file is missing from the manifest: $requiredFile" }
}
if ($VerifyOnly) { Write-Host 'Release files verified.'; return }
if (!$PSBoundParameters.ContainsKey('InstallDir')) {
    $locationFile = Join-Path $root 'install-location.txt'
    if (Test-Path -LiteralPath $locationFile) {
        $saved = (Get-Content -LiteralPath $locationFile -Raw -Encoding UTF8).Trim()
        if ($saved) { $InstallDir = $saved }
    }
    if ($Target -ne 'Quest') {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = 'Choose where GT2 keeps discs, saves and media. Keep it outside the program folder.'
        $dialog.SelectedPath = $env:USERPROFILE
        if (Test-Path -LiteralPath $InstallDir) { $dialog.SelectedPath = $InstallDir }
        $dialog.ShowNewFolderButton = $true
        try {
            if ($dialog.ShowDialog() -ne 'OK') { throw 'No game data folder selected.' }
            $InstallDir = $dialog.SelectedPath
        } finally { $dialog.Dispose() }
    }
}
if ($Gt4Bundle) {
    . (Join-Path $PSScriptRoot 'windows-prerequisites.ps1')
    $Python = Get-Gt2Python -Python $Python
    & $Python -E -s (Join-Path $PSScriptRoot 'package-quest-gt4.py') --verify $Gt4Bundle
    if ($LASTEXITCODE -ne 0) { throw 'GT4 bundle validation failed; nothing was installed.' }
}
if (!$PSBoundParameters.ContainsKey('HdMedia') -and !$DiscImage) {
    . (Join-Path $PSScriptRoot 'hd-upscale.ps1')
    $HdMedia = Select-HdMediaForInstall
}
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
    $modes = @('arcade','simulation','combined' | Where-Object { Test-Path -LiteralPath (Join-Path $InstallDir "$_/disc.raw2352") })
    if ($modes.Count -eq 0) { throw 'No prepared discs.' }
    $questMode = if ($modes -contains 'combined') { 'combined' } else { $modes[0] }
    $both = ($modes -contains 'arcade') -and ($modes -contains 'simulation')
    & (Join-Path $PSScriptRoot 'install-quest.ps1') -Adb $adbPath -Serial $Serial -Apk (Join-Path $root $apkName) -Runtime $InstallDir -Mode $questMode -BothDiscs:$both -Gt4Bundle $Gt4Bundle -Python $Python
} else {
    if ($Gt4Bundle) {
        & $Python -E -s (Join-Path $PSScriptRoot 'install-gt4-pack.py') --bundle $Gt4Bundle --runtime $InstallDir
        if ($LASTEXITCODE -ne 0) { throw 'GT4 installation failed; see the error above.' }
    }
    Write-Host "PC game ready: $InstallDir. Use PLAY.bat for desktop, PLAY-PCVR-META.bat for Meta Link, PLAY-PCVR-STEAMVR.bat for SteamVR, or PLAY-PCVR-VD.bat for Virtual Desktop (VDXR). PLAY-PCVR.bat selects automatically."
}
