param(
    [Parameter(Mandatory)][string]$AndroidSdk,
    [Parameter(Mandatory)][string]$JavaDirectory,
    [string]$Gradle = 'gradle',
    [string]$SigningDirectory = (Join-Path (Split-Path $PSScriptRoot) 'work/signing/release'),
    [switch]$InitializeSigningKey,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$SigningDirectory = [IO.Path]::GetFullPath($SigningDirectory)
$key = Join-Path $SigningDirectory 'gt2-release.jks'
$credentialFile = Join-Path $SigningDirectory 'credential.xml'
$env:JAVA_HOME = $JavaDirectory
$env:ANDROID_HOME = [IO.Path]::GetFullPath($AndroidSdk)
$keytool = Join-Path $JavaDirectory 'bin/keytool.exe'
$buildTools = Join-Path $AndroidSdk 'build-tools/35.0.0'
$signer = Join-Path $buildTools 'apksigner.bat'
$aligner = Join-Path $buildTools 'zipalign.exe'
foreach ($tool in @($keytool,$signer,$aligner)) { if (!(Test-Path -LiteralPath $tool)) { throw "Missing tool: $tool" } }
if (!(Test-Path -LiteralPath $key)) {
    if (!$InitializeSigningKey) { throw 'Release key not found. Use -InitializeSigningKey once for a new release identity.' }
    if (Test-Path -LiteralPath $credentialFile) { throw 'Credential exists without its key. Restore the key backup.' }
    New-Item -ItemType Directory -Force -Path $SigningDirectory | Out-Null
    $random = New-Object byte[] 32
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($random) } finally { $rng.Dispose() }
    $secret = [Convert]::ToBase64String($random)
    $secure = ConvertTo-SecureString $secret -AsPlainText -Force
    [pscredential]::new('gt2-release',$secure) | Export-Clixml -LiteralPath $credentialFile
    $env:GT2_SIGN_PASSWORD = $secret
    try {
        & $keytool -genkeypair -keystore $key -storetype JKS -alias gt2-release -keyalg RSA -keysize 4096 -validity 10000 -dname 'CN=GT2 VR Release' -storepass:env GT2_SIGN_PASSWORD -keypass:env GT2_SIGN_PASSWORD
        if ($LASTEXITCODE -ne 0) { throw 'Signing key creation failed.' }
    } finally { Remove-Item Env:GT2_SIGN_PASSWORD; $secret = $null }
}
if (!(Test-Path -LiteralPath $credentialFile)) { throw 'Missing signing credentials. Restore the matching credential backup.' }
[IO.File]::WriteAllText((Join-Path $repo 'android/local.properties'), 'sdk.dir=' + $env:ANDROID_HOME.Replace('\','/'), [Text.UTF8Encoding]::new($false))
$arguments = @('-p',(Join-Path $repo 'android'),'--no-daemon','assembleRelease')
if ($Offline) { $arguments += '--offline' }
& $Gradle @arguments
if ($LASTEXITCODE -ne 0) { throw 'Android release compilation failed.' }
$out = Join-Path $repo 'dist/GT2-VR-0.3.0.apk'
New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
$unsigned = Join-Path $repo 'android/app/build/outputs/apk/release/app-release-unsigned.apk'
$aligned = Join-Path $repo 'work/gt2-release-aligned.apk'
& $aligner -f -p 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'APK alignment failed.' }
$credential = Import-Clixml -LiteralPath $credentialFile
$env:GT2_SIGN_PASSWORD = $credential.GetNetworkCredential().Password
try {
    & $signer sign --ks $key --ks-key-alias gt2-release --ks-pass env:GT2_SIGN_PASSWORD --key-pass env:GT2_SIGN_PASSWORD --out $out $aligned
    if ($LASTEXITCODE -ne 0) { throw 'APK signing failed.' }
} finally { Remove-Item Env:GT2_SIGN_PASSWORD }
& $signer verify --verbose --print-certs $out
if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed.' }
& $aligner -c -p 4 $out
if ($LASTEXITCODE -ne 0) { throw 'Signed APK alignment failed.' }
Get-FileHash -LiteralPath $out -Algorithm SHA256
Write-Host "Keep a private backup of $SigningDirectory. Future public updates require this exact key. Credentials use Windows DPAPI for this account."
