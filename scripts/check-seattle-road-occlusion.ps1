param(
    [Parameter(Mandatory=$true)][string]$Disc,
    [string]$Audit = "$PSScriptRoot/../build_install/gt2courseviews.exe"
)
$ErrorActionPreference = 'Stop'
$oldWall = $env:GT2_COURSE_WALL_CHECK
$oldFailures = $env:GT2_COURSE_FAILURES_ONLY
Push-Location "$PSScriptRoot/.."
try {
    $env:GT2_COURSE_WALL_CHECK = '1'
    $env:GT2_COURSE_FAILURES_ONLY = '1'
    & $Audit $Disc work/seattle-road-occlusion seatt_s 1 30 40
    if ($LASTEXITCODE -ne 0) { throw 'Seattle Short road occlusion regression failed; see work/seattle-road-occlusion' }
} finally {
    $env:GT2_COURSE_WALL_CHECK = $oldWall
    $env:GT2_COURSE_FAILURES_ONLY = $oldFailures
    Pop-Location
}
