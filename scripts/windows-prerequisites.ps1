$script:Gt2PrerequisiteManifest = Join-Path $PSScriptRoot 'windows-prerequisites.json'

function Test-Gt2Python([string]$Executable) {
    if (!$Executable -or $Executable -match '[\\/]WindowsApps[\\/]') { return $false }
    try {
        $probe = & $Executable -E -s -c "import sys,struct; assert sys.version_info >= (3,11) and struct.calcsize('P')==8; import numpy; from PIL import Image; print('GT2_PYTHON_READY')" 2>$null
        return $LASTEXITCODE -eq 0 -and $probe -contains 'GT2_PYTHON_READY'
    } catch { return $false }
}

function Get-Gt2ToolCache([string]$ToolsRoot) {
    if (!$ToolsRoot) { $ToolsRoot = Join-Path $env:LOCALAPPDATA 'GT2-VR/Tools' }
    $ToolsRoot = [IO.Path]::GetFullPath($ToolsRoot)
    New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null
    return $ToolsRoot
}

function Get-Gt2DependencyArchive($Dependency, [string]$Name, [string]$Cache, [switch]$Offline) {
    $downloads = Join-Path $Cache 'downloads'
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $archive = Join-Path $downloads ($Name + '-' + $Dependency.version + '.zip')
    if (Test-Path -LiteralPath $archive) {
        if ((Get-FileHash -LiteralPath $archive -Algorithm $Dependency.algorithm).Hash -eq $Dependency.hash) { return $archive }
        if ($Offline) { throw "Cached $Name archive failed verification. Run once without -Offline to download it again." }
        Move-Item -LiteralPath $archive -Destination ($archive + '.bad-' + [guid]::NewGuid().ToString('N'))
    }
    if ($Offline) { throw "Missing cached $Name dependency. Run once with an internet connection and without -Offline." }
    Write-Host "Downloading $Name $($Dependency.version). No administrator access is needed."
    $temporary = $archive + '.' + [guid]::NewGuid().ToString('N') + '.download'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $previousProgress = $ProgressPreference
    try {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Dependency.url -OutFile $temporary -UseBasicParsing
    } catch { throw "Cannot download $Name from $($Dependency.url). Check the internet connection and retry. $($_.Exception.Message)" }
    finally { $ProgressPreference = $previousProgress }
    if ((Get-FileHash -LiteralPath $temporary -Algorithm $Dependency.algorithm).Hash -ne $Dependency.hash) {
        throw "$Name download failed verification; it was not extracted or executed. Retry the download."
    }
    Move-Item -LiteralPath $temporary -Destination $archive
    return $archive
}

function Expand-Gt2Dependency([string]$Archive, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $root = [IO.Path]::GetFullPath($Destination).TrimEnd('\') + '\'
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $zip.Entries) {
            $path = [IO.Path]::GetFullPath((Join-Path $root $entry.FullName))
            if (!$path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe dependency archive path.' }
        }
    } finally { $zip.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination)
}

function Get-Gt2Python([string]$Python, [string]$ToolsRoot, [switch]$Offline) {
    $candidates = @()
    if ($Python) {
        $found = Get-Command $Python -CommandType Application -ErrorAction SilentlyContinue
        if ($found) { $candidates += $found.Source }
    } else {
        foreach ($name in @('python.exe','python3.exe')) {
            $found = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
            if ($found) { $candidates += $found.Source }
        }
    }
    foreach ($candidate in $candidates) { if (Test-Gt2Python $candidate) { return $candidate } }
    $cache = Get-Gt2ToolCache $ToolsRoot
    $manifest = Get-Content -LiteralPath $script:Gt2PrerequisiteManifest -Raw | ConvertFrom-Json
    $name = "python-$($manifest.python.version)-numpy-$($manifest.numpy.version)-pillow-$($manifest.pillow.version)"
    $destination = Join-Path $cache $name
    $executable = Join-Path $destination 'tools/python.exe'
    if (Test-Gt2Python $executable) { return $executable }
    $pythonZip = Get-Gt2DependencyArchive $manifest.python 'Python' $cache -Offline:$Offline
    $numpyZip = Get-Gt2DependencyArchive $manifest.numpy 'NumPy' $cache -Offline:$Offline
    $pillowZip = Get-Gt2DependencyArchive $manifest.pillow 'Pillow' $cache -Offline:$Offline
    $stage = $destination + '.preparing-' + [guid]::NewGuid().ToString('N')
    Write-Host 'Preparing the private Python runtime, Pillow and NumPy...'
    Expand-Gt2Dependency $pythonZip $stage
    $packages = Join-Path $stage 'tools/Lib/site-packages'
    Expand-Gt2Dependency $numpyZip $packages
    Expand-Gt2Dependency $pillowZip $packages
    if (!(Test-Gt2Python (Join-Path $stage 'tools/python.exe'))) { throw "Python dependency check failed. Retained files: $stage" }
    if (Test-Path -LiteralPath $destination) {
        Move-Item -LiteralPath $destination -Destination ($destination + '.old-' + [guid]::NewGuid().ToString('N'))
    }
    Move-Item -LiteralPath $stage -Destination $destination
    return $executable
}

function Test-Gt2DotNet([string]$Executable) {
    if (!$Executable) { return $false }
    try {
        $versions = @(& $Executable --list-sdks 2>$null)
        return $LASTEXITCODE -eq 0 -and @($versions | Where-Object { $_ -match '^8\.0\.' }).Count -gt 0
    } catch { return $false }
}

function Get-Gt2DotNet([string]$ToolsRoot, [switch]$Offline) {
    $found = Get-Command dotnet.exe -CommandType Application -ErrorAction SilentlyContinue
    if ($found -and (Test-Gt2DotNet $found.Source)) { return $found.Source }
    $cache = Get-Gt2ToolCache $ToolsRoot
    $manifest = Get-Content -LiteralPath $script:Gt2PrerequisiteManifest -Raw | ConvertFrom-Json
    $destination = Join-Path $cache ('dotnet-' + $manifest.dotnet.version)
    $executable = Join-Path $destination 'dotnet.exe'
    if (Test-Gt2DotNet $executable) { return $executable }
    $archive = Get-Gt2DependencyArchive $manifest.dotnet '.NET-SDK' $cache -Offline:$Offline
    $stage = $destination + '.preparing-' + [guid]::NewGuid().ToString('N')
    Write-Host 'Preparing the private .NET SDK...'
    Expand-Gt2Dependency $archive $stage
    if (!(Test-Gt2DotNet (Join-Path $stage 'dotnet.exe'))) { throw "NET SDK check failed. Retained files: $stage" }
    if (Test-Path -LiteralPath $destination) {
        Move-Item -LiteralPath $destination -Destination ($destination + '.old-' + [guid]::NewGuid().ToString('N'))
    }
    Move-Item -LiteralPath $stage -Destination $destination
    return $executable
}
