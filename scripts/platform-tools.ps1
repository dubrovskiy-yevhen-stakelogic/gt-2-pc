function Get-Adb {
    if ($Adb) { return (Get-Command $Adb -ErrorAction Stop).Source }
    $found = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    $cache = Join-Path $env:LOCALAPPDATA 'GT2-VR/Tools/platform-tools-36.0.2'
    $hashes = [ordered]@{
        'adb.exe' = '56656270DA132F44E9CB4FB86A12BA965635C80423D43DCDD944D9FEC4AB4622'
        'AdbWinApi.dll' = 'A00CF631DD12C82561FFCCEFDB1A99A27C527253B04F06CA8FC1BB86BA2148C4'
        'AdbWinUsbApi.dll' = 'E4D72D5BA3BF4B027F1B7A3781AAE0B04712C1A52244BEA277FB57DD75A85702'
        'NOTICE.txt' = 'BFEDFB7B22C5D204BAECBCCFBB9A6DE6E848EF19F9318088F15769BFBF4B8F79'
        'source.properties' = '6F16C7815EE0A1B820CA24C8FDAD8E26F07DA492B22C2B370C6DB136425A2136'
    }
    $valid = $true
    foreach ($name in $hashes.Keys) {
        $path = Join-Path $cache $name
        if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$name]) { $valid = $false }
    }
    if ($valid) { return (Join-Path $cache 'adb.exe') }
    Write-Host 'Downloading Android Platform Tools 36.0.2 from Google. SDK terms: https://developer.android.com/studio/terms'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $temporary = Join-Path $cache ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporary | Out-Null
    $zip = Join-Path $temporary 'platform-tools.zip'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest 'https://dl.google.com/android/repository/platform-tools_r36.0.2-win.zip' -OutFile $zip -UseBasicParsing
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne 'B024D4F319D6AD3004DE1BA7B96A5C7C5F3512E8B14126308D598B4AB93DCEAD') { throw 'Platform Tools download hash mismatch.' }
    Expand-Archive -LiteralPath $zip -DestinationPath $temporary
    foreach ($name in $hashes.Keys) {
        $path = Join-Path $temporary ('platform-tools/' + $name)
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $hashes[$name]) { throw "Platform Tools hash mismatch: $name" }
    }
    foreach ($name in $hashes.Keys) { Copy-Item -LiteralPath (Join-Path $temporary ('platform-tools/' + $name)) -Destination (Join-Path $cache $name) -Force }
    return (Join-Path $cache 'adb.exe')
}
