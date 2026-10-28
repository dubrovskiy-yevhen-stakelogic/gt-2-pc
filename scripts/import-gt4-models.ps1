param(
    [string]$Iso,
    [string]$Simulation,
    [string]$Arcade,
    [string]$Output,
    [string]$Build,
    [string]$Python,
    [string]$ModelTool,
    [string]$CatalogTool,
    [string]$PdtoolsRoot,
    [switch]$Offline,
    [string]$ToolsRoot,
    [switch]$PrepareToolsOnly,
    [ValidateSet('Quest','PC','BundleOnly')][string]$Target,
    [string]$Bundle,
    [string]$Runtime,
    [string]$Adb,
    [string]$Serial
)
$ErrorActionPreference = 'Stop'
$gt4Repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'windows-prerequisites.ps1')
function Select-Gt4Disc([string]$Title, [string]$Filter, [switch]$Optional) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = $Title
    $dialog.Filter = $Filter
    try {
        if ($dialog.ShowDialog() -eq 'OK') { return $dialog.FileName }
        if (!$Optional) { throw 'Disc selection cancelled. No game data was changed.' }
    } finally { $dialog.Dispose() }
}
function Invoke-Gt4Python([string[]]$Arguments) {
    # Route native output through the host so PowerShell 5.1 records it in the
    # transcript. Stderr text alone is not a failure; preserve the process code.
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 1
    & $Python -E -s @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    return $global:LASTEXITCODE
}
$result = 1
$transcribing = $false
$log = $null
try {
if (!$PrepareToolsOnly) {
    $logs = Join-Path $gt4Repo 'work/import-logs'
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs ('gt4-import-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8) + '.log')
    Start-Transcript -LiteralPath $log | Out-Null
    $transcribing = $true
    Write-Host "Import log: $log"
    if (!$Target) {
        Write-Host ''
        Write-Host 'Install GT4 cars: 1 = Quest (default), 2 = PC / PCVR, 3 = keep bundle for later.'
        $choice = Read-Host 'Choose destination [1]'
        switch ($choice) {
            '' { $Target = 'Quest' }
            '1' { $Target = 'Quest' }
            '2' { $Target = 'PC' }
            '3' { $Target = 'BundleOnly' }
            default { throw 'Unknown destination. Rerun the importer and choose 1, 2 or 3; the bundle is retained.' }
        }
    }
    if (!$Bundle -and !$Iso -and !$Output) {
        $lastBundle = Join-Path $gt4Repo 'work/last-gt4-bundle.txt'
        $prepared = $null
        if (Test-Path -LiteralPath $lastBundle) {
            $candidate = (Get-Content -LiteralPath $lastBundle -Raw).Trim()
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $prepared = $candidate }
        }
        if (!$prepared) {
            $folders = @(Get-ChildItem -LiteralPath (Join-Path $gt4Repo 'work') -Directory -Filter 'gt4-import-*' |
                Where-Object { !($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | Sort-Object LastWriteTime -Descending)
            foreach ($folder in $folders) {
                $candidate = Join-Path $folder.FullName 'gt4-models.tar'
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $prepared = $candidate; break }
            }
        }
        if ($prepared) {
            Write-Host "Prepared GT4 bundle found: $prepared"
            $reuse = Read-Host 'Install these cars without converting again? [Y/n]'
            if ($reuse -notmatch '^(n|no)$') { $Bundle = $prepared }
        }
    }
}
if (!$PrepareToolsOnly -and !$Bundle) {
    Write-Host 'GT4 import: first select the GT4 USA v1.01 ISO, then the GT2 USA Simulation v1.2 disc.'
    if (!$Iso) { $Iso = Select-Gt4Disc 'Select Gran Turismo 4 USA v1.01 (SCUS-97328) ISO' 'GT4 ISO (*.iso)|*.iso' }
    $runtimeCandidates = @((Join-Path $gt4Repo 'runtime'), (Join-Path $gt4Repo 'data'), (Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime'))
    foreach ($location in @('install-location.txt','native/install-location.txt')) {
        $path = Join-Path $gt4Repo $location
        if (Test-Path -LiteralPath $path) { $runtimeCandidates = @((Get-Content -LiteralPath $path -Raw).Trim()) + $runtimeCandidates }
    }
    if (!$Simulation) {
        foreach ($runtime in $runtimeCandidates) {
            if (Test-Path -LiteralPath (Join-Path $runtime 'simulation/disc.raw2352')) {
                $Simulation = Join-Path $runtime 'simulation'
                if (!$Arcade -and (Test-Path -LiteralPath (Join-Path $runtime 'arcade/disc.raw2352'))) { $Arcade = Join-Path $runtime 'arcade' }
                Write-Host "Using installed GT2 Simulation: $Simulation"
                break
            }
        }
        if (!$Simulation) { $Simulation = Select-Gt4Disc 'Select Gran Turismo 2 USA Simulation v1.2 BIN (not the GT4 ISO)' 'GT2 Simulation BIN (*.bin)|*.bin' }
        if (!$Arcade) { $Arcade = Select-Gt4Disc 'Optional: GT2 USA Arcade v1.1 BIN; Cancel to import Simulation only' 'GT2 Arcade BIN (*.bin)|*.bin' -Optional }
    }
    foreach ($disc in @($Iso, $Simulation) + @($Arcade | Where-Object { $_ })) {
        if (!(Test-Path -LiteralPath $disc)) { throw "Disc path does not exist: $disc" }
    }
    if ([IO.Path]::GetExtension($Simulation) -eq '.iso') { throw 'Simulation means the GT2 Simulation BIN or prepared simulation folder. The GT4 ISO belongs in -Iso.' }
    if (!$Build) {
        foreach ($folder in @('native/tools','build','build/Release','build_update','build_update/Release')) {
            $candidate = Join-Path $gt4Repo $folder
            $missing = @('gt2tool','gt4assettool','gt4nativemetadata','gt4materialmetadata','gt4specdbexport','gt4showroomexport' | Where-Object { !(Test-Path -LiteralPath (Join-Path $candidate ($_ + '.exe'))) })
            if (!$missing.Count) { $Build = $candidate; break }
        }
        if (!$Build) { throw 'Native import tools are missing. Extract the complete GT2 release, or build gt4-import-tools and gt2tool and pass -Build.' }
    }
}
$Python = Get-Gt2Python -Python $Python -ToolsRoot $ToolsRoot -Offline:$Offline
if (!$Bundle) {
    $dotnet = Get-Gt2DotNet -ToolsRoot $ToolsRoot -Offline:$Offline
    $env:DOTNET_ROOT = Split-Path $dotnet
    $env:DOTNET_ROOT_X64 = $env:DOTNET_ROOT
    $env:PATH = $env:DOTNET_ROOT + ';' + $env:PATH
    Write-Host "NET SDK ready: $dotnet"
}
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:PYTHONNOUSERSITE = '1'
Write-Host "Python ready: $Python"
if ($PrepareToolsOnly) { return }
if (!$Bundle) {
if (!$Output) { $Output = Join-Path $gt4Repo ('work\gt4-import-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (![IO.Path]::IsPathRooted($Output)) { $Output = Join-Path $gt4Repo $Output }
$Output = [IO.Path]::GetFullPath($Output)
$gt4Args = @((Join-Path $PSScriptRoot 'prepare-gt4-release.py'), '--iso', $Iso, '--simulation', $Simulation, '--output', $Output)
if ($Arcade) { $gt4Args += @('--arcade', $Arcade) }
if ($Build) { $gt4Args += @('--build', $Build) }
if ($ModelTool) { $gt4Args += @('--model-tool', $ModelTool) }
if ($CatalogTool) { $gt4Args += @('--catalog-tool', $CatalogTool) }
if ($PdtoolsRoot) { $gt4Args += @('--pdtools-root', $PdtoolsRoot) }
if ($Offline) { $gt4Args += '--offline' }
Push-Location $gt4Repo
try {
    $result = Invoke-Gt4Python $gt4Args
    if ($result -eq 0) {
        $bundle = Join-Path $Output 'gt4-models.tar'
        $bundleArgs = @((Join-Path $PSScriptRoot 'package-quest-gt4.py'), '--pack', (Join-Path $Output 'modelsets/gt4'),
            '--native', (Join-Path $Output 'modelsets/gt4/native-v16'), '--simulation-expansion', (Join-Path $Output 'expansions/simulation'), '--output', $bundle)
        if ($Arcade) { $bundleArgs += @('--arcade-expansion', (Join-Path $Output 'expansions/arcade')) }
        $result = Invoke-Gt4Python $bundleArgs
    }
} finally { Pop-Location }
} else {
    $Bundle = (Resolve-Path -LiteralPath $Bundle).Path
    $result = Invoke-Gt4Python @((Join-Path $PSScriptRoot 'package-quest-gt4.py'), '--verify', $Bundle)
}
if ($result -eq 0) {
    if (!(Test-Path -LiteralPath $Bundle -PathType Leaf)) { throw 'Packaging reported success but the GT4 bundle is missing.' }
    [IO.File]::WriteAllText((Join-Path $gt4Repo 'work/last-gt4-bundle.txt'), $Bundle, [Text.UTF8Encoding]::new($true))
    $resultFile = Join-Path (Split-Path $Bundle) 'INSTALL-GT4.txt'
    [IO.File]::WriteAllText($resultFile, "GT4 bundle ready. Installation has not completed yet.`r`nBundle: $Bundle`r`nLog: $log`r`nRerun IMPORT-GT4-MODELS.bat to install this bundle without converting again.", [Text.UTF8Encoding]::new($true))
    if ($Target -eq 'Quest') {
        . (Join-Path $PSScriptRoot 'platform-tools.ps1')
        $resolvedAdb = Get-Adb
        & (Join-Path $PSScriptRoot 'install-quest.ps1') -Gt4Only -Gt4Bundle $Bundle -Python $Python -Adb $resolvedAdb -Serial $Serial | Out-Host
        $message = 'GT4 INSTALLATION COMPLETE ON QUEST. Start GT2 normally on the headset.'
    } elseif ($Target -eq 'PC') {
        if (!$Runtime) {
            if ($Simulation -and (Test-Path -LiteralPath (Join-Path $Simulation 'disc.raw2352'))) {
                $Runtime = Split-Path ([IO.Path]::GetFullPath($Simulation))
            } else {
                foreach ($candidate in @('install-location.txt','native/install-location.txt')) {
                    $location = Join-Path $gt4Repo $candidate
                    if (Test-Path -LiteralPath $location) { $Runtime = (Get-Content -LiteralPath $location -Raw).Trim(); break }
                }
                if (!$Runtime) { $Runtime = Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime' }
            }
        }
        $result = Invoke-Gt4Python @((Join-Path $PSScriptRoot 'install-gt4-pack.py'), '--bundle', $Bundle, '--runtime', $Runtime)
        if ($result -ne 0) { throw "PC installation failed (exit code $result). The bundle is retained for retry." }
        $message = "GT4 INSTALLATION COMPLETE ON PC / PCVR: $Runtime"
    } else {
        $message = 'GT4 bundle saved for later. Cars have NOT been installed. Rerun the importer to install without converting again.'
    }
    [IO.File]::WriteAllText($resultFile, "$message`r`nBundle: $Bundle`r`nLog: $log", [Text.UTF8Encoding]::new($true))
    Write-Host $message
    Write-Host "Bundle retained: $Bundle"
    Write-Host "Result saved: $resultFile"
}
if ($result -ne 0) { Write-Host "GT4 import failed (exit code $result). Keep the output and log; do not delete them." -ForegroundColor Red }
} catch {
    $result = 1
    Write-Host "GT4 import failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'Keep the output and log; do not delete them.'
} finally {
    if ($log) { Write-Host "Import log: $log" }
    if ($transcribing) { Stop-Transcript | Out-Null }
}
exit $result
