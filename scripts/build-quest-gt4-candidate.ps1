param(
    [Parameter(Mandatory=$true)][string]$Disc,
    [Parameter(Mandatory=$true)][string]$Pack,
    [Parameter(Mandatory=$true)][string]$Native,
    [Parameter(Mandatory=$true)][string]$Expansion,
    [Parameter(Mandatory=$true)][string]$AuditExe,
    [Parameter(Mandatory=$true)][string]$GateDirectory,
    [Parameter(Mandatory=$true)][string]$GeometryAuditExe,
    [Parameter(Mandatory=$true)][string]$GeometryGateDirectory,
    [Parameter(Mandatory=$true)][string]$Python,
    [Parameter(Mandatory=$true)][string]$Gradle
)
$ErrorActionPreference='Stop'
$gt2Repository=Split-Path $PSScriptRoot -Parent
Push-Location $gt2Repository
try {
    $gt2GateArguments=@('scripts/check_gt4_cabin_seals.py','--disc',$Disc,'--pack',$Pack,
        '--native',$Native,'--expansion',$Expansion,'--exe',$AuditExe,'--output',$GateDirectory,'--verify-report')
    & $Python @gt2GateArguments
    if($LASTEXITCODE -ne 0){throw 'GT4 candidate requires a complete, current colour gate. Run check_gt4_cabin_seals.py first.'}
    $gt2GeometryArguments=@('scripts/check_gt4_geometry.py','--disc',$Disc,'--pack',$Pack,
        '--native',$Native,'--expansion',$Expansion,'--exe',$GeometryAuditExe,'--output',$GeometryGateDirectory,'--verify-report')
    & $Python @gt2GeometryArguments
    if($LASTEXITCODE -ne 0){throw 'GT4 candidate requires a current full-catalogue road/wheel gate.'}
    Push-Location android
    try {
        & $Gradle --no-daemon --no-problems-report --offline --console=plain :app:assembleDebug
        if($LASTEXITCODE -ne 0){throw 'Quest APK build failed'}
    } finally {Pop-Location}
    & $Python @gt2GateArguments
    if($LASTEXITCODE -ne 0){throw 'Inputs changed while building the APK; candidate is rejected'}
    & $Python @gt2GeometryArguments
    if($LASTEXITCODE -ne 0){throw 'Road/wheel inputs changed while building the APK; candidate is rejected'}
} finally {Pop-Location}
