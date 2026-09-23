param(
    [Parameter(Mandatory)][string]$OpenXRLoader,
    [string]$BuildDir = 'build_update',
    [string]$Apk = 'dist/GT2-VR-0.5.0.apk',
    [Parameter(Mandatory)][string]$AndroidSdk,
    [string]$Output = 'dist/GT2-0.5.0',
    [string]$SourceManifest,
    [switch]$AllowUncommitted,
    [switch]$FileSystem
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
function Full([string]$p) { if ([IO.Path]::IsPathRooted($p)) { return [IO.Path]::GetFullPath($p) }; return [IO.Path]::GetFullPath((Join-Path $repo $p)) }
$Output = Full $Output
$BuildDir = Full $BuildDir
$Apk = Full $Apk
$archivePath = $Output + '.zip'
if ((Test-Path -LiteralPath $Output) -or (Test-Path -LiteralPath $archivePath)) { throw 'Output already exists. Choose a new directory.' }
& (Join-Path $PSScriptRoot 'audit-source.ps1') -Repo $repo -FileSystem:$FileSystem
Push-Location $repo
try {
    if ($FileSystem) {
        $sourceDirty = $null
        $revision = $null
    } else {
        & git diff --quiet HEAD --
        $sourceDirty = $LASTEXITCODE -ne 0 -or @(& git ls-files --others --exclude-standard src tools tests cmake scripts docs third_party).Count -gt 0
        if ($sourceDirty -and !$AllowUncommitted) { throw 'Source has uncommitted changes. Use -AllowUncommitted to package the working files.' }
        $revision = (& git rev-parse HEAD).Trim()
    }
} finally { Pop-Location }
$sourceManifestHash = $null
if ($SourceManifest) {
    $SourceManifest = Full $SourceManifest
    $sourceSnapshot = Get-Content -LiteralPath $SourceManifest -Raw | ConvertFrom-Json
    if ($sourceSnapshot.version -ne '0.5.0') { throw 'Unexpected source snapshot version.' }
    $sourceFiles = @(& (Join-Path $PSScriptRoot 'source-files.ps1') -Repo $repo -FileSystem:$FileSystem)
    if (@(Compare-Object $sourceFiles @($sourceSnapshot.files.path)).Count -or $sourceFiles.Count -ne $sourceSnapshot.files.Count) { throw 'Source snapshot inventory differs from the working source.' }
    foreach ($entry in $sourceSnapshot.files) {
        if ((Get-FileHash -LiteralPath (Join-Path $repo $entry.path) -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Source changed after export: $($entry.path)" }
    }
    $sourceManifestHash = (Get-FileHash -LiteralPath $SourceManifest -Algorithm SHA256).Hash.ToLowerInvariant()
}
$bin = Join-Path $AndroidSdk 'build-tools/35.0.0'
& (Join-Path $bin 'apksigner.bat') verify $Apk
if ($LASTEXITCODE -ne 0) { throw 'APK signature is invalid.' }
$badging = @(& (Join-Path $bin 'aapt.exe') dump badging $Apk) -join "`n"
if ($LASTEXITCODE -ne 0 -or $badging -notmatch "versionName='0.5.0'" -or $badging -notmatch "versionCode='23'" -or $badging -match 'application-debuggable') { throw 'APK release metadata is wrong.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$apkZip = [IO.Compression.ZipFile]::OpenRead($Apk)
try {
    $libs = @($apkZip.Entries | Where-Object FullName -Match '^lib/' | ForEach-Object FullName)
    if ($libs.Count -ne 2 -or 'lib/arm64-v8a/libgt2game.so' -notin $libs -or 'lib/arm64-v8a/libopenxr_loader.so' -notin $libs) { throw 'Unexpected APK native libraries.' }
    if (@($apkZip.Entries | Where-Object FullName -Match '^assets/').Count) { throw 'Unexpected APK assets; review the payload.' }
} finally { $apkZip.Dispose() }
foreach ($name in @('gt2game.exe','gt2install.exe','gt2checks.exe','gt2media.exe','gt2bootcapture.exe')) { if (!(Test-Path -LiteralPath (Join-Path $BuildDir $name))) { throw "Missing Windows executable: $name" } }
if (!(Test-Path -LiteralPath $OpenXRLoader -PathType Leaf)) { throw 'Supply the official x64 Khronos openxr_loader.dll.' }
foreach ($dir in @('','tools','scripts','docs','LICENSES')) { New-Item -ItemType Directory -Force -Path (Join-Path $Output $dir) | Out-Null }
Copy-Item -LiteralPath $OpenXRLoader -Destination (Join-Path $Output 'tools/openxr_loader.dll')
Copy-Item -LiteralPath $Apk -Destination (Join-Path $Output 'GT2-VR-0.5.0.apk')
foreach ($name in @('gt2game.exe','gt2install.exe','gt2checks.exe','gt2media.exe','gt2bootcapture.exe')) { Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $Output "tools/$name") }
foreach ($name in @('transfer-saves.ps1','platform-tools.ps1','install-player.ps1','install.ps1','install-quest.ps1','collect-profiler-quest.ps1','prepare-hd.ps1','prepare-hd-wizard.ps1','install-linux.py','gt2_disc.py','gt2_boot.py','linux-downloads.json')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $Output "scripts/$name") }
foreach ($name in @('README.md','THIRD_PARTY.md','LICENSE','CHANGELOG.md','PREPARE-HD.bat','INSTALL-LINUX.sh','TRANSFER_QUEST_SAVES_TO_PC.bat','TRANSFER_PC_SAVES_TO_QUEST.bat')) { Copy-Item -LiteralPath (Join-Path $repo $name) -Destination (Join-Path $Output $name) }
foreach ($name in @('PCVR.md','SAVE-TRANSFER.md','PLAYER-INSTALL.md','QUEST.md','VALIDATION.md','HD-MEDIA.md','MIPMAPS.md','LINUX-INSTALL.md','WHEELS.md','WHEEL-PROFILES.md','COCKPIT.md','images/cockpit-0.5.0.png','wheel-profile-provenance.json','formats/steering_feedback.md')) {
    $destination = Join-Path $Output "docs/$name"
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "docs/$name") -Destination $destination
}
$licences = @{
    'OpenXR-Apache-2.0.txt' = (Join-Path $repo 'third_party/openxr/LICENSE')
    'JsonCpp-LICENSE.txt' = (Join-Path $repo 'third_party/openxr/JSONCPP-LICENSE.txt')
    'UltimateXR-MIT.txt' = (Join-Path $repo 'third_party/vrhands/ULTIMATEXR_LICENSE.txt')
    'MiamiVR-MIT.txt' = (Join-Path $repo 'third_party/vrhands/MIAMIVR_LICENSE.txt')
    'stb-LICENSE.txt' = (Join-Path $repo 'third_party/stb/LICENSE.txt')
    'xBR-MIT.txt' = (Join-Path $repo 'third_party/xbr/LICENSE.txt')
    'libretro-MIT.txt' = (Join-Path $repo 'third_party/libretro/LICENSE.txt')
    'Android-NDK-NOTICE.txt' = (Join-Path $AndroidSdk 'ndk/27.2.12479018/NOTICE')
    'Android-toolchain-NOTICE.txt' = (Join-Path $AndroidSdk 'ndk/27.2.12479018/NOTICE.toolchain')
}
foreach ($name in $licences.Keys) { Copy-Item -LiteralPath $licences[$name] -Destination (Join-Path $Output "LICENSES/$name") }
foreach ($target in @('Quest','PC','PCVR')) {
    $name = if ($target -eq 'Quest') { 'INSTALL.bat' } elseif ($target -eq 'PCVR') { 'INSTALL-PCVR.bat' } else { 'INSTALL-PC.bat' }
    $body = @'
@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install-player.ps1" -Target TARGET %*
if errorlevel 1 (
  echo Installation failed. Existing app data was not uninstalled.
  pause
  exit /b 1
)
echo Installation completed.
pause
'@.Replace('TARGET',$target)
    [IO.File]::WriteAllText((Join-Path $Output $name),$body.Replace("`r`n","`n").Replace("`n","`r`n"),[Text.Encoding]::ASCII)
}
$files = @(Get-ChildItem -LiteralPath $Output -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path=$_.FullName.Substring($Output.Length+1).Replace('\','/'); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[ordered]@{ version='0.5.0'; versionCode=23; sourceCommit=$revision; sourceDirty=$sourceDirty; sourceProvenance=$(if ($FileSystem) { 'filesystem-sha256' } else { 'git' }); sourceManifestSha256=$sourceManifestHash; package='io.github.gt2pc.quest'; abi='arm64-v8a'; debuggable=$false; files=$files } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Output 'release-manifest.json') -Encoding UTF8
& (Join-Path $Output 'scripts/install-player.ps1') -VerifyOnly
$prefix = [IO.Path]::GetFileName($Output) + '/'
$archive = [IO.Compression.ZipFile]::Open($archivePath,[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($relative in (@($files.path) + 'release-manifest.json')) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,(Join-Path $Output $relative),
            ($prefix + $relative),[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $expected = @($files.path) + 'release-manifest.json'
    $entries = @($zip.Entries | Where-Object { $_.Name })
    if ($entries.Count -ne $expected.Count) { throw 'ZIP file count mismatch.' }
    foreach ($entry in $entries) {
        if (!$entry.FullName.StartsWith($prefix)) { throw 'Unexpected ZIP root.' }
        $relative = $entry.FullName.Substring($prefix.Length)
        if ($relative -notin $expected) { throw "Unexpected ZIP file: $relative" }
        $stream=$entry.Open(); $sha=[Security.Cryptography.SHA256]::Create()
        try { $hash=([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') } finally { $stream.Dispose(); $sha.Dispose() }
        if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $Output $relative) -Algorithm SHA256).Hash) { throw "ZIP hash mismatch: $relative" }
    }
} finally { $zip.Dispose() }
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($archivePath+'.sha256'),$hash+'  '+[IO.Path]::GetFileName($archivePath)+[Environment]::NewLine,[Text.Encoding]::ASCII)
Write-Host "Verified release: $Output"
Write-Host "Verified ZIP: $archivePath ($hash)"
