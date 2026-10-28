param(
    [Parameter(Mandatory)][string]$Bundle,
    [Parameter(Mandatory)][string]$Runtime,
    [string]$Python,
    [string]$ToolsRoot,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-prerequisites.ps1')
$Python = Get-Gt2Python -Python $Python -ToolsRoot $ToolsRoot -Offline:$Offline
& $Python -E -s (Join-Path $PSScriptRoot 'install-gt4-pack.py') --bundle $Bundle --runtime $Runtime
exit $LASTEXITCODE
