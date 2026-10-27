param([string]$Output, [string]$Web, [string]$Native)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$project=Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(gt2pc VERSION ([0-9]+\.[0-9]+\.[0-9]+) ') { throw 'Cannot determine release version.' }
$version=$Matches[1]
if (!$Output) { $Output="dist/GT2-$version" }
if (!$Web) { $Web="dist/GT2-Web-$version" }
if (!$Native) { $Native="dist/GT2-native-$version" }
$destination=[IO.Path]::GetFullPath((Join-Path $root $Output))
$webPath=[IO.Path]::GetFullPath((Join-Path $root $Web))
$nativePath=[IO.Path]::GetFullPath((Join-Path $root $Native))
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath ($destination+'.zip'))) { throw 'Choose a new output; existing releases are preserved.' }
& (Join-Path $nativePath 'scripts/install-player.ps1') -VerifyOnly
$webManifest=Get-Content (Join-Path $webPath 'BUILD-MANIFEST.json') -Raw | ConvertFrom-Json
foreach ($entry in $webManifest.files) {
    if ($entry.path -match '(^|[\\/])\.\.([\\/]|$)|^[\\/]|:') { throw 'Invalid web manifest path.' }
    if ((Get-FileHash -LiteralPath (Join-Path $webPath $entry.path)).Hash -ne $entry.sha256) { throw "Web file changed: $($entry.path)" }
}
& (Join-Path $PSScriptRoot 'package-source.ps1') -Output $destination -FileSystem
$snapshot=Get-Content (Join-Path $destination 'SOURCE-MANIFEST.json') -Raw | ConvertFrom-Json
if ($snapshot.version -ne $webManifest.version) { throw 'Source and web versions differ.' }
$nativeManifest=Get-Content (Join-Path $nativePath 'release-manifest.json') -Raw | ConvertFrom-Json
if ($snapshot.version -ne $nativeManifest.version) { throw 'Native and source versions differ.' }
foreach($entry in @($nativeManifest.files) + @([pscustomobject]@{path='release-manifest.json'})) {
    $target=Join-Path $destination ('native/'+$entry.path)
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $nativePath $entry.path) -Destination $target
}
foreach($platform in @('PC','PCVR','QUEST')) {
    $launcher=if($platform -eq 'QUEST') { 'INSTALL.bat' } else { "INSTALL-$platform.bat" }
    $body="@echo off`r`ncall `"%~dp0native\$launcher`" %*`r`nexit /b %errorlevel%`r`n"
    [IO.File]::WriteAllText((Join-Path $destination "INSTALL-$platform.bat"),$body,[Text.Encoding]::ASCII)
}
New-Item -ItemType Directory -Path (Join-Path $destination 'web') | Out-Null
foreach ($entry in @($webManifest.files) + @([pscustomobject]@{path='BUILD-MANIFEST.json'})) {
    $target=Join-Path $destination ('web/'+$entry.path)
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $webPath $entry.path) -Destination $target
}
$files=@(Get-ChildItem -LiteralPath $destination -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($destination.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
})
$obsoleteLaunchers = @('TRY-GT4-MODELS-DESKTOP.bat','TRY-GT4-MODELS-VR.bat','try-gt4-models.ps1',
    'PLAY-PC.bat','PLAY-2D.bat','gt2_arcade.bat','gt2_race.bat','gt2_sim.bat','run_race.bat')
foreach ($entry in $files) {
    if ([IO.Path]::GetFileName($entry.path) -in $obsoleteLaunchers) { throw "Obsolete launcher in release: $($entry.path)" }
}
[ordered]@{version=$snapshot.version;sourceProvenance='filesystem-sha256';contents='Common source, all platform scripts, Windows executables, signed Quest APK and compiled browser site';files=$files} |
    ConvertTo-Json -Depth 5 | Set-Content (Join-Path $destination 'PACKAGE-MANIFEST.json') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::Open($destination+'.zip',[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach($path in @($files.path)+'PACKAGE-MANIFEST.json') {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,(Join-Path $destination $path),([IO.Path]::GetFileName($destination)+'/'+$path),[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }
$hash=(Get-FileHash -LiteralPath ($destination+'.zip')).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($destination+'.zip.sha256', $hash+'  '+[IO.Path]::GetFileName($destination)+'.zip'+[Environment]::NewLine)
Write-Host "Complete release: $destination.zip ($hash)"
