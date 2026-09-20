[CmdletBinding()]
param(
    [string[]]$DiscImage,
    [ValidateSet('Quest','PC')][string]$Target = 'Quest',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime'),
    [string]$Adb,
    [string]$Serial,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $root 'release-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -ne '0.1.0') { throw 'Unexpected release version.' }
$seen = @{}
foreach ($entry in $manifest.files) {
    if ($entry.path -match '(^|[\\/])\.\.([\\/]|$)|^[\\/]|:' -or $seen.ContainsKey($entry.path)) { throw 'Invalid release manifest path.' }
    $seen[$entry.path] = $true
    $path = Join-Path $root $entry.path
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Release file failed verification: $($entry.path)" }
}
foreach ($required in @('GT2-VR-0.1.0.apk','tools/gt2game.exe','tools/gt2install.exe','tools/gt2checks.exe','scripts/install-player.ps1','scripts/install.ps1','scripts/install-quest.ps1')) {
    if (!$seen.ContainsKey($required)) { throw "Required release file is missing from the manifest: $required" }
}
if ($VerifyOnly) { Write-Host 'Release files verified.'; return }
function Get-Adb {
    if ($Adb) { return (Get-Command $Adb -ErrorAction Stop).Source }
    $found = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    $cache = Join-Path $env:LOCALAPPDATA 'GT2-VR/Tools/platform-tools-36.0.2'
    $hashes = [ordered]@{
        'adb.exe' = '56656270DA132F44E9CB4FB86A12BA965635C80423D43DCDD944D9FEC4AB4622'
        'AdbWinApi.dll' = 'A00CF631DD12C82561FFCCEFDB1A99A27C527253B04F06CA8FC1BB86BA2148C4'
        'AdbWinUsbApi.dll' = 'E4D72D5BA3BF4B027F1B7A3781AAE0B04712C1A52244BEA277FB57DD75A85702'
        'NOTICE.txt' = 'BFEDFB7B22C5D204BAECBCCFBB9A6DE6E848EF19F9318088F15769BFBF4B8F79'
        'source.properties' = '6F16C7815EE0A1B820CA24C8FDAD8E26F07DA492B22C2B370C6DB136425A2136'
    }
    $valid = $true
    foreach ($name in $hashes.Keys) {
        $path = Join-Path $cache $name
        if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$name]) { $valid = $false }
    }
    if ($valid) { return (Join-Path $cache 'adb.exe') }
    Write-Host 'Downloading Android Platform Tools 36.0.2 from Google. SDK terms: https://developer.android.com/studio/terms'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $temporary = Join-Path $cache ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporary | Out-Null
    $zip = Join-Path $temporary 'platform-tools.zip'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest 'https://dl.google.com/android/repository/platform-tools_r36.0.2-win.zip' -OutFile $zip -UseBasicParsing
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne 'B024D4F319D6AD3004DE1BA7B96A5C7C5F3512E8B14126308D598B4AB93DCEAD') { throw 'Platform Tools download hash mismatch.' }
    Expand-Archive -LiteralPath $zip -DestinationPath $temporary
    foreach ($name in $hashes.Keys) {
        $path = Join-Path $temporary ('platform-tools/' + $name)
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$name]) { throw "Platform Tools hash mismatch: $name" }
    }
    foreach ($name in $hashes.Keys) { Copy-Item -LiteralPath (Join-Path $temporary ('platform-tools/' + $name)) -Destination (Join-Path $cache $name) -Force }
    return (Join-Path $cache 'adb.exe')
}
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
& (Join-Path $PSScriptRoot 'install.ps1') -DiscImage $DiscImage -InstallDir $InstallDir -BuildDir (Join-Path $root 'tools') -NoBuild
if ($Target -eq 'Quest') {
    $modes = @('arcade','simulation' | Where-Object { Test-Path -LiteralPath (Join-Path $InstallDir "$_/disc.raw2352") })
    if ($modes.Count -eq 0) { throw 'No prepared discs.' }
    & (Join-Path $PSScriptRoot 'install-quest.ps1') -Adb $adbPath -Serial $Serial -Apk (Join-Path $root 'GT2-VR-0.1.0.apk') -Runtime $InstallDir -Mode $modes[0] -BothDiscs:($modes.Count -eq 2)
} else { Write-Host "PC game ready: $InstallDir. Use PLAY-arcade.bat or PLAY-simulation.bat." }