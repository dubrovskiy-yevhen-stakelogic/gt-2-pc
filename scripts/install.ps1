[CmdletBinding()]
param(
    [string[]]$DiscImage,
    [string]$InstallDir = (Join-Path (Split-Path $PSScriptRoot) 'runtime'),
    [string]$BuildDir = 'build_install',
    [switch]$SkipDependencies,
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }

function Refresh-ToolPath {
    $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') + ';' + $env:Path
    if (Test-Path "$env:ProgramFiles\CMake\bin") { $env:Path = "$env:ProgramFiles\CMake\bin;" + $env:Path }
    $sdk = @([Environment]::GetEnvironmentVariable('VULKAN_SDK','Machine'), $env:VULKAN_SDK)
    if (Test-Path 'C:\VulkanSDK') { $sdk += @(Get-ChildItem 'C:\VulkanSDK' -Directory | Sort-Object Name -Descending | ForEach-Object FullName) }
    foreach ($path in $sdk) { if ($path -and (Test-Path (Join-Path $path 'Bin\glslc.exe'))) { $env:VULKAN_SDK = $path; break } }
}
function Install-Dependency([string]$id, [string[]]$extra = @()) {
    if ($SkipDependencies) { throw "Missing dependency $id. Re-run without -SkipDependencies." }
    if (!(Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'Install Microsoft App Installer (winget), then run INSTALL.bat again.' }
    & winget.exe install --exact --source winget --id $id --accept-source-agreements --accept-package-agreements --disable-interactivity @extra
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "Dependency installation failed: $id ($LASTEXITCODE)" }
    Refresh-ToolPath
}
function Find-Vs {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
}
Refresh-ToolPath
if (!$NoBuild) {
    if (!(Get-Command cmake.exe -ErrorAction SilentlyContinue)) { Install-Dependency 'Kitware.CMake' }
    if (!(Find-Vs)) {
        Install-Dependency 'Microsoft.VisualStudio.2022.BuildTools' @('--force', '--override', '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    }
    if (!$env:VULKAN_SDK -or !(Test-Path "$env:VULKAN_SDK\Bin\glslc.exe")) { Install-Dependency 'KhronosGroup.VulkanSDK' }
    if (!(Find-Vs)) { throw 'C++ Build Tools are still unavailable. Restart Windows if the installer requested it.' }
    if (!(Get-Command ninja.exe -ErrorAction SilentlyContinue) -and !(Test-Path (Join-Path (Find-Vs) 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'))) { Install-Dependency 'Ninja-build.Ninja' }
    & (Join-Path $repo 'build.cmd') $build 'install'
    if ($LASTEXITCODE -ne 0) { throw 'The source build failed; no installation was replaced.' }
    & ctest.exe --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed; no installation was replaced.' }
}
$tool = Join-Path $build 'gt2install.exe'
$game = Join-Path $build 'gt2game.exe'
if (!(Test-Path $tool) -or !(Test-Path $game)) { throw 'Build outputs gt2game.exe and gt2install.exe are required.' }

if (!$DiscImage -or $DiscImage.Count -eq 0) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select GT2 US or European Arcade and/or Simulation disc images'
    $dialog.Filter = 'PS1 disc images or archives|*.bin;*.cue;*.iso;*.zip;*.7z|All files|*.*'
    $dialog.Multiselect = $true
    if ($dialog.ShowDialog() -ne 'OK') { throw 'No disc image selected.' }
    $DiscImage = @($dialog.FileNames)
    $dialog.Dispose()
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$job = Join-Path $InstallDir ('.install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $job | Out-Null
$sources = @()
foreach ($inputPath in $DiscImage) {
    $source = (Resolve-Path -LiteralPath $inputPath).Path
    $extension = [IO.Path]::GetExtension($source).ToLowerInvariant()
    if ($extension -eq '.zip' -or $extension -eq '.7z') {
        $unpacked = Join-Path $job ([Guid]::NewGuid().ToString('N'))
        if ($extension -eq '.zip') { Expand-Archive -LiteralPath $source -DestinationPath $unpacked }
        else {
            $seven = "$env:ProgramFiles\7-Zip\7z.exe"
            if (!(Test-Path $seven)) { Install-Dependency '7zip.7zip' }
            & $seven x $source "-o$unpacked" -y
            if ($LASTEXITCODE -ne 0) { throw '7-Zip could not unpack the disc archive.' }
        }
        $images = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File | Where-Object { $_.Extension -in '.bin','.iso' })
        if ($images.Count -eq 0) { throw 'Archive has no BIN or ISO images.' }
        $sources += @($images | ForEach-Object FullName)
    } elseif ($extension -eq '.cue') {
        $cue = Get-Content -LiteralPath $source -Raw
        $files = [regex]::Matches($cue, '(?im)^\s*FILE\s+"([^"]+)"\s+BINARY\s*$')
        if ($files.Count -ne 1 -or $cue -notmatch '(?im)TRACK\s+0?1\s+MODE2/2352') { throw 'Use a single data-track MODE2/2352 BIN/CUE dump.' }
        $sources += (Resolve-Path -LiteralPath (Join-Path (Split-Path $source) $files[0].Groups[1].Value)).Path
    } else { $sources += $source }
}

$prepared = @()
$modes = @{}
foreach ($source in ($sources | Select-Object -Unique)) {
    $json = & $tool inspect $source
    if ($LASTEXITCODE -ne 0) { throw "Unsupported or damaged image: $source" }
    $info = $json | ConvertFrom-Json
    if ($modes.ContainsKey($info.mode)) { throw "Select only one $($info.mode) disc." }
    $modes[$info.mode] = $true
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $data = Join-Path $job $info.mode
    & $tool extract $source $data
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed. Diagnostics and partial files remain in $job" }
    if ((Get-FileHash -LiteralPath (Join-Path $data 'disc.raw2352') -Algorithm SHA256).Hash -ne $hash) { throw 'Disc copy hash mismatch.' }
    & $game $data --scan-courses > (Join-Path $data 'course-check.log')
    if ($LASTEXITCODE -ne 0) { throw 'Installed course validation failed.' }
    & $game $data --selftest --cars 6 > (Join-Path $data 'selftest.log')
    if ($LASTEXITCODE -ne 0) { throw 'Installed physics self-test failed.' }
    $checks = Join-Path $build 'gt2checks.exe'
    if (!(Test-Path -LiteralPath $checks)) { throw 'gt2checks.exe is required to validate disc menus and media.' }
    & $checks $data > (Join-Path $data 'media-check.log')
    if ($LASTEXITCODE -ne 0) { throw 'Installed menu/media regression checks failed.' }
    $assetHashes = @(Get-ChildItem -LiteralPath (Join-Path $data 'assets') -Recurse -File | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($data.Length + 1).Replace('\','/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    [ordered]@{ format = 1; mode = $info.mode; exeSha1 = $info.exeSha1; discSha256 = $hash; assets = $assetHashes } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $data 'install-manifest.json') -Encoding UTF8
    $prepared += [pscustomobject]@{ Mode = $info.mode; Data = $data }
}

# Publish only validated data. Keep old data and executables for rollback, and retain all saves.
$backup = Join-Path $InstallDir ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $backup | Out-Null
foreach ($item in $prepared) {
    $destination = Join-Path $InstallDir $item.Mode
    if (Test-Path $destination) { Move-Item -LiteralPath $destination -Destination (Join-Path $backup $item.Mode) }
    Move-Item -LiteralPath $item.Data -Destination $destination
}
foreach ($name in 'gt2game.exe','gt2install.exe') {
    $destination = Join-Path $InstallDir $name
    if (Test-Path $destination) { Copy-Item -LiteralPath $destination -Destination (Join-Path $backup $name) }
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination -Force
}
foreach ($mode in 'arcade','simulation') {
    if (Test-Path (Join-Path $InstallDir $mode)) {
        $launcher = @'
@echo off
setlocal
cd /d "%~dp0"
"%~dp0gt2game.exe" "%~dp0MODE" --settings "%~dp0saves\MODE\settings.txt" --card "%~dp0saves\MODE\card1.mcd" %*
'@.Replace('MODE', $mode)
        [IO.File]::WriteAllText((Join-Path $InstallDir "PLAY-$mode.bat"), $launcher, [Text.Encoding]::ASCII)
        New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir "saves\$mode") | Out-Null
    }
}
[IO.File]::WriteAllText((Join-Path $repo 'install-location.txt'), $InstallDir, [Text.Encoding]::UTF8)
Write-Host "Ready: $InstallDir"
Write-Host 'Use PLAY-arcade.bat or PLAY-simulation.bat. F10 opens the in-game settings and cheat overlay.'
Write-Host "Existing data is preserved in $backup. Installation staging is in $job."
