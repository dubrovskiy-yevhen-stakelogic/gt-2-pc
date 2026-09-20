param(
    [string]$AndroidSdk = $env:ANDROID_HOME,
    [string]$Gradle = 'gradle',
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$AndroidSdk) { $AndroidSdk = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
if (!(Test-Path (Join-Path $AndroidSdk 'ndk/27.2.12479018'))) { throw 'Install Android NDK 27.2.12479018 in the selected SDK.' }
if (!$env:VULKAN_SDK -or !(Test-Path (Join-Path $env:VULKAN_SDK 'Bin/glslc.exe'))) { throw 'Set VULKAN_SDK to an installed Vulkan SDK with glslc.' }
$env:ANDROID_HOME = [IO.Path]::GetFullPath($AndroidSdk)
$property = 'sdk.dir=' + $env:ANDROID_HOME.Replace('\','/')
[IO.File]::WriteAllText((Join-Path $repo 'android/local.properties'), $property, [Text.UTF8Encoding]::new($false))
$argsList = @('-p', (Join-Path $repo 'android'), '--no-daemon', 'assembleDebug')
if ($Offline) { $argsList += '--offline' }
& $Gradle @argsList
if ($LASTEXITCODE -ne 0) { throw "Quest build failed ($LASTEXITCODE)" }
Get-Item (Join-Path $repo 'android/app/build/outputs/apk/debug/app-debug.apk')
