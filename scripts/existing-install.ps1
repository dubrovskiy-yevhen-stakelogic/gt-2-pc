# Shared by the launcher and its first-run folder picker.
function Test-Gt2ExistingData([string]$Path) {
    if (!$Path -or !(Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    foreach ($mode in @('arcade','simulation','combined')) {
        if ((Test-Path -LiteralPath (Join-Path $Path "$mode/disc.raw2352") -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $Path "$mode/assets") -PathType Container)) { return $true }
    }
    return $false
}

function Find-Gt2ExistingData([string]$PackageRoot) {
    $hint = Join-Path $PackageRoot 'install-location.txt'
    if (Test-Path -LiteralPath $hint) {
        $saved = (Get-Content -LiteralPath $hint -Raw -Encoding UTF8).Trim()
        # An explicit location must not silently switch to another installation.
        if ($saved) { return $saved }
    }
    foreach ($candidate in @((Join-Path $PackageRoot 'runtime'), (Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime'))) {
        if (Test-Gt2ExistingData $candidate) { return $candidate }
    }
    return Join-Path $PackageRoot 'runtime'
}

function Update-Gt2ExistingData([string]$PackageRoot, [string]$Data) {
    if (!(Test-Gt2ExistingData $Data)) { throw 'This folder has no prepared GT2 discs.' }
    foreach ($name in @('gt2game.exe','gt2install.exe','openxr_loader.dll')) {
        $source = Join-Path $PackageRoot "tools/$name"
        $target = Join-Path $Data $name
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing package file: $name" }
        if ((Test-Path -LiteralPath $target) -and
            (Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $target).Hash) { continue }
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    [IO.File]::WriteAllText((Join-Path $PackageRoot 'install-location.txt'), [IO.Path]::GetFullPath($Data), [Text.Encoding]::UTF8)
}
