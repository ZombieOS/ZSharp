param(
    [string]$SdkRoot = 'E:\Android\Sdk',
    [string]$BuildRoot = 'E:\Android\build\zsharp-port-test'
)

$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolchain = Join-Path $SdkRoot 'ndk\27.0.12077973\build\cmake\android.toolchain.cmake'
$cmake = Join-Path $SdkRoot 'cmake\3.22.1\bin\cmake.exe'
$runtimeCmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $SdkRoot 'cmake\3.22.1\bin\ninja.exe'
$aapt2 = Join-Path $SdkRoot 'build-tools\35.0.0\aapt2.exe'
$d8 = Join-Path $SdkRoot 'build-tools\35.0.0\d8.bat'
$zipalign = Join-Path $SdkRoot 'build-tools\35.0.0\zipalign.exe'
$apksigner = Join-Path $SdkRoot 'build-tools\35.0.0\apksigner.bat'
$androidJar = Join-Path $SdkRoot 'platforms\android-35\android.jar'
$jdkBin = 'C:\Program Files\Java\jdk-21\bin'
$javac = Join-Path $jdkBin 'javac.exe'
$jar = Join-Path $jdkBin 'jar.exe'
$keytool = Join-Path $jdkBin 'keytool.exe'
$keystore = Join-Path $BuildRoot 'port-test-debug.keystore'
$nativeBuild = Join-Path $BuildRoot 'native'
$runtimeBuild = Join-Path $BuildRoot 'runtime'
$classes = Join-Path $BuildRoot 'classes'
$dex = Join-Path $BuildRoot 'dex'
$staging = Join-Path $BuildRoot 'staging'
$baseApk = Join-Path $BuildRoot 'base.apk'
$alignedApk = Join-Path $BuildRoot 'aligned.apk'
$outputApk = Join-Path $BuildRoot 'ZSharp-Android-Port-Test.apk'

foreach ($tool in @($toolchain, $cmake, $runtimeCmake, $ninja, $aapt2, $d8,
                    $zipalign, $apksigner, $androidJar, $javac, $jar, $keytool)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Missing build tool: $tool" }
}

$env:ANDROID_HOME = $SdkRoot
$env:ANDROID_SDK_ROOT = $SdkRoot
$env:ANDROID_USER_HOME = Join-Path $BuildRoot 'android-user'
$env:JAVA_TOOL_OPTIONS = "-Djava.io.tmpdir=$(Join-Path $BuildRoot 'temp')"
$env:TEMP = Join-Path $BuildRoot 'temp'
$env:TMP = $env:TEMP
foreach ($dir in @($BuildRoot, $classes, $dex, $staging,
                   $env:ANDROID_USER_HOME, (Join-Path $BuildRoot 'temp'),
                   (Join-Path $staging 'lib\arm64-v8a'))) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

& $cmake -S (Join-Path $sourceRoot 'native') -B $nativeBuild -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-26'
if ($LASTEXITCODE -ne 0) { throw 'Native configure failed' }
& $cmake --build $nativeBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $sourceRoot '..\..')).Path
& $runtimeCmake -S $repositoryRoot -B $runtimeBuild -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-26' `
    '-DZSHARP_BUILD_GAME_RUNTIME=ON' '-DBUILD_TESTING=OFF'
if ($LASTEXITCODE -ne 0) { throw 'Android VM configure failed' }
& $runtimeCmake --build $runtimeBuild --target zsharp_android_vm --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Android VM build failed' }

$javaSources = @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'src') -Recurse -Filter '*.java' |
    ForEach-Object FullName)
$sdlJava = Join-Path $runtimeBuild '_deps\sdl3-src\android-project\app\src\main\java\org\libsdl\app'
if (-not (Test-Path -LiteralPath $sdlJava)) { throw 'SDL Android Java sources not found' }
$javaSources += @(Get-ChildItem -LiteralPath $sdlJava -Filter '*.java' |
    ForEach-Object FullName)
& $javac --release 8 -classpath $androidJar -d $classes @javaSources
if ($LASTEXITCODE -ne 0) { throw 'Java compile failed' }
$classFiles = @(Get-ChildItem -LiteralPath $classes -Recurse -Filter '*.class' | ForEach-Object FullName)
& $d8 --min-api 26 --lib $androidJar --output $dex @classFiles
if ($LASTEXITCODE -ne 0) { throw 'DEX conversion failed' }

& $aapt2 link -o $baseApk --manifest (Join-Path $sourceRoot 'AndroidManifest.xml') `
    -I $androidJar --min-sdk-version 26 --target-sdk-version 35
if ($LASTEXITCODE -ne 0) { throw 'APK resource linking failed' }
$nativeLibrary = Join-Path $nativeBuild 'libzsharp_port_test.so'
if (-not (Test-Path -LiteralPath $nativeLibrary)) { throw 'Native library not found' }
Copy-Item -LiteralPath $nativeLibrary -Destination (Join-Path $staging 'lib\arm64-v8a\libzsharp_port_test.so') -Force
$vmLibrary = Join-Path $runtimeBuild 'libzsharp_android_vm.so'
if (-not (Test-Path -LiteralPath $vmLibrary)) { throw 'Android VM library not found' }
Copy-Item -LiteralPath $vmLibrary -Destination (Join-Path $staging 'lib\arm64-v8a\libzsharp_android_vm.so') -Force
$sdlLibrary = Join-Path $runtimeBuild '_deps\sdl3-build\libSDL3.so'
if (-not (Test-Path -LiteralPath $sdlLibrary)) { throw 'SDL Android library not found' }
Copy-Item -LiteralPath $sdlLibrary -Destination (Join-Path $staging 'lib\arm64-v8a\libSDL3.so') -Force
Push-Location $staging
try {
    & $jar uf $baseApk -C $dex 'classes.dex' `
        'lib/arm64-v8a/libzsharp_port_test.so' 'lib/arm64-v8a/libzsharp_android_vm.so' `
        'lib/arm64-v8a/libSDL3.so'
    if ($LASTEXITCODE -ne 0) { throw 'APK assembly failed' }
} finally {
    Pop-Location
}

& $zipalign -f 4 $baseApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw 'APK alignment failed' }
if (-not (Test-Path -LiteralPath $keystore)) {
    & $keytool -genkeypair -keystore $keystore -alias androiddebugkey `
        -storepass android -keypass android -keyalg RSA -keysize 2048 `
        -validity 10000 -dname 'CN=ZSharp Android Port Test,O=ZombieOS,C=US'
    if ($LASTEXITCODE -ne 0) { throw 'Test signing key creation failed' }
}
& $apksigner sign --ks $keystore --ks-key-alias androiddebugkey `
    --ks-pass pass:android --key-pass pass:android --out $outputApk $alignedApk
if ($LASTEXITCODE -ne 0) { throw 'APK signing failed' }
& $apksigner verify --verbose $outputApk
if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed' }
Write-Output "Built test APK: $outputApk"
