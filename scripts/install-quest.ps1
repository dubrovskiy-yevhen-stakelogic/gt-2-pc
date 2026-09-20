param(
    [string]$Adb = 'adb',
    [string]$Serial,
    [string]$Apk = (Join-Path (Split-Path $PSScriptRoot) 'android/app/build/outputs/apk/debug/app-debug.apk'),
    [string]$Runtime = (Join-Path (Split-Path $PSScriptRoot) 'runtime'),
    [ValidateSet('arcade','simulation')][string]$Mode = 'arcade',
    [switch]$BothDiscs
)
$ErrorActionPreference = 'Stop'
$package = 'io.github.gt2pc.quest'
if (!(Test-Path -LiteralPath $Apk)) { throw 'Build the Quest APK first.' }
$devices = @(& $Adb devices)
if ($LASTEXITCODE -ne 0) { throw 'Cannot query ADB devices.' }
if (!$Serial) {
    $connected = @($devices | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($connected.Count -ne 1) { throw 'Connect one authorized Quest, or specify -Serial.' }
    $Serial = $connected[0]
}
if ($Serial -notmatch '^[A-Za-z0-9._:-]+$') { throw 'Invalid ADB serial.' }
function Invoke-Adb([string[]]$Arguments) {
    & $Adb -s $Serial @Arguments
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($Arguments -join ' ')" }
}
$modes = if ($BothDiscs) { @('arcade','simulation') } else { @($Mode) }
foreach ($disc in $modes) {
    $source = Join-Path $Runtime $disc
    if (!(Test-Path (Join-Path $source 'disc.raw2352')) -or !(Test-Path (Join-Path $source 'assets'))) {
        throw "Prepare $disc with INSTALL.bat before installing to Quest."
    }
}
Invoke-Adb @('install','-r',$Apk)
$external = "/sdcard/Android/data/$package/files"
$reply = @(Invoke-Adb @('shell','am','broadcast','-n',"$package/.ImportAccessReceiver",'-a',"$package.PREPARE_DATA"))
if (($reply -join ' ') -notmatch 'result=0' -or ($reply -join ' ') -notmatch 'files') {
    throw 'The application did not create its external files directory.'
}
foreach ($disc in $modes) {
    $source = Join-Path $Runtime $disc
    $source = [IO.Path]::GetFullPath($source)
    $directories = @($disc, "$disc/assets")
    $directories += @(Get-ChildItem (Join-Path $source 'assets') -Recurse -Directory | ForEach-Object {
        "$disc/" + $_.FullName.Substring($source.Length + 1).Replace('\','/')
    })
    $ready = @(Invoke-Adb @('shell','am','broadcast','-n',"$package/.ImportAccessReceiver",'-a',"$package.PREPARE_DATA",'--es','paths',($directories -join ',')))
    if (($ready -join ' ') -notmatch 'result=0') { throw "Cannot prepare $disc directories as application UID." }
    Invoke-Adb @('push','--sync',(Join-Path $source 'assets'),"$external/$disc/")
    Invoke-Adb @('push','--sync',(Join-Path $source 'disc.raw2352'),"$external/$disc/disc.raw2352")
    $expected = (Get-FileHash (Join-Path $source 'disc.raw2352') -Algorithm SHA256).Hash.ToLowerInvariant()
    $actual = @(Invoke-Adb @('shell','sha256sum',"$external/$disc/disc.raw2352"))
    if (($actual -join ' ') -notmatch "^$expected\s") { throw "$disc disc hash differs on the headset." }
    $access = @(Invoke-Adb @('shell','am','broadcast','-n',"$package/.ImportAccessReceiver",'-a',"$package.VERIFY_DATA",'--es','paths',"$disc/disc.raw2352,$disc/assets/.carcolor"))
    if (($access -join ' ') -notmatch 'result=0') { throw "The app cannot read $disc data." }
    Write-Host "$disc verified: disc SHA256 and application read access."
}
$modeFile = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllText($modeFile, $Mode, [Text.UTF8Encoding]::new($false))
    Invoke-Adb @('push',$modeFile,"$external/launch-mode.txt")
} finally { Remove-Item -LiteralPath $modeFile }
Invoke-Adb @('shell','dumpsys','package',$package)
Write-Host 'GT2 VR installed. Launch it from Unknown Sources on the headset. Existing saves were retained.'
