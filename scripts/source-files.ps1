[CmdletBinding()]
param([string]$Repo = (Split-Path $PSScriptRoot), [switch]$FileSystem)
$ErrorActionPreference = 'Stop'
if ($true) {
    $root = (Get-Item -LiteralPath $Repo).FullName.TrimEnd('\', '/')
    $rootFiles = @('.gitattributes', '.gitignore', 'build.cmd', 'BUILD-STEAMDECK.sh', 'INSTALL-STEAMDECK.sh', 'PLAY-STEAMDECK.sh', 'IMPORT-DISC-STEAMDECK.sh', 'PREPARE-HD.command', 'BUILD-MACOS.command', 'PLAY-MACOS.command', 'INSTALL-MACOS.command', 'CHANGELOG.md', 'CMakeLists.txt',
        'gt2_arcade.bat', 'gt2_race.bat', 'gt2_sim.bat', 'INSTALL-LINUX.sh', 'INSTALL.bat', 'LICENSE',
        'PREPARE-HD.bat', 'README.md', 'run_race.bat', 'THIRD_PARTY.md',
        'TRANSFER_PC_SAVES_TO_QUEST.bat', 'TRANSFER_QUEST_SAVES_TO_PC.bat')
    # Public documentation is explicit: local validation reports can contain device
    # identifiers and private diagnostic paths. New research needs a release review.
    $publicDocs = @('COCKPIT.md', 'EUROPEAN-DISCS.md', 'HD-MEDIA.md',
        'LINUX-INSTALL.md', 'STEAMDECK.md', 'MACOS.md', 'MIPMAPS.md', 'PCVR.md', 'PLAYER-INSTALL.md', 'QUEST.md',
        'SAVE-TRANSFER.md', 'VALIDATION.md', 'wheel-profile-provenance.json',
        'WEB.md', 'WHEEL-PROFILES.md', 'WHEELS.md')
    # The root allowlist excludes runtime game data. tools/xrsim/runtime is
    # source code and must remain part of the exported simulator.
    $skipDirectory = '^(bin|obj|build[^/]*|cmake-build-.*|out|\.cxx|\.gradle|work|dist|saves|\.git|\.codex|\.agents|\.claude|\.vs|\.vscode|\.idea|__pycache__|game-decomp|recomp_out)$'
    $skipFile = '^(AGENTS|CLAUDE|HANDOFF)\.md$|^local\.properties$|^credentials?\.(xml|json)$|^\.env($|\.)|\.(keystore|jks|p12|pfx|pem|key|gt2host|gt2invite|gt2resume)$'
    $files = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in $rootFiles) {
        if (!(Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) { throw "Missing public source file: $name" }
        $files.Add($name)
    }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    foreach ($name in @('src', 'tools', 'tests', 'cmake', 'scripts', 'docs', 'third_party', 'android', 'db', 're')) {
        $path = Join-Path $root $name
        if (!(Test-Path -LiteralPath $path -PathType Container)) { throw "Missing source directory: $name" }
        $pending.Push($path)
    }
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($item.PSIsContainer -and $item.Name -match $skipDirectory) { continue }
            if (!$item.PSIsContainer -and $item.Name -match $skipFile) { continue }
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source links need explicit review: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName); continue }
            $relative = $item.FullName.Substring($root.Length + 1).Replace('\', '/')
            # Historical reverse-engineering journals remain local. The release
            # contains buildable source and English product documentation.
            if ($relative.StartsWith('docs/research/') -or $relative.StartsWith('docs/formats/')) { continue }
            if ($relative.StartsWith('docs/') -and !$relative.StartsWith('docs/formats/')) {
                $doc = $relative.Substring(5)
                # Unknown document text stays private. Other file types remain in
                # the inventory so the audit catches misplaced binary payloads.
                if ($doc -notin $publicDocs -and $item.Extension -match '^\.(md|txt|json|yaml|yml|csv)$') { continue }
            }
            $files.Add($relative)
        }
    }
    $files | Sort-Object -Unique
    return
}