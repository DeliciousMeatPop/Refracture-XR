# Builds the VrApi smoke test APK (arm64). -VrApiLoader is Meta's libvrapi.so, which is not part of Refract:
# take it from a VrApi game's APK (lib/arm64-v8a/libvrapi.so) or a Quest (/system_ext/lib64/libvrapi.so).
param(
    [Parameter(Mandatory = $true)][string]$VrApiLoader,
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$Jdk = "$env:ProgramFiles\Android\Android Studio\jbr",
    [string]$NdkVersion = '27.3.13750724',
    [string]$BuildToolsVersion = '36.1.0'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$build = "$root\build-vrapi-smoke"
$bt = "$Sdk\build-tools\$BuildToolsVersion"
$ndk = "$Sdk\ndk\$NdkVersion"
$clang = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe"
$glue = "$ndk\sources\android\native_app_glue"
$keystore = "$root\build-android-runtime-windows-x86_64\debug.keystore"
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE)" }
}
foreach ($required in @($VrApiLoader, $clang, "$bt\aapt2.exe", "$Jdk\bin\jar.exe", $keystore)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing prerequisite: $required" }
}
$lib = "$build\package\lib\arm64-v8a"
New-Item -ItemType Directory -Force $lib | Out-Null
Copy-Item -LiteralPath $VrApiLoader "$lib\libvrapi.so" -Force
Run $clang.Replace('clang++', 'clang') @('--target=aarch64-linux-android29', '-c', '-fPIC', '-O2', "$glue\android_native_app_glue.c", '-o', "$build\glue.o")
Run $clang @('--target=aarch64-linux-android29', '-std=c++17', '-shared', '-fPIC', '-O2', '-Wall', "-I$glue",
             "$PSScriptRoot\vrapi_smoke.cpp", "$build\glue.o", '-u', 'ANativeActivity_onCreate', "-L$lib", '-lvrapi',
             '-lEGL', '-lGLESv3', '-landroid', '-llog', '-static-libstdc++', '-o', "$lib\libvrapi_smoke.so")
Run "$bt\aapt2.exe" @('link', '-I', "$Sdk\platforms\android-29\android.jar", '--manifest', "$PSScriptRoot\AndroidManifest.xml", '-o', "$build\unsigned.apk")
Run "$Jdk\bin\jar.exe" @('uf', "$build\unsigned.apk", '-C', "$build\package", 'lib')
Run "$bt\zipalign.exe" @('-f', '-p', '4', "$build\unsigned.apk", "$build\aligned.apk")
$oldJavaHome = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $Jdk
    Run "$bt\apksigner.bat" @('sign', '--ks', $keystore, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', "$build\refract-vrapi-smoke.apk", "$build\aligned.apk")
} finally { $env:JAVA_HOME = $oldJavaHome }
Write-Host "Built $build\refract-vrapi-smoke.apk"
