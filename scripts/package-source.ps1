param([string]$Output, [switch]$AllowUncommitted, [switch]$FileSystem)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(gt2pc VERSION ([0-9]+\.[0-9]+\.[0-9]+) ') { throw 'Cannot determine source version.' }
$sourceVersion = $Matches[1]
if (!$Output) { $Output = Join-Path $repo "dist/GT2-source-$sourceVersion" }
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
        $entry = [ordered]@{path=$file;sha256=$hash}
        if ([IO.Path]::GetExtension($file) -notin @('.png', '.uxrh')) {
            $bytes = [IO.File]::ReadAllBytes($destination)
            $utf8 = [Text.UTF8Encoding]::new($false, $true)
            $text = $utf8.GetString($bytes).Replace("`r`n", "`n")
            $hasher = [Security.Cryptography.SHA256]::Create()
            try { $entry.textSha256 = ([BitConverter]::ToString($hasher.ComputeHash($utf8.GetBytes($text)))).Replace('-', '').ToLowerInvariant() }
            finally { $hasher.Dispose() }
        }
        $manifest += $entry
    }
    $metadata = [ordered]@{version=$sourceVersion;sourceCommit=$revision;sourceDirty=$sourceDirty;files=$manifest}
    if ($FileSystem) { $metadata.sourceProvenance = 'filesystem-sha256' }
    $metadata | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $Output 'SOURCE-MANIFEST.json') -Encoding UTF8
    if ($FileSystem) { Write-Host "Source folder ready: $Output ($($files.Count) files, filesystem SHA256 snapshot)" }
    else { Write-Host "Source folder ready: $Output ($($files.Count) files, revision $revision)" }
} finally { Pop-Location }
