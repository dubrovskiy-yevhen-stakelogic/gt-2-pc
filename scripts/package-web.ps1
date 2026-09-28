param([string]$Build='build_web', [string]$Output='dist/GT2-Web-0.8.0', [string]$Emsdk='work/toolchains/emsdk-6.0.10')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$site=Join-Path (Join-Path $root $Build) 'site'
$destination=[IO.Path]::GetFullPath((Join-Path $root $Output))
$sdk=Join-Path (Join-Path $root $Emsdk) 'upstream/emscripten'
if (Test-Path -LiteralPath $destination) { throw 'Output already exists; choose a new output folder.' }
if (Test-Path -LiteralPath ($destination+'.zip')) { throw 'Output ZIP already exists; choose a new output folder.' }
$files=@('index.html','launcher.js','gt2.js','gt2.wasm')
foreach($name in $files) { if (!(Test-Path -LiteralPath (Join-Path $site $name))) { throw "Missing build output: $name" } }
$licenses=@{
  'Emscripten.txt'=(Join-Path $sdk 'LICENSE')
  'SDL2.txt'=(Join-Path $sdk 'cache/ports/sdl2/SDL-release-2.32.10/LICENSE.txt')
  'musl.txt'=(Join-Path $sdk 'system/lib/libc/musl/COPYRIGHT')
  'libcxx.txt'=(Join-Path $sdk 'system/lib/libcxx/LICENSE.TXT')
  'libcxxabi.txt'=(Join-Path $sdk 'system/lib/libcxxabi/LICENSE.TXT')
  'compiler-rt.txt'=(Join-Path $sdk 'system/lib/compiler-rt/LICENSE.TXT')
  'stb.txt'=(Join-Path $root 'third_party/stb/LICENSE.txt')
  'xbr.txt'=(Join-Path $root 'third_party/xbr/LICENSE.txt')
}
foreach($source in $licenses.Values) { if (!(Test-Path -LiteralPath $source)) { throw "Missing dependency license: $source" } }
New-Item -ItemType Directory -Path (Join-Path $destination 'LICENSES') -Force | Out-Null
foreach($name in $files) { Copy-Item -LiteralPath (Join-Path $site $name) -Destination (Join-Path $destination $name) }
foreach($name in @('LICENSE','THIRD_PARTY.md')) { Copy-Item -LiteralPath (Join-Path $root $name) -Destination $destination }
foreach($name in $licenses.Keys) { Copy-Item -LiteralPath $licenses[$name] -Destination (Join-Path $destination "LICENSES/$name") }
Copy-Item -LiteralPath (Join-Path $root 'scripts/serve-web.py') -Destination $destination
@'
GT2 0.8.0 browser

Run: python serve-web.py --directory . --port 8080
Open: http://127.0.0.1:8080/
Or upload this directory to your own static HTTPS hosting.
Do not open index.html directly through file://.

Select your full PS1 BIN (2352 bytes/sector), choose its Simulation/Arcade save
slot and press Start. The disc remains local to your browser and is not uploaded.
Desktop Chrome was validated. WebGL2, WebAssembly and IndexedDB are required.
GPU: maximum texture size at least 8192; at least 512 array texture layers.

Arrows: menus / steering, throttle and brake-to-reverse (automatic transmission).
Down brakes to a stop before reversing. Up brakes reverse motion before driving forward.
Enter: choose. Space: back / handbrake. Resolution scale changes in 5% steps.
C: camera. Esc: pause. Shift+Q: settings. Browser gamepads use the same C++ pad mapping.
Save and export before closing the page. Import/export buttons transfer JSON save
backups; the embedded cards are the same C++ .mcd format. Site storage depends on
the exact URL origin (scheme, host and port). Keep a backup before clearing it.

Requirements and limits: one selected disc per page session, full BIN kept in RAM,
flat screen only, no WebXR. No native wheel force feedback.
Mobile browsers and physical gamepads are not yet validated. GPU context loss
requires a reload. This archive contains no game data, BIOS or player saves.

Implementation: shared C++ game and material shaders, Emscripten 6.0.10, SDL 2.32.10.
Compiler/toolchain files are not part of this archive.
'@ | Set-Content -LiteralPath (Join-Path $destination 'README.txt') -Encoding UTF8
$manifest=@(Get-ChildItem -LiteralPath $destination -Recurse -File | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($destination.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant();bytes=$_.Length}
})
[ordered]@{version='0.8.0';emscripten='6.0.10';sdl='2.32.10';files=$manifest} | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $destination 'BUILD-MANIFEST.json') -Encoding UTF8
Compress-Archive -Path (Join-Path $destination '*') -DestinationPath ($destination+'.zip') -CompressionLevel Optimal
Get-FileHash -LiteralPath ($destination+'.zip') -Algorithm SHA256
