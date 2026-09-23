param([string]$Output = (Join-Path (Split-Path $PSScriptRoot) 'dist/GT2-source-0.5.0'), [switch]$AllowUncommitted, [switch]$FileSystem)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
& (Join-Path $PSScriptRoot 'audit-source.ps1') -Repo $repo -FileSystem:$FileSystem
$Output = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { throw 'The source output folder already exists. Choose a new folder.' }
Push-Location $repo
try {
    $revision = $null; $sourceDirty = $null
    if (!$FileSystem) {
        & git diff --quiet HEAD --
        $sourceDirty = $LASTEXITCODE -ne 0 -or @(& git ls-files --others --exclude-standard src tools tests cmake scripts docs third_party).Count -gt 0
        if ($sourceDirty -and !$AllowUncommitted) { throw 'Source has uncommitted changes. Use -AllowUncommitted to export the working files.' }
        $revision = (& git rev-parse HEAD).Trim()
    }
    $files = @(& (Join-Path $PSScriptRoot 'source-files.ps1') -Repo $repo -FileSystem:$FileSystem)
    if ((!$FileSystem -and $LASTEXITCODE -ne 0) -or !$files.Count) { throw 'Cannot list source files.' }
    New-Item -ItemType Directory -Force -Path $Output | Out-Null
    $manifest = @()
    foreach ($file in $files) {
        $destination = Join-Path $Output $file
        New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
        Copy-Item -LiteralPath $file -Destination $destination
        $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash) { throw "Source copy mismatch: $file" }
        $manifest += [ordered]@{path=$file;sha256=$hash}
    }
    $metadata = [ordered]@{version='0.5.0';sourceCommit=$revision;sourceDirty=$sourceDirty;files=$manifest}
    if ($FileSystem) { $metadata.sourceProvenance = 'filesystem-sha256' }
    $metadata | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $Output 'SOURCE-MANIFEST.json') -Encoding UTF8
    if ($FileSystem) { Write-Host "Source folder ready: $Output ($($files.Count) files, filesystem SHA256 snapshot)" }
    else { Write-Host "Source folder ready: $Output ($($files.Count) files, revision $revision)" }
} finally { Pop-Location }
