[CmdletBinding()]
param([string]$Repo = (Split-Path $PSScriptRoot), [switch]$FileSystem)
$ErrorActionPreference = 'Stop'
if ($FileSystem) {
    $root = (Get-Item -LiteralPath $Repo).FullName.TrimEnd('\', '/')
    $rootFiles = @('.gitattributes', '.gitignore', 'build.cmd', 'CHANGELOG.md', 'CMakeLists.txt',
        'gt2_arcade.bat', 'gt2_race.bat', 'gt2_sim.bat', 'INSTALL-LINUX.sh', 'INSTALL.bat', 'LICENSE',
        'PREPARE-HD.bat', 'README.md', 'run_race.bat', 'THIRD_PARTY.md',
        'TRANSFER_PC_SAVES_TO_QUEST.bat', 'TRANSFER_QUEST_SAVES_TO_PC.bat')
    # Public documentation is explicit: local validation reports can contain device
    # identifiers and private diagnostic paths. New research needs a release review.
    $publicDocs = @('COCKPIT.md', 'EUROPEAN-DISCS.md', 'hardware_boundary.md', 'HD-MEDIA.md',
        'LINUX-INSTALL.md', 'MIPMAPS.md', 'PCVR.md', 'PLAYER-INSTALL.md', 'QUEST.md',
        'RENDER_BENCHMARK.md', 'SAVE-TRANSFER.md', 'VALIDATION.md', 'wheel-profile-provenance.json',
        'WHEEL-PROFILES.md', 'WHEELS.md', 'research/arcade_disc.md', 'research/audio_video.md',
        'research/engine_tech.md', 'research/formats.md', 'research/menus_gtmode.md',
        'research/quest_ports.md', 'research/re_state.md', 'research/scout_disc.md',
        'research/scout_exe.md', 'research/scout_vol.md', 'research/vr_modern.md',
        'research/vr_port_plan.md', 'research/xrsim.md')
    # The root allowlist excludes runtime game data. tools/xrsim/runtime is
    # source code and must remain part of the exported simulator.
    $skipDirectory = '^(bin|obj|build[^/]*|cmake-build-.*|out|\.cxx|\.gradle|work|dist|saves|\.git|\.codex|\.agents|\.claude|\.vs|\.vscode|\.idea|__pycache__|game-decomp|recomp_out)$'
    $skipFile = '^(AGENTS|CLAUDE|HANDOFF)\.md$|^local\.properties$|^credentials?\.(xml|json)$|^\.env($|\.)|\.(keystore|jks|p12|pfx|pem|key)$'
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
Push-Location $Repo
try {
    $tracked = @(& git -c core.quotepath=false ls-files)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list tracked source files.' }
    $new = @(& git -c core.quotepath=false ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list new source files.' }
    $approved = @($new | Where-Object {
        $_ -eq 'docs/images/cockpit-0.5.0.png' -or
        $_ -eq 'TRANSFER_QUEST_SAVES_TO_PC.bat' -or $_ -eq 'TRANSFER_PC_SAVES_TO_QUEST.bat' -or $_ -eq 'PREPARE-HD.bat' -or $_ -eq 'INSTALL-LINUX.sh' -or $_ -eq 'LICENSE' -or $_ -eq 'CHANGELOG.md' -or
        ($_ -match '^(src|tools|tests|cmake|scripts|docs|third_party|android/app/src/main/java)/' -and
        $_ -match '\.(java|cpp|h|hpp|c|glsl|vert|frag|cmake|ps1|py|sh|md|txt|json)$')
    })
    @($tracked + $approved | Sort-Object -Unique)
} finally { Pop-Location }
