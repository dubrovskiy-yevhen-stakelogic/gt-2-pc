param([string]$Emsdk = $env:EMSDK, [string]$Build = 'build_web', [switch]$DebugBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$Emsdk) { $Emsdk = Join-Path $root 'work/toolchains/emsdk-6.0.10' }
$emcmake = Join-Path $Emsdk 'upstream/emscripten/emcmake.exe'
if (!(Test-Path -LiteralPath $emcmake)) { $emcmake = Join-Path $Emsdk 'upstream/emscripten/emcmake.bat' }
if (!(Test-Path -LiteralPath $emcmake)) { throw 'Emscripten SDK is missing. Pass -Emsdk <official activated SDK folder>.' }
$buildPath = [IO.Path]::GetFullPath((Join-Path $root $Build))
$config = if ($DebugBuild) { 'Debug' } else { 'Release' }
# emcmake uses the SDK's activated .emscripten file; no global PATH mutation.
& $emcmake cmake -S $root -B $buildPath -G Ninja "-DCMAKE_BUILD_TYPE=$config" -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { throw 'Web CMake configuration failed.' }
& cmake --build $buildPath --target gt2web --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'Web compilation failed.' }
Write-Host "Browser build: $buildPath/site/index.html"
Write-Host "Serve locally: python scripts/serve-web.py --directory `"$buildPath/site`""
