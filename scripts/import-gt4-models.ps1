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
    [switch]$PrepareToolsOnly
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
$dotnet = Get-Gt2DotNet -ToolsRoot $ToolsRoot -Offline:$Offline
$env:DOTNET_ROOT = Split-Path $dotnet
$env:DOTNET_ROOT_X64 = $env:DOTNET_ROOT
$env:PATH = $env:DOTNET_ROOT + ';' + $env:PATH
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:PYTHONNOUSERSITE = '1'
Write-Host "Python ready: $Python"
Write-Host "NET SDK ready: $dotnet"
if ($PrepareToolsOnly) { return }
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
        if ($result -eq 0) {
            if (!(Test-Path -LiteralPath $bundle -PathType Leaf)) { throw 'Packaging reported success but the GT4 bundle is missing.' }
            $runtime = Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime'
            if (Test-Path -LiteralPath (Join-Path $Simulation 'disc.raw2352')) { $runtime = Split-Path ([IO.Path]::GetFullPath($Simulation)) }
            function Quote-Gt4Path([string]$Path) { return "'" + $Path.Replace("'", "''") + "'" }
            $bundleArg = Quote-Gt4Path $bundle
            $runtimeArg = Quote-Gt4Path $runtime
            $pcInstaller = Quote-Gt4Path (Join-Path $PSScriptRoot 'install-gt4-pack.ps1')
            $questInstaller = Quote-Gt4Path (Join-Path $gt4Repo 'native/scripts/install-player.ps1')
            $instructions = @"
GT4 IMPORT COMPLETE - INSTALLATION IS STILL REQUIRED

Verified bundle: $bundle
Import log: $log

Keep the bundle. You do not need to convert the discs again.
Close the game before installing. Run the matching command in PowerShell.
The runtime folder must contain your prepared arcade and/or simulation discs.
Detected/default runtime: $runtime

Quest (connect one headset and allow USB debugging):
powershell.exe -NoProfile -ExecutionPolicy Bypass -File $questInstaller -Target Quest -Gt4Bundle $bundleArg -InstallDir $runtimeArg

PC / PCVR:
powershell.exe -NoProfile -ExecutionPolicy Bypass -File $pcInstaller -Bundle $bundleArg -Runtime $runtimeArg

After installation, use your usual game launcher. See docs/GT4-CARS.md.
"@
            $resultFile = Join-Path $Output 'INSTALL-GT4.txt'
            [IO.File]::WriteAllText($resultFile, $instructions, [Text.UTF8Encoding]::new($true))
            Write-Host ''
            Write-Host $instructions
            Write-Host "These instructions are saved in: $resultFile"
        }
    }
} finally { Pop-Location }
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
