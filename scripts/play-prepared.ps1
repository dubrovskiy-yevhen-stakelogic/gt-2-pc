[CmdletBinding(PositionalBinding=$false)]
param(
    [string]$Root,
    [switch]$VR,
    [ValidateSet('auto','meta','steamvr','vdxr')][string]$XRRuntime = 'auto',
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$GameArguments
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Root)) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = (Resolve-Path -LiteralPath $Root).Path
$data = Join-Path $Root 'data'
foreach ($mode in @('arcade','simulation')) {
    if (!(Test-Path -LiteralPath (Join-Path $data "$mode/disc.raw2352"))) { throw "Prepared $mode disc is missing." }
}
$exe = Join-Path $data 'gt2game.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Prepared Windows player is missing.' }
$env:GT2_GT4_MODEL_SET = Join-Path $Root 'modelsets/gt4'
$env:GT2_GT4_NATIVE = Join-Path $env:GT2_GT4_MODEL_SET 'native-v16'
if (!(Test-Path -LiteralPath (Join-Path $env:GT2_GT4_NATIVE 'manifest.json'))) { throw 'Prepared GT4 models are missing.' }
$playerArguments = @('--player','--data-root',$data,'--save-root',(Join-Path $data 'saves'))
if ($VR) {
    $playerArguments += '--vr'
    $env:GT2_XR_RUNTIME = $XRRuntime
    if ($XRRuntime -ne 'auto') { Remove-Item Env:XR_RUNTIME_JSON -ErrorAction SilentlyContinue }
}
$playerArguments += $GameArguments
Push-Location $Root
try { & $exe @playerArguments; $result = $LASTEXITCODE } finally { Pop-Location }
exit $result
