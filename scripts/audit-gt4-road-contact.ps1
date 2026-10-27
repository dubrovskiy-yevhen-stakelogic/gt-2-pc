param(
    [Parameter(Mandatory)][string]$DiscDirectory,
    [Parameter(Mandatory)][string]$ModelSet,
    [string]$Expansion,
    [string]$Build = 'build_gt4_models',
    [string]$Output = 'work/gt4-road-contact-audit',
    [switch]$Stock,
    [switch]$Drive,
    [string[]]$Cars
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$binary = Join-Path $root "$Build/gt2roadcontactaudit.exe"
if (!(Test-Path -LiteralPath $binary)) { throw 'Build the gt2roadcontactaudit CMake target first.' }
$previousExpansion = $env:GT2_CAR_EXPANSION
$previousStock=$env:GT2_AUDIT_STOCK
$previousDrive=$env:GT2_AUDIT_ROAD_DRIVE
Push-Location $root
try {
    if ($Expansion) { $env:GT2_CAR_EXPANSION = [IO.Path]::GetFullPath($Expansion) }
    $env:GT2_AUDIT_STOCK=if($Stock){'1'}else{$null}
    $env:GT2_AUDIT_ROAD_DRIVE=if($Drive){'1'}else{$null}
    & $binary $DiscDirectory $ModelSet $Output @Cars
    if ($LASTEXITCODE -ne 0) { throw "Road-contact audit failed. Inspect $Output/contacts.csv and errors.csv." }
} finally {
    $env:GT2_CAR_EXPANSION = $previousExpansion
    $env:GT2_AUDIT_STOCK=$previousStock
    $env:GT2_AUDIT_ROAD_DRIVE=$previousDrive
    Pop-Location
}
