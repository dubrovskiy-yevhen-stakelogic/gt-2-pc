param([string]$TestDirectory = (Join-Path $env:TEMP ('gt2-save-test-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot) 'scripts/transfer-saves.ps1') -FunctionsOnly
[IO.Directory]::CreateDirectory($TestDirectory) | Out-Null
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Reject([scriptblock]$Action) { $failed=$false; try { & $Action } catch { $failed=$true }; Assert $failed 'Expected rejection' }
$card = New-Object byte[] 131072
$card[0]=77; $card[1]=67; $card[127]=77 -bxor 67
for ($f=1; $f -lt 16; ++$f) { $card[$f*128]=0xA0; $card[$f*128+8]=255; $card[$f*128+9]=255; $card[$f*128+127]=0xA0 }
Reject { Test-MemoryCard $card }
$card[128]=0x51; $card[255]=0x51
Test-MemoryCard $card
$bad=$card.Clone(); $bad[127]=0; Reject { Test-MemoryCard $bad }
$bad=$card.Clone(); $bad[256]=7; Reject { Test-MemoryCard $bad }
Reject { Test-MemoryCard ([byte[]]@(77,67)) }
$pc=Join-Path $TestDirectory 'saves/arcade/card1.mcd'
$backup=Join-Path $TestDirectory 'old-card.mcd'
Write-PcCard $pc $card 'missing' $backup
$oldHash=Get-CardHash $pc
$next=$card.Clone(); $next[9000]=123
Reject { Write-PcCard $pc $next 'missing' $backup }
Assert ((Get-CardHash $pc) -eq $oldHash) 'Conflict changed destination'
Write-PcCard $pc $next $oldHash $backup
Assert ((Get-CardHash $backup) -eq $oldHash) 'Backup is not the previous card'
Assert ((Get-CardHash $pc) -eq (Get-DataHash $next)) 'Readback mismatch'
$savedHash=Get-CardHash $pc
Reject { Write-PcCard $pc $bad $savedHash (Join-Path $TestDirectory 'bad-backup.mcd') }
Assert ((Get-CardHash $pc) -eq $savedHash) 'Invalid source changed destination'
$guard = [IO.File]::Open((Join-Path (Split-Path $pc) '.transfer.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try { Reject { Write-PcCard $pc $card $savedHash (Join-Path $TestDirectory 'locked-backup.mcd') } }
finally { $guard.Dispose() }
Assert ((Get-CardHash $pc) -eq $savedHash) 'A locked save was overwritten'
[IO.File]::WriteAllBytes((Join-Path $TestDirectory 'fixture.mcd'),$card)
$Adb = (Get-Process -Id $PID).Path
$echoCode = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('[Console]::OpenStandardInput().CopyTo([Console]::OpenStandardOutput())'))
$binary = Invoke-AdbBytes @('-NoProfile','-EncodedCommand',$echoCode) $card
Assert ($binary -is [byte[]]) 'Binary subprocess output contains non-byte pipeline values'
Assert ((Get-DataHash $binary) -eq (Get-DataHash $card)) 'Binary subprocess corrupted card bytes'
Write-Host "Save transfer checks passed: invalid/empty cards, corrupt directory, missing source shape, conflict, backup and exact readback. Fixtures: $TestDirectory"
