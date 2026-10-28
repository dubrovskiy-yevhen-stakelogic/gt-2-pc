param(
    [string]$Adb = 'adb',
    [string]$Serial,
    [string]$Apk = (Join-Path (Split-Path $PSScriptRoot) 'android/app/build/outputs/apk/debug/app-debug.apk'),
    [string]$Runtime = (Join-Path (Split-Path $PSScriptRoot) 'runtime'),
    [ValidateSet('arcade','simulation')][string]$Mode = 'arcade',
    [switch]$BothDiscs,
    [switch]$Gt4Only,
    [string]$Gt4Bundle,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$package = 'io.github.gt2pc.quest'
if ($Gt4Bundle) {
    . (Join-Path $PSScriptRoot 'windows-prerequisites.ps1')
    $Python = Get-Gt2Python -Python $Python
}
if ($Gt4Only -and !$Gt4Bundle) { throw 'GT4-only installation requires a prepared bundle.' }
if (!$Gt4Only -and !(Test-Path -LiteralPath $Apk)) { throw 'Build the Quest APK first.' }
$devices = @(& $Adb devices)
if ($LASTEXITCODE -ne 0) { throw 'Cannot query ADB devices.' }
if (!$Serial) {
    $connected = @($devices | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($connected.Count -ne 1) { throw 'Connect one authorized Quest, or specify -Serial.' }
    $Serial = $connected[0]
}
if ($Serial -notmatch '^[A-Za-z0-9._:-]+$') { throw 'Invalid ADB serial.' }
function Invoke-Adb([string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 1
    & $Adb -s $Serial @Arguments 2>&1 | ForEach-Object { $_.ToString() }
    if ($global:LASTEXITCODE -ne 0) { throw "ADB failed: $($Arguments -join ' ')" }
}
$modes = if ($Gt4Only) { @() } elseif ($BothDiscs) { @('arcade','simulation') } else { @($Mode) }
foreach ($disc in $modes) {
    $source = Join-Path $Runtime $disc
    if (!(Test-Path (Join-Path $source 'disc.raw2352')) -or !(Test-Path (Join-Path $source 'assets'))) {
        throw "Prepare $disc with INSTALL.bat before installing to Quest."
    }
}
if ($Gt4Bundle) {
    $Gt4Bundle = (Resolve-Path -LiteralPath $Gt4Bundle).Path
    & $Python (Join-Path $PSScriptRoot 'package-quest-gt4.py') --verify $Gt4Bundle
    if ($LASTEXITCODE -ne 0) { throw 'GT4 bundle validation failed; nothing was installed.' }
}
if ($Gt4Only) {
    $installed = @(Invoke-Adb @('shell','pm','path',$package))
    if (($installed -join ' ') -notmatch 'package:') { throw 'GT2 is not installed on this Quest. Install the game first, then rerun the importer to use the saved bundle.' }
    Write-Host "Quest GT4: updating cars on $Serial. Discs, APK, saves and settings are retained."
    Invoke-Adb @('shell','am','force-stop',$package)
} else {
    Write-Host 'Quest stage: installing APK. File-transfer percentages below apply to each named group, not the whole installation.'
    Invoke-Adb @('install','-r',$Apk)
}
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
    $hd = Join-Path $source 'hd'
    if (Test-Path -LiteralPath $hd) {
        $directories += "$disc/hd"
        $directories += @(Get-ChildItem -LiteralPath $hd -Recurse -Directory | ForEach-Object { "$disc/hd/" + $_.FullName.Substring($hd.Length+1).Replace('\','/') })
    }
    $ready = @(Invoke-Adb @('shell','am','broadcast','-n',"$package/.ImportAccessReceiver",'-a',"$package.PREPARE_DATA",'--es','paths',($directories -join ',')))
    if (($ready -join ' ') -notmatch 'result=0') { throw "Cannot prepare $disc directories as application UID." }
    if (Test-Path -LiteralPath $hd) { Write-Host "Quest $disc`: copying prepared HD media."; Invoke-Adb @('push','--sync',$hd,"$external/$disc/") }
    Write-Host "Quest $disc`: copying game assets."
    Invoke-Adb @('push','--sync',(Join-Path $source 'assets'),"$external/$disc/")
    Write-Host "Quest $disc`: copying disc image."
    Invoke-Adb @('push','--sync',(Join-Path $source 'disc.raw2352'),"$external/$disc/disc.raw2352")
    Write-Host "Quest $disc`: verifying copied data."
    $expected = (Get-FileHash (Join-Path $source 'disc.raw2352') -Algorithm SHA256).Hash.ToLowerInvariant()
    $actual = @(Invoke-Adb @('shell','sha256sum',"$external/$disc/disc.raw2352"))
    if (($actual -join ' ') -notmatch "^$expected\s") { throw "$disc disc hash differs on the headset." }
    $access = @(Invoke-Adb @('shell','am','broadcast','-n',"$package/.ImportAccessReceiver",'-a',"$package.VERIFY_DATA",'--es','paths',"$disc/disc.raw2352,$disc/assets/.carcolor"))
    if (($access -join ' ') -notmatch 'result=0') { throw "The app cannot read $disc data." }
    Write-Host "$disc verified: disc SHA256 and application read access."
}
foreach ($startupName in @('startup.gtm','startup-hd.gtm' | Where-Object { !$Gt4Only })) {
$startup = Join-Path $Runtime $startupName
if (Test-Path -LiteralPath $startup) {
    Invoke-Adb @('push','--sync',$startup,"$external/$startupName")
    $expected = (Get-FileHash -LiteralPath $startup -Algorithm SHA256).Hash.ToLowerInvariant()
    $actual = @(Invoke-Adb @('shell','sha256sum',"$external/$startupName"))
    if (($actual -join ' ') -notmatch "^$expected\s") { throw 'Startup movie hash differs on the headset.' }
}
}
if ($Gt4Bundle) {
    Write-Host 'Quest GT4: transferring the verified model and catalogue bundle.'
    $paths = 'modelsets/gt4,arcade/expansions/gt4,simulation/expansions/gt4'
    $ready = @(Invoke-Adb @('shell','am','broadcast','-n',"$package/.ImportAccessReceiver",'-a',"$package.PREPARE_DATA",'--es','paths',$paths))
    if (($ready -join ' ') -notmatch 'result=0') { throw 'Cannot prepare GT4 directories as application UID.' }
    $remoteBundle = '/data/local/tmp/gt2-models-' + [Guid]::NewGuid().ToString('N') + '.tar'
    Invoke-Adb @('push',$Gt4Bundle,$remoteBundle)
    $expected = (Get-FileHash -LiteralPath $Gt4Bundle -Algorithm SHA256).Hash.ToLowerInvariant()
    $actual = @(Invoke-Adb @('shell','sha256sum',$remoteBundle))
    if (($actual -join ' ') -notmatch "^$expected\s") { throw 'GT4 transfer hash mismatch.' }
    Write-Host 'Quest GT4: unpacking models on the headset. This can take several minutes.'
    Invoke-Adb @('shell','tar','-xf',$remoteBundle,'-C',$external)
    Write-Host 'Quest GT4: verifying every installed model and catalogue file.'
    # Paths are fixed application locations. The bundle validator rejects links,
    # traversal, duplicate members and payloads outside the three GT4 roots.
    Invoke-Adb @('shell',"cd $external && sha256sum -s -c modelsets/gt4/quest-install.sha256")
    $access = @(Invoke-Adb @('shell','am','broadcast','-n',"$package/.ImportAccessReceiver",'-a',"$package.VERIFY_DATA",'--es','paths','modelsets/gt4/manifest.json,modelsets/gt4/native-v16/manifest.json,modelsets/gt4/native-v16/wheels.json'))
    if (($access -join ' ') -notmatch 'result=0') { throw 'The app cannot read the GT4 models.' }
    Invoke-Adb @('shell','rm',$remoteBundle)
    Write-Host 'GT4 verified: complete file checksums and application read access.'
}
if ($Gt4Only) {
    Write-Host 'GT4 INSTALLATION COMPLETE ON QUEST. Start GT2 normally on the headset.'
    return
}
$modeFile = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllText($modeFile, $Mode, [Text.UTF8Encoding]::new($false))
    Invoke-Adb @('push',$modeFile,"$external/launch-mode.txt")
} finally { Remove-Item -LiteralPath $modeFile }
Invoke-Adb @('shell','dumpsys','package',$package)
Write-Host 'GT2 VR installed. Launch it from Unknown Sources on the headset. Existing saves were retained.'
