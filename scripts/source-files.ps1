[CmdletBinding()]
param([string]$Repo = (Split-Path $PSScriptRoot))
Push-Location $Repo
try {
    $tracked = @(& git -c core.quotepath=false ls-files)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list tracked source files.' }
    $new = @(& git -c core.quotepath=false ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list new source files.' }
    $approved = @($new | Where-Object {
        $_ -eq 'TRANSFER_QUEST_SAVES_TO_PC.bat' -or $_ -eq 'TRANSFER_PC_SAVES_TO_QUEST.bat' -or $_ -eq 'PREPARE-HD.bat' -or $_ -eq 'INSTALL-LINUX.sh' -or $_ -eq 'LICENSE' -or $_ -eq 'CHANGELOG.md' -or
        ($_ -match '^(src|tools|tests|cmake|scripts|docs|third_party|android/app/src/main/java)/' -and
        $_ -match '\.(java|cpp|h|hpp|c|glsl|vert|frag|cmake|ps1|py|sh|md|txt|json)$')
    })
    @($tracked + $approved | Sort-Object -Unique)
} finally { Pop-Location }
