param([string]$Repo = (Split-Path $PSScriptRoot))
$ErrorActionPreference = 'Stop'
Push-Location $Repo
try {
    $files = @(& git -c core.quotepath=false ls-files)
    if ($LASTEXITCODE -ne 0 -or !$files.Count) { throw 'Stage or commit the source files in Git before auditing.' }
    $forbidden = '(?i)^(work|runtime|build[^/]*|dist|saves)/|^android/(\.gradle/|build/|app/(build|\.cxx)/|local\.properties$)|(^|/)(bin|obj|game-decomp)/|\.(bin|cue|iso|raw2352|chd|gtr|vol|ovl|tro|trp|cdo|cdp|mcd|mcr|sav|png|jpg|exe|dll|lib|pdb|spv|apk|aar|so|keystore|jks|prims\.txt)$|^install-location\.txt$'
    $forbidden += '|(^|/)(AGENTS|CLAUDE|HANDOFF)\.md$|(^|/)\.(codex|agents|claude)/|credential\.xml$'
    # This redistributable MIT texture is a source asset, not extracted game data.
    $allowedImage = 'third_party/vrhands/BigHandsAlbedo.png'
    if ($files -notcontains $allowedImage) { throw 'Missing redistributable hand texture in the source snapshot.' }
    $bad = @($files | Where-Object { $_ -match $forbidden -and $_ -ne $allowedImage })
    if ($bad.Count) { throw ('Non-source files are tracked: ' + ($bad -join ', ')) }
    if ($files -contains $allowedImage) {
        if ((Get-FileHash -LiteralPath $allowedImage -Algorithm SHA256).Hash -ne '44D2DF3FB67FA65AAA448F2A8847809D0A24E8400445D2720759FCA4232A3D9A') { throw 'Unexpected VR hand texture hash; verify its provenance.' }
        if (!(Test-Path third_party/vrhands/ULTIMATEXR_LICENSE.txt)) { throw 'Missing VR hand license.' }
    }
    foreach ($path in $files) {
        if ((Get-Item -LiteralPath $path).Length -gt 8MB) { throw "Unexpectedly large source file: $path" }
    }
    Write-Host "Source audit passed: $($files.Count) tracked files; no game payloads or build artifacts by path/type/size checks."
} finally { Pop-Location }
