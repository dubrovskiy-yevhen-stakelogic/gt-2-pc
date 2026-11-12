$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path $PSScriptRoot) 'scripts/existing-install.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('gt2-update-' + [guid]::NewGuid().ToString('N'))
$oldLocal = $env:LOCALAPPDATA
function Assert($condition, $message) { if (!$condition) { throw $message } }
function Prepare($path) {
    New-Item -ItemType Directory -Force "$path/simulation/assets", "$path/saves/simulation", "$path/hd" | Out-Null
    Set-Content "$path/simulation/disc.raw2352" 'disc fixture'
    Set-Content "$path/saves/simulation/card1.mcd" 'save fixture'
    Set-Content "$path/hd/keep.txt" 'HD fixture'
}
try {
    $package = Join-Path $testRoot 'new MSI'
    $env:LOCALAPPDATA = Join-Path $testRoot 'local'
    $legacy = Join-Path $env:LOCALAPPDATA 'GT2-VR/runtime'
    New-Item -ItemType Directory -Force "$package/tools", "$package/scripts" | Out-Null
    Prepare $legacy
    Assert ((Find-Gt2ExistingData $package) -eq $legacy) 'Fresh MSI must find legacy data'
    $before = @{}
    Get-ChildItem $legacy -Recurse -File | ForEach-Object { $before[$_.FullName] = (Get-FileHash $_.FullName).Hash }
    foreach ($name in @('gt2game.exe','gt2install.exe','gt2checks.exe','openxr_loader.dll')) { Set-Content "$package/tools/$name" "new $name" }
    Update-Gt2ExistingData $package $legacy
    foreach ($path in $before.Keys) { Assert ((Get-FileHash $path).Hash -eq $before[$path]) "Changed user data: $path" }
    Assert ((Get-Content "$package/install-location.txt" -Raw).Trim() -eq $legacy) 'Must persist discovered location'
    Assert ((Get-Content "$legacy/gt2game.exe" -Raw).Trim() -eq 'new gt2game.exe') 'Must refresh executable'
    Set-Content "$package/install-location.txt" "$testRoot/missing-custom"
    Assert ((Find-Gt2ExistingData $package) -eq "$testRoot/missing-custom") 'Do not silently switch stale custom location'
    Remove-Item "$package/install-location.txt"
    Prepare "$package/runtime"
    Assert ((Find-Gt2ExistingData $package) -eq (Join-Path $package 'runtime')) 'Portable data has priority over legacy default'
    Assert (!(Test-Gt2ExistingData "$testRoot/empty")) 'Empty path is not an install'
    New-Item -ItemType Directory -Force "$testRoot/only-saves/saves" | Out-Null
    Assert (!(Test-Gt2ExistingData "$testRoot/only-saves")) 'Saves alone must not skip import'
    # Execute the real installer with a selected custom data folder. If reuse fails,
    # it will reach missing HD/import helpers and fail, rather than prompt the user.
    $source = Split-Path $PSScriptRoot
    Copy-Item "$source/scripts/install-player.ps1", "$source/scripts/existing-install.ps1" "$package/scripts"
    foreach ($name in @('install.ps1','launch.ps1','platform-tools.ps1','transfer-saves.ps1')) { Set-Content "$package/scripts/$name" "throw 'Unexpected import/helper execution'" }
    $files = @(Get-ChildItem $package/tools,$package/scripts -File | ForEach-Object {
        @{ path=$_.FullName.Substring($package.Length+1).Replace('\','/'); sha256=(Get-FileHash $_.FullName).Hash }
    })
    @{version='0.9.2';files=$files} | ConvertTo-Json -Depth 4 | Set-Content "$package/release-manifest.json"
    $custom = Join-Path $testRoot 'custom data'
    Prepare $custom
    & "$package/scripts/install-player.ps1" -Target PC -InstallDir $custom -ReuseExisting
    Assert ((Get-Content "$package/install-location.txt" -Raw).Trim() -eq $custom) 'Selected existing folder must be attached'
    Assert ((Get-Content "$custom/saves/simulation/card1.mcd" -Raw).Trim() -eq 'save fixture') 'Installer changed save'
    Write-Host 'PASS: legacy discovery, portable priority, explicit hint, missing data, executable refresh, data preservation, selected-folder reuse'
} finally {
    $env:LOCALAPPDATA = $oldLocal
    # Only this test-created temporary tree is removed.
    if ([IO.Path]::GetFullPath($testRoot).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()))) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
