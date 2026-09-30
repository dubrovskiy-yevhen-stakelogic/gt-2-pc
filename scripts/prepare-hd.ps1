[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Runtime,
    [string]$BuildDir = (Join-Path (Split-Path $PSScriptRoot) 'build_update'),
    [ValidateSet('Original','Menus','MenusAndMovies')][string]$HdMedia = 'MenusAndMovies',
    [string]$Bios,
    [string]$CaptureCore,
    [string]$Cache,
    [int[]]$Movies = @(24,25,26)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Runtime = (Resolve-Path -LiteralPath $Runtime).Path
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
if (!$Cache) { $Cache = Join-Path $Runtime '.hd-work' }
$Cache = [IO.Path]::GetFullPath($Cache)
New-Item -ItemType Directory -Force -Path $Cache | Out-Null
$media = Join-Path $BuildDir 'gt2media.exe'
$inspect = Join-Path $BuildDir 'gt2install.exe'
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Asset preparation failed ($LASTEXITCODE): $Exe. Work is retained in $Cache." }
}
function Fetch([string]$Url, [string]$Hash, [string]$Name, [string]$ArchiveMember) {
    $extension = if ($ArchiveMember) { '.7z' } else { '.zip' }
    $zip = Join-Path $Cache ($Name + $extension)
    if (!(Test-Path -LiteralPath $zip) -or (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $Hash) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest $Url -OutFile ($zip + '.download') -UseBasicParsing
        $actual = (Get-FileHash -LiteralPath ($zip + '.download') -Algorithm SHA256).Hash
        if ($actual -ne $Hash) { throw "The $Name download failed SHA-256 verification. Expected: $Hash. Received: $actual. URL: $Url. The downloaded archive was not extracted or executed. Retry with the current installer or supply a verified local tool." }
        Move-Item -LiteralPath ($zip + '.download') -Destination $zip -Force
    }
    # Re-extract verified bytes rather than trusting an executable left in the cache.
    $folder = Join-Path $Cache ($Name + '-' + [guid]::NewGuid().ToString('N'))
    if ($ArchiveMember) {
        # The versioned core bundle is 7z. Extract only the software PSX core.
        $tar = Join-Path $env:SystemRoot 'System32/tar.exe'
        $seven = Join-Path $env:ProgramFiles '7-Zip/7z.exe'
        New-Item -ItemType Directory -Path $folder | Out-Null
        if (Test-Path -LiteralPath $seven) {
            & $seven x $zip "-o$folder" $ArchiveMember -y | Out-Null
        } elseif (Test-Path -LiteralPath $tar) {
            & $tar -xf $zip -C $folder $ArchiveMember
        } else { throw 'BIOS startup capture requires Windows tar.exe or 7-Zip to extract the core bundle. Install 7-Zip or supply -CaptureCore.' }
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath (Join-Path $folder $ArchiveMember) -PathType Leaf)) {
            throw 'Could not extract the BIOS capture core. Install current 7-Zip or supply -CaptureCore.'
        }
    } else { Expand-Archive -LiteralPath $zip -DestinationPath $folder }
    return $folder
}
function Publish([string]$Source, [string]$Destination) {
    # All sources are staging paths below this task's cache; destinations stay inside Runtime.
    $sourceFull = [IO.Path]::GetFullPath($Source)
    $destFull = [IO.Path]::GetFullPath($Destination)
    if (!$sourceFull.StartsWith($Cache.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
        !$destFull.StartsWith($Runtime.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Asset publication escaped its directory.' }
    $backup = $destFull + '.backup-' + [guid]::NewGuid().ToString('N')
    $hadOld = Test-Path -LiteralPath $destFull
    if ($hadOld) { Move-Item -LiteralPath $destFull -Destination $backup }
    try { Move-Item -LiteralPath $sourceFull -Destination $destFull }
    catch { if ($hadOld) { Move-Item -LiteralPath $backup -Destination $destFull }; throw }
}
function Write-HdManifest([string]$Folder,[string]$Profile) {
    $files = @(Get-ChildItem -LiteralPath $Folder -Recurse -File | Where-Object { $_.Name -ne 'manifest.json' } | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($Folder.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    [ordered]@{format=1;profile=$Profile;model='realesrgan-x4plus';scale=4;ui='indexed-scale4x-v2';fonts='palette-contours4x-v1';video='source-bounded-v1';files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Folder 'manifest.json') -Encoding UTF8
}
$discs = @()
foreach ($mode in @('arcade','simulation')) {
    $path = Join-Path $Runtime "$mode/disc.raw2352"
    if (Test-Path -LiteralPath $path) {
        $json = & $inspect inspect $path
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect $mode." }
        $info = $json | ConvertFrom-Json
        $discs += [pscustomobject]@{ mode=$mode; path=$path; profile=$info.exeSha1 }
    }
}
if (!$discs.Count) { throw 'No installed discs.' }
if ($HdMedia -ne 'Original') {
    $upscaleDir = Fetch 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip' 'ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D' 'realesrgan-20220424'
    $upscale = Join-Path $upscaleDir 'realesrgan-ncnn-vulkan.exe'
    function Upscale([string]$InputDir,[string]$OutputDir) {
        New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
        Run $upscale @('-i',$InputDir,'-o',$OutputDir,'-n','realesrgan-x4plus','-m',(Join-Path $upscaleDir 'models'),'-s','4','-t','128','-f','png')
    }
    foreach ($disc in $discs) {
        Write-Host "Preparing HD media for $($disc.mode). This can take a long time; keep the PC connected to power."
        $discHash = (Get-FileHash -LiteralPath $disc.path -Algorithm SHA256).Hash.ToLowerInvariant()
        $work = Join-Path $Cache ($discHash + '-v2')
        New-Item -ItemType Directory -Force -Path $work | Out-Null
        $stage = Join-Path $work ('candidate-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $stage | Out-Null
        $original = Join-Path $work 'images-original'
        Run $media @('images',$disc.path,$original)
        $images = Join-Path $work 'images-upscaled'
        # A completion stamp is created only after the whole group succeeds.
        if (!(Test-Path -LiteralPath (Join-Path $images 'complete.txt'))) {
            $inputImages = Join-Path $work 'image-input'
            New-Item -ItemType Directory -Force -Path $inputImages | Out-Null
            Get-ChildItem -LiteralPath $original -Filter '*.png' | Copy-Item -Destination $inputImages -Force
            Upscale $inputImages $images
            Run $media @('fit-images',$images)
            Set-Content -LiteralPath (Join-Path $images 'complete.txt') -Value 'realesrgan-x4plus-4x-v1' -Encoding ascii
        }
        Run $media @('fit-images',$images)
        $expectedImages = @(Get-ChildItem -LiteralPath $original -Filter '*.png' | ForEach-Object Name | Sort-Object)
        $actualImages = @(Get-ChildItem -LiteralPath $images -Filter '*.png' | ForEach-Object Name | Sort-Object)
        if (!$expectedImages.Count -or (Compare-Object $expectedImages $actualImages)) { throw "HD image set is incomplete for $($disc.mode)." }
        New-Item -ItemType Directory -Path (Join-Path $stage 'images') | Out-Null
        Get-ChildItem -LiteralPath $images -Filter '*.png' | Copy-Item -Destination (Join-Path $stage 'images')
        Copy-Item -LiteralPath (Join-Path $original 'profile.txt') -Destination $stage
        $ui = Join-Path $work 'ui-indexed-scale4x-v2-pages'
        $uiDone = Join-Path $work 'ui-indexed-scale4x-v2-pages.complete'
        if (!(Test-Path -LiteralPath $uiDone)) {
            Run $media @('ui',$disc.path,$ui)
            Set-Content -LiteralPath $uiDone -Value 'complete' -Encoding ascii
        }

        $mapDone = Join-Path $work 'ui-map-boundaries-v1.complete'
        if (!(Test-Path -LiteralPath $mapDone)) {
            Run $media @('ui-maps',$disc.path,$ui)
            Set-Content -LiteralPath $mapDone -Value 'complete' -Encoding ascii
        }
        $keys = @(Get-Content -LiteralPath (Join-Path $ui 'index.txt') | Select-Object -Skip 1)
        if (!$keys.Count) { throw 'HD interface index is empty.' }
        foreach ($key in $keys) {
            if ($key -notmatch '^[0-9a-f]{16}$' -or !(Test-Path -LiteralPath (Join-Path $ui ($key+'.png')))) { throw 'HD interface cache is incomplete.' }
        }
        Copy-Item -LiteralPath $ui -Destination (Join-Path $stage 'ui') -Recurse
        if ($HdMedia -eq 'MenusAndMovies' -and $disc.mode -eq 'arcade') {
            New-Item -ItemType Directory -Path (Join-Path $stage 'movies') | Out-Null
            foreach ($id in $Movies) {
                if ($id -notin @(24,25,26)) { throw 'Only full-screen movies 24, 25 and 26 are supported.' }
                $packed = Join-Path $work "$id-conservative-v1.gtm"
                if (!(Test-Path -LiteralPath $packed)) {
                    $frames = Join-Path $work "movie-$id-original"
                    if (!(Test-Path -LiteralPath (Join-Path $frames 'movie.txt'))) { Run $media @('movie',$disc.path,"$id",$frames) }
                    $inputFrames = Join-Path $work "movie-$id-input"
                    New-Item -ItemType Directory -Force -Path $inputFrames | Out-Null
                    Get-ChildItem -LiteralPath $frames -Filter '*.png' | ForEach-Object {
                        $link = Join-Path $inputFrames $_.Name
                        if (!(Test-Path -LiteralPath $link)) {
                            try { New-Item -ItemType HardLink -Path $link -Target $_.FullName -ErrorAction Stop | Out-Null }
                            catch { Copy-Item -LiteralPath $_.FullName -Destination $link }
                        }
                    }
                    $enhanced = Join-Path $work "movie-$id-upscaled"
                    Upscale $inputFrames $enhanced
                    Copy-Item -LiteralPath (Join-Path $frames 'movie.txt'),(Join-Path $frames 'audio.pcm') -Destination $enhanced -Force
                    Run $media @('pack',$enhanced,($packed+'.candidate'),$frames)
                    Run $media @('inspect',($packed+'.candidate'))
                    Move-Item -LiteralPath ($packed+'.candidate') -Destination $packed
                }
                Run $media @('inspect',$packed)
                Copy-Item -LiteralPath $packed -Destination (Join-Path $stage "movies/$id.gtm")
            }
        }
        $fontPack = Join-Path $work 'fonts-contours-xbr-v1'
        if (!(Test-Path -LiteralPath (Join-Path $fontPack 'index.txt'))) {
            Run $media @('fonts',$disc.path,$fontPack)
        }
        Copy-Item -LiteralPath $fontPack -Destination (Join-Path $stage 'fonts') -Recurse
        $previousHd = Join-Path $Runtime "$($disc.mode)/hd"
        if ((Test-Path -LiteralPath (Join-Path $previousHd 'startup.gtm')) -and
            (Test-Path -LiteralPath (Join-Path $previousHd 'profile.txt')) -and
            (Get-Content -LiteralPath (Join-Path $previousHd 'profile.txt') -Raw).Trim() -eq $disc.profile) {
            Copy-Item -LiteralPath (Join-Path $previousHd 'startup.gtm') -Destination $stage
            if (Test-Path -LiteralPath (Join-Path $previousHd 'startup-hd.gtm')) { Copy-Item -LiteralPath (Join-Path $previousHd 'startup-hd.gtm') -Destination $stage }
        }
        Write-HdManifest $stage $disc.profile
        Publish $stage (Join-Path $Runtime "$($disc.mode)/hd")
    }
}
if ($Bios) {
    $Bios = (Resolve-Path -LiteralPath $Bios).Path
    if ((Get-Item -LiteralPath $Bios).Length -ne 524288) { throw 'Select a 512 KiB PS1 BIOS dump.' }
    if (!$CaptureCore) {
        # Pin a release archive: nightly/latest changes independently of this installer.
        $coreMember = 'RetroArch-Win64/cores/mednafen_psx_libretro.dll'
        $coreDir = Fetch 'https://buildbot.libretro.com/stable/1.22.2/windows/x86_64/RetroArch_cores.7z' '86B871E11B9B4772AC644B40A38F2C8E9449DA1F355EAE7DA08AA061148547B0' 'beetle-psx-retroarch-1.22.2' $coreMember
        $CaptureCore = Join-Path $coreDir $coreMember
    }
    $boot = Join-Path $Cache ('boot-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $boot | Out-Null
    # The core selects a regional filename. Actual firmware is always the user's dump.
    foreach ($name in @('scph5500.bin','scph5501.bin','scph5502.bin')) { Copy-Item -LiteralPath $Bios -Destination (Join-Path $boot $name) }
    $cue = Join-Path $boot 'disc.cue'
    [IO.File]::WriteAllText($cue,('FILE "'+$discs[0].path+'" BINARY' + "`n  TRACK 01 MODE2/2352`n    INDEX 01 00:00:00`n"),[Text.Encoding]::UTF8)
    $startup = Join-Path $boot 'startup.gtm'
    Run (Join-Path $BuildDir 'gt2bootcapture.exe') @($CaptureCore,$boot,$cue,$startup)
    Run $media @('inspect',$startup)
    foreach ($disc in $discs) {
        $hd = Join-Path $Runtime "$($disc.mode)/hd"
        New-Item -ItemType Directory -Force -Path $hd | Out-Null
        if ((Test-Path -LiteralPath (Join-Path $hd 'profile.txt')) -and (Get-Content -LiteralPath (Join-Path $hd 'profile.txt') -Raw).Trim() -ne $disc.profile) { throw 'Existing HD media belongs to a different disc profile.' }
        [IO.File]::WriteAllText((Join-Path $hd 'profile.txt'),$disc.profile,[Text.Encoding]::ASCII)
        Copy-Item -LiteralPath $startup -Destination (Join-Path $hd 'startup.gtm') -Force
        Write-HdManifest $hd $disc.profile
    }
    Publish $startup (Join-Path $Runtime 'startup.gtm')
}
$originalStartup = Join-Path $Runtime 'startup.gtm'
if ($HdMedia -ne 'Original' -and (Test-Path -LiteralPath $originalStartup)) {
    $bootHash = (Get-FileHash -LiteralPath $originalStartup -Algorithm SHA256).Hash.ToLowerInvariant()
    $bootWork = Join-Path $Cache ("startup-$bootHash-hd-v1")
    $packed = Join-Path $bootWork 'startup-hd.gtm'
    if (!(Test-Path -LiteralPath $packed)) {
        $frames = Join-Path $bootWork 'original'
        Run $media @('unpack',$originalStartup,$frames)
        $inputFrames = Join-Path $bootWork 'input'
        New-Item -ItemType Directory -Force -Path $inputFrames | Out-Null
        # BIOS holds still frames for seconds; process each distinct image only once.
        $unique = @{}
        $mapping = @{}
        foreach ($frame in (Get-ChildItem -LiteralPath $frames -Filter '*.png' | Sort-Object Name)) {
            $hash = (Get-FileHash -LiteralPath $frame.FullName -Algorithm SHA256).Hash
            if (!$unique.ContainsKey($hash)) {
                $unique[$hash] = $frame.Name
                Copy-Item -LiteralPath $frame.FullName -Destination $inputFrames -Force
            }
            $mapping[$frame.Name] = $unique[$hash]
        }
        $uniqueOutput = Join-Path $bootWork 'unique-upscaled'
        Upscale $inputFrames $uniqueOutput
        $enhanced = Join-Path $bootWork 'upscaled'
        New-Item -ItemType Directory -Force -Path $enhanced | Out-Null
        foreach ($name in $mapping.Keys) {
            $source = Join-Path $uniqueOutput $mapping[$name]
            $target = Join-Path $enhanced $name
            if (!(Test-Path -LiteralPath $target)) {
                try { New-Item -ItemType HardLink -Path $target -Target $source -ErrorAction Stop | Out-Null }
                catch { Copy-Item -LiteralPath $source -Destination $target }
            }
        }
        Copy-Item -LiteralPath (Join-Path $frames 'movie.txt'),(Join-Path $frames 'audio.pcm') -Destination $enhanced -Force
        Run $media @('pack',$enhanced,($packed+'.candidate'))
        Run $media @('inspect',($packed+'.candidate'))
        Move-Item -LiteralPath ($packed+'.candidate') -Destination $packed
    }
    Run $media @('inspect',$packed)
    foreach ($disc in $discs) {
        $hd = Join-Path $Runtime "$($disc.mode)/hd"
        Copy-Item -LiteralPath $packed -Destination (Join-Path $hd 'startup-hd.gtm') -Force
        Write-HdManifest $hd $disc.profile
    }
    $candidate = Join-Path $bootWork ('publish-' + [guid]::NewGuid().ToString('N') + '.gtm')
    Copy-Item -LiteralPath $packed -Destination $candidate
    Publish $candidate (Join-Path $Runtime 'startup-hd.gtm')
}
Write-Host 'Prepared media is ready. Original disc data and saves were retained. Intermediate files remain in the cache for inspection/resume.'
