# Windows HD preparation: bounded batches, verified resume and named progress.
function Select-HdMediaForInstall {
    Write-Host 'Choose media preparation before installation:'
    Write-Host '1. Original media - quickest installation; HD can be prepared later (default).'
    Write-Host '2. HD menus/text/HUD - neural processing on this PC; can take hours on a slow GPU.'
    Write-Host '3. HD menus plus movies - additional processing for every movie frame.'
    switch (Read-Host 'Choose 1, 2 or 3 (Enter = 1)') {
        '' { return 'Original' }; '1' { return 'Original' }; '2' { return 'Menus' }; '3' { return 'MenusAndMovies' }
        default { throw 'Invalid media choice.' }
    }
}
function Get-HdPngSize([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        if ($stream.Length -lt 45) { throw "Incomplete PNG: $Path" }
        $header = New-Object byte[] 24
        [void]$stream.Read($header,0,24)
        if ([BitConverter]::ToString($header,0,8) -ne '89-50-4E-47-0D-0A-1A-0A' -or
            [Text.Encoding]::ASCII.GetString($header,12,4) -ne 'IHDR') { throw "Invalid PNG: $Path" }
        [void]$stream.Seek(-12,[IO.SeekOrigin]::End)
        $tail = New-Object byte[] 12
        [void]$stream.Read($tail,0,12)
        if ([BitConverter]::ToString($tail) -ne '00-00-00-00-49-45-4E-44-AE-42-60-82') { throw "Incomplete PNG: $Path" }
        $w = [int64]$header[16]*16777216 + [int64]$header[17]*65536 + [int64]$header[18]*256 + $header[19]
        $h = [int64]$header[20]*16777216 + [int64]$header[21]*65536 + [int64]$header[22]*256 + $header[23]
        if ($w -lt 1 -or $h -lt 1) { throw "Invalid PNG size: $Path" }
        return @($w,$h)
    } finally { $stream.Dispose() }
}

function Invoke-HdUpscale {
    param([string]$Exe,[string]$Models,[string]$InputDirectory,[string]$OutputDirectory,
        [string]$Label,[int]$Tile=128,[int]$Gpu=-1,[int]$BatchSize=32)
    if ($BatchSize -lt 1) { throw 'Invalid HD batch size.' }
    $inputs = @(Get-ChildItem -LiteralPath $InputDirectory -Filter '*.png' -File | Sort-Object Name)
    if (!$inputs.Count) { throw "No input pictures: $InputDirectory" }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $outputRoot = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
    $checkpoint = Join-Path $outputRoot 'upscale-resume.json'
    $identity = @($Exe,(Join-Path $Models 'realesrgan-x4plus.param'),(Join-Path $Models 'realesrgan-x4plus.bin')) |
        ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
    $identity = ($identity -join ':') + ":scale4:tile${Tile}:gpu${Gpu}"
    $saved = @{}
    if (Test-Path -LiteralPath $checkpoint) {
        try {
            $old = Get-Content -LiteralPath $checkpoint -Raw | ConvertFrom-Json
            if ($old.identity -eq $identity) { foreach ($row in $old.files) { $saved[$row.name] = $row } }
        } catch { Write-Host 'Unreadable HD checkpoint; verifying a fresh batch.' }
    }
    $verified = New-Object 'System.Collections.Generic.List[object]'
    $pending = New-Object 'System.Collections.Generic.List[object]'
    foreach ($file in $inputs) {
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        $target = Join-Path $outputRoot $file.Name
        $cached = $saved[$file.Name]
        if ($cached -and $cached.source -eq $hash -and (Test-Path -LiteralPath $target) -and
            (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $cached.output) {
            $verified.Add($cached)
        } else { $pending.Add([pscustomobject]@{file=$file;source=$hash}) }
    }
    Write-Host "$Label`: $($verified.Count)/$($inputs.Count) pictures already verified. Neural processing runs on this PC."
    $timer = [Diagnostics.Stopwatch]::StartNew()
    for ($start=0; $start -lt $pending.Count; $start+=$BatchSize) {
        $count = [Math]::Min($BatchSize,$pending.Count-$start)
        $batch = Join-Path $outputRoot ('.upscale-batch-'+[guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $batch | Out-Null
        $batchFull = [IO.Path]::GetFullPath($batch)
        if (!$batchFull.StartsWith($outputRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'HD batch escaped its output directory.' }
        try {
            for ($i=0;$i -lt $count;++$i) {
                $file = $pending[$start+$i].file
                Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $batch $file.Name)
            }
            $arguments = @('-i',$batch,'-o',$outputRoot,'-n','realesrgan-x4plus','-m',$Models,'-s','4','-t',"$Tile",'-f','png')
            if ($Gpu -ge 0) { $arguments += @('-g',"$Gpu") }
            # No shell: quote each argv value; paths are full filenames, not shell expressions.
            foreach ($arg in $arguments) { if ($arg.Contains('"') -or $arg.EndsWith('\')) { throw 'Unsupported upscaler argument.' } }
            $psi = New-Object Diagnostics.ProcessStartInfo
            $psi.FileName = $Exe
            $psi.Arguments = ($arguments | ForEach-Object { '"'+$_+'"' }) -join ' '
            $psi.UseShellExecute=$false; $psi.CreateNoWindow=$true
            $psi.RedirectStandardOutput=$true; $psi.RedirectStandardError=$true
            $process = New-Object Diagnostics.Process
            $process.StartInfo=$psi
            $started=$false
            try {
                $started=$process.Start()
                $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
                $lastReport=-15.0
                while (!$process.WaitForExit(1000)) {
                    if ($timer.Elapsed.TotalSeconds-$lastReport -ge 15) {
                        $lastReport=$timer.Elapsed.TotalSeconds
                        Write-Host ("{0}: {1}/{2} verified; processing next {3}; elapsed {4:mm\:ss}." -f $Label,$verified.Count,$inputs.Count,$count,$timer.Elapsed)
                    }
                }
                $log = $stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
                [IO.File]::WriteAllText((Join-Path $outputRoot 'upscale-last-batch.log'),$log)
                if ($process.ExitCode -ne 0) { throw "Upscaler failed ($($process.ExitCode)); see $outputRoot/upscale-last-batch.log. Completed batches are retained." }
            } finally {
                if ($started -and !$process.HasExited) { $process.Kill(); $process.WaitForExit() }
                $process.Dispose()
            }
            for ($i=0;$i -lt $count;++$i) {
                $row=$pending[$start+$i]; $target=Join-Path $outputRoot $row.file.Name
                $sourceSize=Get-HdPngSize $row.file.FullName; $targetSize=Get-HdPngSize $target
                if ($targetSize[0] -ne $sourceSize[0]*4 -or $targetSize[1] -ne $sourceSize[1]*4) { throw "Upscaler output is not 4x: $target" }
                $verified.Add([pscustomobject]@{name=$row.file.Name;source=$row.source;output=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash})
            }
            @{format=1;identity=$identity;files=@($verified.ToArray())} | ConvertTo-Json -Depth 4 |
                Set-Content -LiteralPath ($checkpoint+'.tmp') -Encoding UTF8
            Move-Item -LiteralPath ($checkpoint+'.tmp') -Destination $checkpoint -Force
            Write-Host "$Label`: $($verified.Count)/$($inputs.Count) pictures verified and saved."
        } finally { Remove-Item -LiteralPath $batchFull -Recurse -Force }
    }
}
