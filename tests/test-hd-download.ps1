param(
    [string]$TestDirectory = (Join-Path $env:TEMP ('gt2-hd-download-' + [guid]::NewGuid().ToString('N'))),
    [string]$StableArchive,
    [switch]$Online
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
# Load only the production downloader; do not prepare discs or launch any core.
$source = Join-Path (Split-Path $PSScriptRoot) 'scripts/prepare-hd.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
Assert (!$parseErrors.Count) 'Installer has PowerShell syntax errors.'
$fetch = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Fetch' }, $true)
Invoke-Expression $fetch.Extent.Text
New-Item -ItemType Directory -Path $TestDirectory | Out-Null
$Cache = Join-Path $TestDirectory 'cache'
New-Item -ItemType Directory -Path $Cache | Out-Null
$fixture = Join-Path $TestDirectory 'fixture'
New-Item -ItemType Directory -Path $fixture | Out-Null
Set-Content -LiteralPath (Join-Path $fixture 'payload.txt') -Value 'verified fixture' -Encoding ascii
$goodZip = Join-Path $TestDirectory 'fixture.zip'
Compress-Archive -LiteralPath (Join-Path $fixture 'payload.txt') -DestinationPath $goodZip
$goodHash = (Get-FileHash -LiteralPath $goodZip -Algorithm SHA256).Hash
$script:DownloadFixture = $goodZip
$script:Requests = 0
$script:RejectDownload = $false
function Invoke-WebRequest($Uri, $OutFile, [switch]$UseBasicParsing) {
    ++$script:Requests
    if ($script:RejectDownload) { throw 'Unexpected network request for a valid cache.' }
    if ($script:DownloadFixture) { Copy-Item -LiteralPath $script:DownloadFixture -Destination $OutFile -Force }
    else { Microsoft.PowerShell.Utility\Invoke-WebRequest -Uri $Uri -OutFile $OutFile -UseBasicParsing }
}
$first = Fetch 'https://fixture.invalid/core.zip' $goodHash 'fixture'
Assert ((Get-Content -LiteralPath (Join-Path $first 'payload.txt')).Trim() -eq 'verified fixture') 'ZIP extraction failed.'
Assert ($script:Requests -eq 1) 'Cold cache did not download.'
Set-Content -LiteralPath (Join-Path $first 'payload.txt') -Value 'tampered extracted data'
$script:RejectDownload = $true
$second = Fetch 'https://fixture.invalid/core.zip' $goodHash 'fixture'
Assert ($first -ne $second) 'Reused an untrusted extraction directory.'
Assert ((Get-Content -LiteralPath (Join-Path $second 'payload.txt')).Trim() -eq 'verified fixture') 'Trusted tampered extracted data.'
Assert ($script:Requests -eq 1) 'Warm cache requested network access.'
$script:RejectDownload = $false
Set-Content -LiteralPath (Join-Path $Cache 'fixture.zip') -Value 'broken cache'
$repaired = Fetch 'https://fixture.invalid/core.zip' $goodHash 'fixture'
Assert ($script:Requests -eq 2) 'Corrupt cache was not downloaded again.'
Assert ((Get-FileHash -LiteralPath (Join-Path $Cache 'fixture.zip')).Hash -eq $goodHash) 'Corrupt cache was retained.'
$badFile = Join-Path $TestDirectory 'bad-download.html'
Set-Content -LiteralPath $badFile -Value '<html>proxy error</html>' -Encoding ascii
$script:DownloadFixture = $badFile
$failure = ''
try { Fetch 'https://fixture.invalid/replaced.zip' $goodHash 'rejected' | Out-Null } catch { $failure = $_.Exception.Message }
Assert ($failure -match 'Expected:' -and $failure -match 'Received:' -and $failure -match 'https://fixture.invalid/replaced.zip') 'Mismatch lacks actionable diagnostics.'
Assert (!(Test-Path -LiteralPath (Join-Path $Cache 'rejected.zip'))) 'Invalid download was promoted to the trusted cache.'
Assert (@(Get-ChildItem -LiteralPath $Cache -Directory -Filter 'rejected-*').Count -eq 0) 'Invalid download was extracted.'
Assert (Test-Path -LiteralPath (Join-Path $Cache 'rejected.zip.download')) 'Failed bytes were not retained for diagnosis.'
Write-Host 'PASS: ZIP cold cache, offline reuse, fresh extraction, corrupt cache recovery, changed/HTML download rejection.'

if ($StableArchive -or $Online) {
    # Read release metadata from the actual call site so this tests what ships.
    $call = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Fetch' -and
        $node.Extent.Text -match 'RetroArch_cores\.7z'
    }, $true)
    Assert ($null -ne $call) 'Missing fixed core release.'
    $url = $call.CommandElements[1].Value
    $hash = $call.CommandElements[2].Value
    $name = $call.CommandElements[3].Value
    $memberAssignment = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$coreMember'
    }, $true)
    $member = $memberAssignment.Right.Find({ param($node) $node -is [Management.Automation.Language.StringConstantExpressionAst] }, $true).Value
    Assert ($url -match '/stable/[0-9.]+/windows/x86_64/RetroArch_cores\.7z$') 'Core URL is not a fixed release.'
    $script:DownloadFixture = if ($Online) { $null } else { (Resolve-Path -LiteralPath $StableArchive).Path }
    $coreDir = Fetch $url $hash $name $member
    $core = Join-Path $coreDir $member
    $files = @(Get-ChildItem -LiteralPath $coreDir -Recurse -File)
    Assert ($files.Count -eq 1 -and $files[0].Name -eq 'mednafen_psx_libretro.dll') 'Extracted unrelated cores.'
    $bytes = [IO.File]::ReadAllBytes($core)
    Assert ($bytes[0] -eq 77 -and $bytes[1] -eq 90) 'Extracted core is not a PE binary.'
    $coreHash = (Get-FileHash -LiteralPath $core).Hash
    $beforeReuse = $script:Requests
    $script:RejectDownload = $true
    $reused = Fetch $url $hash $name $member
    Assert ($script:Requests -eq $beforeReuse) 'Stable archive was downloaded again.'
    Assert ((Get-FileHash -LiteralPath (Join-Path $reused $member)).Hash -eq $coreHash) 'Stable extraction changed between runs.'
    $failure = ''
    try { Fetch $url $hash $name 'missing-core.dll' | Out-Null } catch { $failure = $_.Exception.Message }
    Assert ($failure.Length -gt 0) 'Missing archive member was accepted.'
    Write-Host "PASS: stable archive SHA-256, single-core extraction, offline reuse, missing member rejection. Core SHA-256: $coreHash"
    Write-Host "Verified core: $core"
}
Write-Host "Download checks passed. No downloaded program was executed. Fixtures: $TestDirectory"
