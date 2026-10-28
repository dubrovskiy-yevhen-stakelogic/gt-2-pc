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
if (!$PrepareToolsOnly) {
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
$gt4Args = @((Join-Path $PSScriptRoot 'prepare-gt4-release.py'), '--iso', $Iso, '--simulation', $Simulation, '--output', $Output)
if ($Arcade) { $gt4Args += @('--arcade', $Arcade) }
if ($Build) { $gt4Args += @('--build', $Build) }
if ($ModelTool) { $gt4Args += @('--model-tool', $ModelTool) }
if ($CatalogTool) { $gt4Args += @('--catalog-tool', $CatalogTool) }
if ($PdtoolsRoot) { $gt4Args += @('--pdtools-root', $PdtoolsRoot) }
if ($Offline) { $gt4Args += '--offline' }
Push-Location $gt4Repo
try {
    & $Python -E -s @gt4Args
    $result = $LASTEXITCODE
    if ($result -eq 0) {
        $bundle = Join-Path $Output 'gt4-models.tar'
        $bundleArgs = @((Join-Path $PSScriptRoot 'package-quest-gt4.py'), '--pack', (Join-Path $Output 'modelsets/gt4'),
            '--native', (Join-Path $Output 'modelsets/gt4/native-v16'), '--simulation-expansion', (Join-Path $Output 'expansions/simulation'), '--output', $bundle)
        if ($Arcade) { $bundleArgs += @('--arcade-expansion', (Join-Path $Output 'expansions/arcade')) }
        & $Python -E -s @bundleArgs
        $result = $LASTEXITCODE
        if ($result -eq 0) { Write-Host "GT4 bundle ready: $bundle"; Write-Host 'See docs/GT4-CARS.md for desktop and Quest installation. Python is prepared automatically.' }
    }
} finally { Pop-Location }
exit $result
