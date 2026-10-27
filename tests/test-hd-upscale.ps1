$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path (Split-Path $PSScriptRoot) 'scripts/hd-upscale.ps1')
$root=Join-Path ([IO.Path]::GetTempPath()) ('gt2-hd-resume-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$root=[IO.Path]::GetFullPath($root)
try {
    $inputDir=Join-Path $root 'input with spaces'; $outputDir=Join-Path $root 'output'; $models=Join-Path $root 'models'
    New-Item -ItemType Directory -Path $inputDir,$outputDir,$models | Out-Null
    Set-Content (Join-Path $models 'realesrgan-x4plus.param') 'fixture'; Set-Content (Join-Path $models 'realesrgan-x4plus.bin') 'fixture'
    $exe=Join-Path $root 'fake upscaler.exe'
    Add-Type -ReferencedAssemblies System.Drawing -OutputAssembly $exe -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
class FakeUpscale {
    static int Main(string[] args) {
        string input=null, output=null;
        for(int i=0;i<args.Length-1;i++) { if(args[i]=="-i") input=args[i+1]; if(args[i]=="-o") output=args[i+1]; }
        int calls=File.Exists(Path.Combine(output,"calls.txt")) ? int.Parse(File.ReadAllText(Path.Combine(output,"calls.txt"))) : 0;
        File.WriteAllText(Path.Combine(output,"calls.txt"),(calls+1).ToString());
        foreach(string file in Directory.GetFiles(input,"*.png")) {
            using(var original=new Bitmap(file)) using(var enlarged=new Bitmap(original,original.Width*4,original.Height*4))
                enlarged.Save(Path.Combine(output,Path.GetFileName(file)),ImageFormat.Png);
            Console.Error.WriteLine("97.50%"); Console.Error.WriteLine("0.00%");
            if(File.Exists(Path.Combine(output,"fail")) && calls==1) return 7;
        }
        return 0;
    }
}
'@
    Add-Type -AssemblyName System.Drawing
    for($i=0;$i -lt 5;++$i) {
        $bitmap=New-Object Drawing.Bitmap 4,3
        try { $bitmap.Save((Join-Path $inputDir "$i.png"),[Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }
    }
    function Run-Fixture { Invoke-HdUpscale -Exe $exe -Models $models -InputDirectory $inputDir -OutputDirectory $outputDir -Label 'Test menus' -BatchSize 2 }
    Set-Content (Join-Path $outputDir 'fail') 'fail second batch'
    $failed=$false
    try { Run-Fixture } catch { if($_ -notmatch 'Upscaler failed \(7\)') { throw }; $failed=$true }
    if(!$failed) { throw 'Expected failure was lost.' }
    $checkpoint=Get-Content (Join-Path $outputDir 'upscale-resume.json') -Raw | ConvertFrom-Json
    if(@($checkpoint.files).Count -ne 2) { throw 'Only the completed batch may be cached.' }
    Remove-Item -LiteralPath (Join-Path $outputDir 'fail')
    Run-Fixture
    if((Get-Content (Join-Path $outputDir 'calls.txt')) -ne '4') { throw 'Resume did not skip completed batch.' }
    Run-Fixture
    if((Get-Content (Join-Path $outputDir 'calls.txt')) -ne '4') { throw 'Complete group was processed again.' }
    [IO.File]::WriteAllText((Join-Path $outputDir '0.png'),'corrupt')
    Run-Fixture
    if((Get-Content (Join-Path $outputDir 'calls.txt')) -ne '5') { throw 'Corrupted output was not rebuilt.' }
    Set-Content (Join-Path $models 'realesrgan-x4plus.param') 'changed model'
    Run-Fixture
    if((Get-Content (Join-Path $outputDir 'calls.txt')) -ne '8') { throw 'Model change did not invalidate resume cache.' }
    $bad=Join-Path $root 'bad.png'; [IO.File]::WriteAllBytes($bad,[byte[]](0..50))
    $rejected=$false; try { Get-HdPngSize $bad | Out-Null } catch { $rejected=$true }
    if(!$rejected) { throw 'Bad PNG accepted.' }
    if(@(Get-ChildItem -LiteralPath $outputDir -Directory).Count) { throw 'Temporary batch inputs leaked.' }
    Write-Host 'HD progress/resume tests passed: failure, restart, cached success, corrupt output, changed model, invalid PNG, batch cleanup.'
} finally {
    $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if($root.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -and (Split-Path $root -Leaf) -like 'gt2-hd-resume-*') { Remove-Item -LiteralPath $root -Recurse -Force }
}
