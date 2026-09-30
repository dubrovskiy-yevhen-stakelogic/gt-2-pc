param([string]$Base = 'dist/GT2-0.8.0', [string]$Output = 'dist/GT2-0.8.0-installer-r2')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot
function Full([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repo $Path))
}
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Save-Json($Value, [string]$Path) { $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8 }
$Base = Full $Base
$Output = Full $Output
if ((Test-Path -LiteralPath $Output) -or (Test-Path -LiteralPath ($Output + '.zip'))) { throw 'Choose a new output; existing releases are preserved.' }
$package = Get-Content -LiteralPath (Join-Path $Base 'PACKAGE-MANIFEST.json') -Raw | ConvertFrom-Json
if ($package.version -ne '0.8.0') { throw 'This installer update requires the 0.8.0 package.' }
# Verify and copy only the original release inventory, never local runtime/cache files.
$seen = @{}
foreach ($entry in $package.files) {
    if ($entry.path -match '(^|[\\/])\.\.([\\/]|$)|^[\\/]|:' -or $seen.ContainsKey($entry.path)) { throw 'Invalid base package inventory.' }
    $seen[$entry.path] = $true
    if ((Hash (Join-Path $Base $entry.path)) -ne $entry.sha256) { throw "Base package changed: $($entry.path)" }
}
foreach ($entry in $package.files) {
    $target = Join-Path $Output $entry.path
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $Base $entry.path) -Destination $target
}
$updates = @('scripts/prepare-hd.ps1', 'docs/HD-MEDIA.md', 'tests/test-hd-download.ps1', 'scripts/package-installer-update.ps1')
foreach ($relative in $updates) {
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination (Join-Path $Output $relative)
}
foreach ($relative in @('scripts/prepare-hd.ps1', 'docs/HD-MEDIA.md')) {
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination (Join-Path $Output ('native/' + $relative))
}
# Refresh the source snapshot from actual bytes, retaining the original provenance.
$source = Get-Content -LiteralPath (Join-Path $Output 'SOURCE-MANIFEST.json') -Raw | ConvertFrom-Json
$source.files = @((@($source.files.path) + $updates) | Sort-Object -Unique | ForEach-Object {
    $relative = $_
    $path = Join-Path $Output $relative
    $entry = [ordered]@{path=$relative;sha256=(Hash $path)}
    if ([IO.Path]::GetExtension($relative) -notin @('.png', '.uxrh')) {
        $utf8 = [Text.UTF8Encoding]::new($false, $true)
        $text = $utf8.GetString([IO.File]::ReadAllBytes($path)).Replace("`r`n", "`n")
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $entry.textSha256 = ([BitConverter]::ToString($sha.ComputeHash($utf8.GetBytes($text)))).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
    }
    $entry
})
Save-Json $source (Join-Path $Output 'SOURCE-MANIFEST.json')
$nativePath = Join-Path $Output 'native'
$native = Get-Content -LiteralPath (Join-Path $nativePath 'release-manifest.json') -Raw | ConvertFrom-Json
# The binaries are unchanged. Keep their original sourceManifestSha256; record
# the installer snapshot separately rather than imply that they were rebuilt.
$native | Add-Member -Force NoteProperty installerRevision 2
$native | Add-Member -Force NoteProperty installerSourceManifestSha256 (Hash (Join-Path $Output 'SOURCE-MANIFEST.json'))
foreach ($entry in $native.files) {
    $path = Join-Path $nativePath $entry.path
    $entry.sha256 = Hash $path
    $entry.bytes = (Get-Item -LiteralPath $path).Length
}
Save-Json $native (Join-Path $nativePath 'release-manifest.json')
& (Join-Path $nativePath 'scripts/install-player.ps1') -VerifyOnly
$package | Add-Member -Force NoteProperty installerRevision 2
$package | Add-Member -Force NoteProperty basePackageManifestSha256 (Hash (Join-Path $Base 'PACKAGE-MANIFEST.json'))
$package.files = @(Get-ChildItem -LiteralPath $Output -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($Output.Length + 1).Replace('\','/');bytes=$_.Length;sha256=(Hash $_.FullName)}
})
Save-Json $package (Join-Path $Output 'PACKAGE-MANIFEST.json')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$prefix = [IO.Path]::GetFileName($Output) + '/'
$zip = [IO.Compression.ZipFile]::Open($Output + '.zip', [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($relative in @($package.files.path) + 'PACKAGE-MANIFEST.json') {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $Output $relative), $prefix + $relative, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }
$zip = [IO.Compression.ZipFile]::OpenRead($Output + '.zip')
try {
    if ($zip.Entries.Count -ne $package.files.Count + 1) { throw 'Archive inventory mismatch.' }
    foreach ($entry in $zip.Entries) {
        if (!$entry.FullName.StartsWith($prefix)) { throw 'Unexpected archive root.' }
        $stream = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose(); $sha.Dispose() }
        if ($actual -ne (Hash (Join-Path $Output $entry.FullName.Substring($prefix.Length)))) { throw "Archive file mismatch: $($entry.FullName)" }
    }
} finally { $zip.Dispose() }
$hash = Hash ($Output + '.zip')
[IO.File]::WriteAllText($Output + '.zip.sha256', $hash + '  ' + [IO.Path]::GetFileName($Output) + '.zip' + [Environment]::NewLine)
Write-Host "Verified installer update: $Output.zip ($hash). Original game binaries retained."
