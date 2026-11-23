param([string]$Repo = (Split-Path $PSScriptRoot), [switch]$FileSystem)
$ErrorActionPreference = 'Stop'
$Repo = (Get-Item -LiteralPath $Repo).FullName
Push-Location $Repo
try {
    $files = @(& (Join-Path $PSScriptRoot 'source-files.ps1') -Repo $repo -FileSystem:$FileSystem)
    if (!$files.Count) { throw 'No source files found for auditing.' }
    # Multiplayer is part of the source kit. Reject an incomplete export instead
    # of silently publishing a client without its protocol, dependencies or server.
    $networkRequired = @('cmake/network.cmake', 'src/platform/net/lobby.h',
        'src/platform/net/lobby.cpp', 'src/platform/net/lobby_rules.h',
        'src/platform/net/lobby_endpoint.h', 'src/platform/net/lan_room.cpp',
        'src/game/net/room_protocol.cpp', 'tools/gt2game/online_race.cpp',
        'tools/gt2game/online_controls.h', 'tools/gt2lobby/main.cpp',
        'tools/gt2server/main.cpp', 'scripts/server/build-linux.sh',
        'scripts/server/gt2lobby.service', 'docs/MULTIPLAYER.md', 'docs/NETWORK_STATE.md',
        'tests/lobby_checks.cpp', 'tests/network_checks.cpp',
        'third_party/enet/include/enet/enet.h', 'third_party/libhydrogen/hydrogen.c')
    foreach ($required in $networkRequired) {
        if ($files -notcontains $required) { throw "Incomplete multiplayer source kit: $required" }
    }

    foreach ($required in @('src/gt2view/steering_wheel.h', 'src/gt2view/vr_driving_visuals.cpp',
        'cmake/vr-hand-assets.cmake', 'third_party/vrhands/BigGloveLeft.uxrh',
        'third_party/vrhands/BigGloveRight.uxrh', 'third_party/vrhands/BigGlovesTechnicalTex_A.png',
        'third_party/vrhands/SOURCE.md', 'third_party/vrhands/ULTIMATEXR_LICENSE.txt',
        'third_party/vrhands/MIAMIVR_LICENSE.txt', 'assets/cockpits/r32/nr32n/attribution.json')) {
        if ($files -notcontains $required) { throw "Incomplete wheel/glove source kit: $required" }
    }

    if ($files -notcontains 'LICENSE') { throw 'Missing project MIT license in the source snapshot.' }
    $forbidden = '(?i)^(work|runtime|build[^/]*|dist|saves)/|^android/(\.gradle/|build/|app/(build|\.cxx)/|local\.properties$)|(^|/)(bin|obj|game-decomp)/|\.(bin|cue|iso|raw2352|chd|gtr|vol|ovl|tro|trp|cdo|cdp|mcd|mcr|sav|png|jpg|exe|dll|lib|pdb|spv|apk|aar|so|keystore|jks|prims\.txt)$|^install-location\.txt$'
    $forbidden += '|(^|/)(AGENTS|CLAUDE|HANDOFF)\.md$|(^|/)\.(codex|agents|claude)/|credential\.xml$'
    # This redistributable MIT texture is a source asset, not extracted game data.
    $allowedImage = 'third_party/vrhands/BigHandsAlbedo.png'
    $gloveImage = 'third_party/vrhands/BigGlovesTechnicalTex_A.png'
    if ((Get-FileHash -LiteralPath $gloveImage -Algorithm SHA256).Hash -ne 'DA266F414EDE3F6DC53C9F80B8E31AAE0877417BDF41A61826FDD74C0557DF1D') { throw 'Unexpected glove texture; review provenance before packaging' }
    $readmeImage = 'docs/images/cockpit-0.5.0.png'
    $gt4Image = 'docs/images/gt4-cars-0.9.0.png'
    $modImage = 'docs/images/mod-datasets-picker.png'
    $featureImages = @('docs/images/multiplayer-1.0.0.png', 'docs/images/cockpit-1.0.0.png', 'docs/images/seattle-original-1.0.0.png', 'docs/images/seattle-hd-1.0.0.png')
    $ignoreRules = @(Get-Content -LiteralPath '.gitignore')
    foreach ($image in (@($readmeImage, $gt4Image, $modImage) + $featureImages)) {
        if ($ignoreRules -notcontains "!$image") { throw "README image needs an explicit .gitignore exception: $image" }
    }
    foreach ($image in $featureImages) { if ($files -notcontains $image) { throw "Missing feature screenshot: $image" } }
    if ($files -notcontains $modImage -or (Get-FileHash -LiteralPath $modImage -Algorithm SHA256).Hash -ne '2109F8396A4A25A64DA9BB34054A63D1E8DA1D8838DBC5450C57887A2426BF91') { throw 'Missing or changed mod dataset screenshot; review before packaging.' }
    if ($files -notcontains $gt4Image) { throw 'Missing GT4 README screenshot.' }
    if ((Get-FileHash -LiteralPath $gt4Image -Algorithm SHA256).Hash -ne '3538EC1140CCA2142D2EB8317E6B0E4678DF877CB8B87C95132699A53B37AB1C') { throw 'Unexpected GT4 README screenshot; review the replacement before packaging.' }
    if ($files -notcontains $readmeImage) { throw 'Missing README screenshot.' }
    if ((Get-FileHash -LiteralPath $readmeImage -Algorithm SHA256).Hash -ne 'E10CB0F35258951842EDF0F113C382C1AFF0D88B8E7FBD88C8562810949284D9') { throw 'Unexpected README screenshot; review the replacement before packaging.' }
    if ($files -notcontains $allowedImage) { throw 'Missing redistributable hand texture in the source snapshot.' }
    $forbidden += '|^docs/(research/|formats/)|^tools/worldpacks/|\.prompt\.txt$|^scripts/install-local-visuals\.py$'
    $bad = @($files | Where-Object { $_ -match $forbidden -and $_ -notin $featureImages -and $_ -notin @($allowedImage, $gloveImage, $readmeImage, $gt4Image, $modImage) })
    if ($bad.Count) { throw ('Non-source files in the snapshot: ' + ($bad -join ', ')) }
    if ($files -contains $allowedImage) {
        if ((Get-FileHash -LiteralPath $allowedImage -Algorithm SHA256).Hash -ne '44D2DF3FB67FA65AAA448F2A8847809D0A24E8400445D2720759FCA4232A3D9A') { throw 'Unexpected VR hand texture hash; verify its provenance.' }
        if (!(Test-Path third_party/vrhands/ULTIMATEXR_LICENSE.txt)) { throw 'Missing VR hand license.' }
    }
    & python -c "import sys; sys.path.insert(0, 'scripts'); from authored_cockpit_pack import assets; assets()"
    if ($LASTEXITCODE -ne 0) { throw 'Cockpit payload validation failed' }
    foreach ($path in $files) {
        if ((Get-Item -LiteralPath $path -Force).Length -gt 8MB) { throw "Unexpectedly large source file: $path" }
        & {
            $asset = $path -in $featureImages -or $path -match '^assets/cockpits/r32/[a-z0-9]+/r32\.glb$' -or $path -in @($allowedImage, $gloveImage, $readmeImage, $gt4Image, $modImage, 'third_party/vrhands/BigHandLeft.uxrh', 'third_party/vrhands/BigHandRight.uxrh', 'third_party/vrhands/BigGloveLeft.uxrh', 'third_party/vrhands/BigGloveRight.uxrh')
            $extension = [IO.Path]::GetExtension($path)
            $text = $extension -match '^\.(cpp|mm|h|hpp|c|cs|csproj|sln|glsl|vert|frag|cmake|ps1|py|sh|command|md|txt|json|yaml|yml|java|xml|properties|gradle|inc|cmd|bat|service|conf|html|js|cjs|desktop|svg|wxs|rtf)$' -or
                [IO.Path]::GetFileName($path) -in @('LICENSE', '.gitignore', '.gitattributes')
            if (!$asset -and !$text) { throw "Unexpected source file type: $path" }
            if (!$asset) {
                try { $null = [Text.UTF8Encoding]::new($false, $true).GetString([IO.File]::ReadAllBytes((Join-Path $Repo $path))) }
                catch { throw "Source text must be UTF-8: $path" }
            }

            if (!$asset -and [Array]::IndexOf([IO.File]::ReadAllBytes((Join-Path $Repo $path)), [byte]0) -ge 0) {
                throw "Binary data in source text file: $path"
            }
            if (!$asset -and [IO.File]::ReadAllText((Join-Path $Repo $path)) -match '[\u0400-\u04FF]') {
                throw "Non-English Cyrillic text in public source: $path"
            }
            if (!$asset -and [IO.File]::ReadAllText((Join-Path $Repo $path)) -match 'same[ ]sole[ ]author\b|same[ ]author.s\b|project[ ]author\b|their[ ]author\b') {
                throw "Personal attribution narrative in public source: $path"
            }

        }
    }
    Write-Host "Source audit passed: $($files.Count) source files; no game payloads or build artifacts by path/type/size checks."
} finally { Pop-Location }
