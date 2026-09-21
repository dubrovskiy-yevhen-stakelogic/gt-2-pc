[CmdletBinding()]
param(
    [ValidateSet('QuestToPC','PCToQuest')][string]$Direction,
    [ValidateSet('arcade','simulation','both')][string]$Disc = 'both',
    [string]$Runtime,
    [string]$Adb,
    [string]$Serial,
    [switch]$Yes,
    [switch]$FunctionsOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$provider = 'content://io.github.gt2pc.quest.saves'

function Test-MemoryCard([byte[]]$Data) {
    if ($Data.Length -ne 131072 -or $Data[0] -ne 77 -or $Data[1] -ne 67) { throw 'Not a 128 KiB PS1 memory card.' }
    $occupied = 0
    for ($frame = 0; $frame -lt 16; ++$frame) {
        $xor = 0
        for ($i = 0; $i -lt 128; ++$i) { $xor = $xor -bxor $Data[$frame*128+$i] }
        if ($xor) { throw "Damaged card directory frame $frame." }
        if ($frame -gt 0 -and $Data[$frame*128] -eq 0x51) { ++$occupied }
    }
    if (!$occupied) { throw 'The source card is empty. Nothing will be overwritten.' }
}
function Get-DataHash([byte[]]$Data) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Data))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-CardHash([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return 'missing' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbBytes([string[]]$Arguments, [byte[]]$InputBytes = @()) {
    # Avoid PowerShell's text encoding of native stdin/stdout for memory cards.
    foreach ($arg in $Arguments) { if ($arg -match '["\r\n]') { throw 'Invalid ADB argument.' } }
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Adb
    $start.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true; $start.RedirectStandardInput = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    $buffer = New-Object IO.MemoryStream
    try {
        [void]$process.Start()
        $errors = $process.StandardError.ReadToEndAsync()
        $output = $process.StandardOutput.BaseStream.CopyToAsync($buffer)
        if ($InputBytes.Length) { $process.StandardInput.BaseStream.Write($InputBytes,0,$InputBytes.Length) }
        $process.StandardInput.Close()
        if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'ADB timed out.' }
        [void]$output.GetAwaiter().GetResult()
        $errorText = $errors.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "ADB failed: $errorText" }
        return ,$buffer.ToArray()
    } finally { $buffer.Dispose(); $process.Dispose() }
}
function Invoke-AdbText([string[]]$Arguments) {
    return [Text.Encoding]::UTF8.GetString((Invoke-AdbBytes $Arguments))
}
function Get-QuestHash([string]$Mode) {
    $reply = Invoke-AdbText @('-s',$Serial,'shell','content','call','--uri',$provider,'--method','stat','--arg',$Mode)
    if ($reply -notmatch 'status=ok' -or $reply -notmatch 'sha256=(missing|[a-f0-9]{64})') {
        throw "Quest save access failed. Install GT2 VR 0.3.0 or newer and close the game. $reply"
    }
    return $Matches[1]
}
function Assert-GameClosed {
    if (Get-Process gt2game -ErrorAction SilentlyContinue) { throw 'Close GT2 on the PC before transferring saves.' }
}
function Write-PcCard([string]$Path, [byte[]]$Data, [string]$Expected, [string]$Backup) {
    Test-MemoryCard $Data
    Assert-GameClosed
    [IO.Directory]::CreateDirectory((Split-Path $Path)) | Out-Null
    $guard = [IO.File]::Open((Join-Path (Split-Path $Path) '.transfer.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try {
    if ((Get-CardHash $Path) -ne $Expected) { throw 'PC destination changed; repeat the transfer.' }
    $temporary = $Path + '.transfer-' + [guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllBytes($temporary,$Data)
    if ($Expected -eq 'missing') { [IO.File]::Move($temporary,$Path) }
    else { [IO.File]::Replace($temporary,$Path,$Backup) }
    if ((Get-CardHash $Path) -ne (Get-DataHash $Data)) { throw 'PC save readback failed. Restore the backup.' }
    } finally { $guard.Dispose() }
}
if ($FunctionsOnly) { return }
Assert-GameClosed
if (!$Direction) {
    $choice = Read-Host '1: Quest to PC, 2: PC to Quest'
    if ($choice -notin '1','2') { throw 'Choose 1 or 2.' }
    $Direction = if ($choice -eq '1') { 'QuestToPC' } else { 'PCToQuest' }
}
if (!$Runtime) {
    $hint = Join-Path (Split-Path $PSScriptRoot) 'install-location.txt'
    $Runtime = if (Test-Path -LiteralPath $hint) { (Get-Content -LiteralPath $hint -Raw).Trim() } else { Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime' }
    if (!$Yes) {
        $chosen = Read-Host "PC game folder [$Runtime]"
        if ($chosen) { $Runtime = $chosen.Trim('"') }
    }
}
if (!(Test-Path -LiteralPath (Join-Path $Runtime 'gt2game.exe'))) { throw 'Select the installed PC game folder containing gt2game.exe.' }
$Runtime = (Resolve-Path -LiteralPath $Runtime).Path
if (!$Adb) {
    . (Join-Path $PSScriptRoot 'platform-tools.ps1')
    $Adb = Get-Adb
} else { $Adb = (Get-Command $Adb -ErrorAction Stop).Source }
$devices = Invoke-AdbText @('devices')
$ready = @($devices -split '\r?\n' | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
if (!$Serial) {
    if ($ready.Count -ne 1) { throw 'Connect one Quest and accept USB debugging, or specify -Serial.' }
    $Serial = $ready[0]
}
if ($Serial -notin $ready) { throw 'Selected Quest is not connected and authorized.' }
$modes = if ($Disc -eq 'both') { @('arcade','simulation') } else { @($Disc) }
$plan = @()
foreach ($mode in $modes) {
    $path = Join-Path $Runtime "saves/$mode/card1.mcd"
    $pcHash = Get-CardHash $path
    $questHash = Get-QuestHash $mode
    $sourceHash = if ($Direction -eq 'PCToQuest') { $pcHash } else { $questHash }
    if ($sourceHash -eq 'missing') { Write-Host "$mode has no source card; skipping."; continue }
    if ($pcHash -eq $questHash) { Write-Host "$mode already matches; skipping."; continue }
    $data = if ($Direction -eq 'PCToQuest') { [IO.File]::ReadAllBytes($path) } else {
        Invoke-AdbBytes @('-s',$Serial,'exec-out','content','read','--uri',"$provider/card/$mode")
    }
    Test-MemoryCard $data
    if ((Get-DataHash $data) -ne $sourceHash) { throw 'Source changed during read; repeat the transfer.' }
    $plan += [pscustomobject]@{ Mode=$mode; Path=$path; Data=$data; Pc=$pcHash; Quest=$questHash; Hash=$sourceHash }
    Write-Host "$mode : $Direction (source $sourceHash)"
}
if (!$plan.Count) { Write-Host 'Nothing to transfer.'; return }
if (!$Yes -and (Read-Host 'Replace the destination cards after backing them up? (y/N)') -notmatch '^(y|yes)$') { Write-Host 'Cancelled.'; return }
$backup = Join-Path $Runtime ('save-backups/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($backup) | Out-Null
foreach ($item in $plan) {
    Assert-GameClosed
    [IO.File]::WriteAllBytes((Join-Path $backup ($item.Mode + '-source.mcd')),$item.Data)
    $old = Join-Path $backup ($item.Mode + '-destination.mcd')
    if ($Direction -eq 'QuestToPC') {
        Write-PcCard $item.Path $item.Data $item.Pc $old
    } else {
        if ((Get-QuestHash $item.Mode) -ne $item.Quest) { throw 'Quest destination changed; repeat the transfer.' }
        if ($item.Quest -ne 'missing') {
            $before = Invoke-AdbBytes @('-s',$Serial,'exec-out','content','read','--uri',"$provider/card/$($item.Mode)")
            if ((Get-DataHash $before) -ne $item.Quest) { throw 'Quest backup verification failed.' }
            [IO.File]::WriteAllBytes($old,$before)
        }
        $token = [guid]::NewGuid().ToString('N')
        $null = Invoke-AdbBytes @('-s',$Serial,'shell','-T','content','write','--uri',"$provider/stage/$token") $item.Data
        $reply = Invoke-AdbText @('-s',$Serial,'shell','content','call','--uri',$provider,'--method','commit','--arg',$item.Mode,
            '--extra',"token:s:$token",'--extra',"sha256:s:$($item.Hash)",'--extra',"expected:s:$($item.Quest)")
        if ($reply -notmatch 'status=ok' -or (Get-QuestHash $item.Mode) -ne $item.Hash) { throw "Quest import failed. Backups: $backup. $reply" }
    }
    Write-Host "$($item.Mode) transferred and verified."
}
Write-Host "Backups: $backup"
Write-Host 'Graphics, VR preferences and game assets were not replaced.'
