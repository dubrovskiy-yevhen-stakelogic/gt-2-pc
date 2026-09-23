param([string]$Repo = (Split-Path $PSScriptRoot), [switch]$FileSystem)
$ErrorActionPreference = 'Stop'
$Repo = (Get-Item -LiteralPath $Repo).FullName
Push-Location $Repo
try {
    $files = @(& (Join-Path $PSScriptRoot 'source-files.ps1') -Repo $repo -FileSystem:$FileSystem)
    if ((!$FileSystem -and $LASTEXITCODE -ne 0) -or !$files.Count) { throw 'No source files found for auditing.' }
    if ($files -notcontains 'LICENSE') { throw 'Missing project MIT license in the source snapshot.' }
    $forbidden = '(?i)^(work|runtime|build[^/]*|dist|saves)/|^android/(\.gradle/|build/|app/(build|\.cxx)/|local\.properties$)|(^|/)(bin|obj|game-decomp)/|\.(bin|cue|iso|raw2352|chd|gtr|vol|ovl|tro|trp|cdo|cdp|mcd|mcr|sav|png|jpg|exe|dll|lib|pdb|spv|apk|aar|so|keystore|jks|prims\.txt)$|^install-location\.txt$'
    $forbidden += '|(^|/)(AGENTS|CLAUDE|HANDOFF)\.md$|(^|/)\.(codex|agents|claude)/|credential\.xml$'
    # This redistributable MIT texture is a source asset, not extracted game data.
    $allowedImage = 'third_party/vrhands/BigHandsAlbedo.png'
    $readmeImage = 'docs/images/cockpit-0.5.0.png'
    if ($files -notcontains $readmeImage) { throw 'Missing README screenshot.' }
    if ((Get-FileHash -LiteralPath $readmeImage -Algorithm SHA256).Hash -ne 'E10CB0F35258951842EDF0F113C382C1AFF0D88B8E7FBD88C8562810949284D9') { throw 'Unexpected README screenshot; review the replacement before packaging.' }
    if ($files -notcontains $allowedImage) { throw 'Missing redistributable hand texture in the source snapshot.' }
    $bad = @($files | Where-Object { $_ -match $forbidden -and $_ -notin @($allowedImage, $readmeImage) })
    if ($bad.Count) { throw ('Non-source files in the snapshot: ' + ($bad -join ', ')) }
    if ($files -contains $allowedImage) {
        if ((Get-FileHash -LiteralPath $allowedImage -Algorithm SHA256).Hash -ne '44D2DF3FB67FA65AAA448F2A8847809D0A24E8400445D2720759FCA4232A3D9A') { throw 'Unexpected VR hand texture hash; verify its provenance.' }
        if (!(Test-Path third_party/vrhands/ULTIMATEXR_LICENSE.txt)) { throw 'Missing VR hand license.' }
    }
    foreach ($path in $files) {
        if ((Get-Item -LiteralPath $path).Length -gt 8MB) { throw "Unexpectedly large source file: $path" }
        if ($FileSystem) {
            $asset = $path -in @($allowedImage, $readmeImage, 'third_party/vrhands/BigHandLeft.uxrh', 'third_party/vrhands/BigHandRight.uxrh')
            $extension = [IO.Path]::GetExtension($path)
            $text = $extension -match '^\.(cpp|h|hpp|c|cs|csproj|sln|glsl|vert|frag|cmake|ps1|py|sh|md|txt|json|yaml|yml|java|xml|properties|gradle|inc|cmd|bat)$' -or
                [IO.Path]::GetFileName($path) -in @('LICENSE', '.gitignore', '.gitattributes')
            if (!$asset -and !$text) { throw "Unexpected source file type: $path" }
            if (!$asset -and [Array]::IndexOf([IO.File]::ReadAllBytes((Join-Path $Repo $path)), [byte]0) -ge 0) {
                throw "Binary data in source text file: $path"
            }
        }
    }
    Write-Host "Source audit passed: $($files.Count) source files; no game payloads or build artifacts by path/type/size checks."
} finally { Pop-Location }
