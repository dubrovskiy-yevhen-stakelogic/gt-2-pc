param(
    [Parameter(Mandatory)][string]$Iso,
    [Parameter(Mandatory)][string]$Simulation,
    [string]$Arcade,
    [string]$Output,
    [string]$Build,
    [string]$Python = 'python',
    [string]$ModelTool,
    [string]$CatalogTool,
    [string]$PdtoolsRoot,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$gt4Repo = Split-Path $PSScriptRoot -Parent
if (!$Output) { $Output = Join-Path $gt4Repo ('work\gt4-import-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$gt4Args = @((Join-Path $PSScriptRoot 'prepare-gt4-release.py'), '--iso', $Iso, '--simulation', $Simulation, '--output', $Output)
if ($Arcade) { $gt4Args += @('--arcade', $Arcade) }
if ($Build) { $gt4Args += @('--build', $Build) }
if ($ModelTool) { $gt4Args += @('--model-tool', $ModelTool) }
if ($CatalogTool) { $gt4Args += @('--catalog-tool', $CatalogTool) }
if ($PdtoolsRoot) { $gt4Args += @('--pdtools-root', $PdtoolsRoot) }
if ($Offline) { $gt4Args += '--offline' }
Push-Location $gt4Repo
try { & $Python @gt4Args; $result = $LASTEXITCODE } finally { Pop-Location }
exit $result
