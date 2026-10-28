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
    if (!('Gt2DependencyZip' -as [type])) {
        $source = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class Gt2DependencyZip
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateDirectoryW(string path, IntPtr security);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFileAttributesW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share,
        IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool MoveFileExW(string existing, string destination, uint flags);

    private static string Extended(string path)
    {
        string full = Path.GetFullPath(path).Replace('/', '\\');
        if (full.StartsWith(@"\\?\", StringComparison.Ordinal)) return full;
        return full.StartsWith(@"\\", StringComparison.Ordinal)
            ? @"\\?\UNC\" + full.Substring(2) : @"\\?\" + full;
    }

    private static void DirectoryTree(string path, HashSet<string> directories)
    {
        if (directories.Contains(path)) return;
        uint attributes = GetFileAttributesW(path);
        if (attributes != 0xffffffff && (attributes & 16) != 0) {
            directories.Add(path);
            return;
        }
        int separator = path.LastIndexOf('\\');
        if (separator < 7) throw new IOException("Cannot create dependency directory: " + path);
        DirectoryTree(path.Substring(0, separator), directories);
        if (!CreateDirectoryW(path, IntPtr.Zero)) {
            int error = Marshal.GetLastWin32Error();
            attributes = GetFileAttributesW(path);
            if (error != 183 || attributes == 0xffffffff || (attributes & 16) == 0)
                throw new Win32Exception(error, "Cannot create dependency directory: " + path);
        }
        directories.Add(path);
    }

    private static string RelativeName(string name)
    {
        string value = name.Replace('/', '\\');
        if (value.StartsWith(@"\", StringComparison.Ordinal) || value.IndexOf(':') >= 0)
            throw new InvalidDataException("Unsafe dependency archive path: " + name);
        var parts = new List<string>();
        foreach (string part in value.Split('\\')) {
            if (part == "" || part == ".") continue;
            if (part == ".." || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                throw new InvalidDataException("Unsafe dependency archive path: " + name);
            parts.Add(part);
        }
        return String.Join(@"\", parts.ToArray());
    }

    public static void Extract(string archive, string destination)
    {
        string root = Extended(destination).TrimEnd('\\');
        using (SafeFileHandle handle = CreateFileW(Extended(archive), 0x80000000, 1,
            IntPtr.Zero, 3, 0x80, IntPtr.Zero)) {
            if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot read dependency archive");
            using (var input = new FileStream(handle, FileAccess.Read))
            using (var zip = new ZipArchive(input, ZipArchiveMode.Read)) {
                // Validate every name before writing any entry. Never pass ZIP paths
                // through legacy .NET normalization, which rejects paths over MAX_PATH.
                foreach (var entry in zip.Entries) RelativeName(entry.FullName);
                var directories = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                DirectoryTree(root, directories);
                foreach (var entry in zip.Entries) {
                    string relative = RelativeName(entry.FullName);
                    if (relative.Length == 0) continue;
                    string target = root + @"\" + relative;
                    if (entry.FullName.EndsWith("/") || entry.FullName.EndsWith(@"\")) {
                        DirectoryTree(target, directories);
                        continue;
                    }
                    DirectoryTree(target.Substring(0, target.LastIndexOf('\\')), directories);
                    using (SafeFileHandle output = CreateFileW(target, 0x40000000, 0,
                        IntPtr.Zero, 1, 0x80, IntPtr.Zero)) {
                        if (output.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot create dependency file: " + target);
                        using (var stream = new FileStream(output, FileAccess.Write))
                        using (var contents = entry.Open()) contents.CopyTo(stream);
                    }
                }
            }
        }
    }

    public static void MoveDirectory(string source, string destination)
    {
        // Both paths are on the same cache volume. Rename the directory itself;
        // PowerShell 5.1 Move-Item walks its contents and can reject long names.
        if (!MoveFileExW(Extended(source), Extended(destination), 0))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot move dependency directory: " + source);
    }
}
'@
        if ($PSVersionTable.PSEdition -eq 'Desktop') {
            Add-Type -TypeDefinition $source -ReferencedAssemblies 'System.dll','System.Core.dll','System.IO.Compression.dll'
        } else { Add-Type -TypeDefinition $source }
    }
    [Gt2DependencyZip]::Extract($Archive, $Destination)
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
        [Gt2DependencyZip]::MoveDirectory($destination, ($destination + '.old-' + [guid]::NewGuid().ToString('N')))
    }
    [Gt2DependencyZip]::MoveDirectory($stage, $destination)
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
        [Gt2DependencyZip]::MoveDirectory($destination, ($destination + '.old-' + [guid]::NewGuid().ToString('N')))
    }
    [Gt2DependencyZip]::MoveDirectory($stage, $destination)
    return $executable
}
