param(
    [string]$Adb = 'adb',
    [string]$Serial,
    [string]$Output = (Join-Path (Split-Path $PSScriptRoot) ('work/quest-profiler-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'
if (!$Serial) {
    $devices = @(& $Adb devices)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot query ADB devices.' }
    $connected = @($devices | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($connected.Count -ne 1) { throw 'Connect one authorized Quest, or specify -Serial.' }
    $Serial = $connected[0]
}
if (Test-Path -LiteralPath $Output) { throw 'Choose a new output directory to preserve previous captures.' }
New-Item -ItemType Directory -Path $Output | Out-Null
& $Adb -s $Serial pull '/sdcard/Android/data/io.github.gt2pc.quest/files/logs' $Output
if ($LASTEXITCODE -ne 0) { throw 'Cannot collect logs. Enable FPS profiler in the game, drive, then turn it off first.' }
Get-ChildItem -LiteralPath $Output -Recurse -Filter 'profiler-*.csv' | Get-FileHash -Algorithm SHA256
Write-Host "Profiler logs copied to $Output. Headset files were retained."
