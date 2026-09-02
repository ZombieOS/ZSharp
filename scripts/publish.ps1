[CmdletBinding()]
param(
    [string] $ProjectRoot = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$buildRoot = Join-Path $ProjectRoot "build"
$headerPath = Join-Path $ProjectRoot "native\include\zsharp.h"
$publishingBase = Join-Path `
    ([Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)) `
    "Downloads\ZSharp Publishing"
$toolsRoot = Join-Path $publishingBase "Tools"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)] [string] $File,
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [Parameter(Mandatory = $true)] [string] $Description,
        [string] $WorkingDirectory = $ProjectRoot
    )
    Write-Host "`n== $Description ==" -ForegroundColor Cyan
    Push-Location $WorkingDirectory
    try {
        & $File @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "$Description failed with exit code $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }
}

function Read-Version {
    $header = Get-Content -LiteralPath $headerPath -Raw
    $parts = foreach ($name in "MAJOR", "MINOR", "PATCH", "REVISION") {
        $match = [regex]::Match(
            $header,
            "#define\s+ZSHARP_VERSION_$name\s+([0-9]+)"
        )
        if (-not $match.Success) {
            throw "Could not read ZSHARP_VERSION_$name from $headerPath"
        }
        $match.Groups[1].Value
    }
    return $parts -join "."
}

function Assert-VersionText {
    param([string] $Path, [string] $Pattern, [string] $Description)
    $text = Get-Content -LiteralPath $Path -Raw
    if ($text -notmatch $Pattern) {
        throw "$Description does not match Z# $version"
    }
}

function Find-Tool {
    param([string] $Command, [string] $FallbackPattern)
    $found = Get-Command $Command -ErrorAction SilentlyContinue
    if ($null -ne $found) { return $found.Source }
    if ($Command -eq "cmake" -or $Command -eq "ctest") {
        $toolName = "$Command.exe"
        foreach ($edition in "Community", "BuildTools", "Professional", "Enterprise") {
            $candidate = Join-Path ${env:ProgramFiles} `
                "Microsoft Visual Studio\2022\$edition\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\$toolName"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }
    $fallback = Get-ChildItem -LiteralPath $toolsRoot -Filter $FallbackPattern `
        -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($null -eq $fallback) {
        throw "$Command was not found. Put it on PATH or in $toolsRoot"
    }
    return $fallback.FullName
}

function Write-HashSidecars {
    param([string] $Path)
    foreach ($algorithm in "MD5", "SHA1") {
        $hash = (Get-FileHash -LiteralPath $Path -Algorithm $algorithm).Hash
        [IO.File]::WriteAllText(
            "$Path.$($algorithm.ToLowerInvariant())",
            $hash.ToLowerInvariant(),
            [Text.Encoding]::ASCII
        )
    }
}

$version = Read-Version
$outRoot = Join-Path $publishingBase $version
$statusPath = Join-Path $outRoot "PUBLISHING-STATUS.txt"
New-Item -ItemType Directory -Path $outRoot -Force | Out-Null

Assert-VersionText -Path (Join-Path $ProjectRoot "CMakeLists.txt") `
    -Pattern "project\(zsharp VERSION $([regex]::Escape($version)) " `
    -Description "CMake version"
Assert-VersionText -Path (Join-Path $ProjectRoot "java\pom.xml") `
    -Pattern "<version>$([regex]::Escape($version))</version>" `
    -Description "Maven version"
Assert-VersionText -Path (Join-Path $ProjectRoot "java\build.gradle.kts") `
    -Pattern "version\s*=\s*`"$([regex]::Escape($version))`"" `
    -Description "Gradle version"
Assert-VersionText -Path (Join-Path $ProjectRoot "project.zsettings") `
    -Pattern "Version:\s*\[$([regex]::Escape($version))\]:" `
    -Description "Z# project version"
Assert-VersionText -Path (Join-Path $ProjectRoot "project.zsettings") `
    -Pattern "ZSharp:\s*\[$([regex]::Escape($version))\]:" `
    -Description "Z# language version"

$cmake = Find-Tool -Command "cmake" -FallbackPattern "cmake.exe"
$ctest = Find-Tool -Command "ctest" -FallbackPattern "ctest.exe"
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot "CMakeCache.txt"))) {
    Invoke-Checked -File $cmake -Arguments @(
        "-S", $ProjectRoot, "-B", $buildRoot,
        "-DCMAKE_BUILD_TYPE=Release", "-DZSHARP_BUILD_GAME_RUNTIME=ON"
    ) -Description "Configure Z#"
}
Invoke-Checked -File $cmake -Arguments @(
    "--build", $buildRoot, "--config", "Release", "--parallel"
) -Description "Build Z# $version"
Invoke-Checked -File $ctest -Arguments @(
    "--test-dir", $buildRoot, "-C", "Release", "--output-on-failure"
) -Description "Test Z# $version"

$runtimeCandidates = @(
    (Join-Path $buildRoot "Release\zsharp.exe"),
    (Join-Path $buildRoot "zsharp.exe"),
    (Join-Path $buildRoot "zsharp")
)
$runtime = $runtimeCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if ($null -eq $runtime) { throw "The built ZVM could not be found" }
$reported = (& $runtime --version | Out-String).Trim()
if ($reported -ne "Z# $version") {
    throw "The built runtime reported '$reported' instead of Z# $version"
}

if ($IsWindows -or $env:OS -eq "Windows_NT") {
    & (Join-Path $PSScriptRoot "stage-native-runtime.ps1") `
        -Platform windows-x86_64 -Runtime $runtime
    if ($LASTEXITCODE -ne 0) { throw "Could not stage windows-x86_64" }
}

$stageRoot = Join-Path $buildRoot "publish-stage-$version"
if (Test-Path -LiteralPath $stageRoot) {
    $resolvedStage = [IO.Path]::GetFullPath($stageRoot)
    $resolvedBuild = [IO.Path]::GetFullPath($buildRoot)
    if (-not $resolvedStage.StartsWith($resolvedBuild,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Publishing stage escaped the build directory"
    }
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
Invoke-Checked -File $cmake -Arguments @(
    "--install", $buildRoot, "--config", "Release", "--prefix", $stageRoot
) -Description "Stage the Windows distribution"
$nativeArchive = Join-Path $outRoot "zsharp-$version-Windows-AMD64.zip"
if (Test-Path -LiteralPath $nativeArchive) {
    Remove-Item -LiteralPath $nativeArchive -Force
}
Compress-Archive -Path (Join-Path $stageRoot "*") `
    -DestinationPath $nativeArchive -CompressionLevel Optimal

Invoke-Checked -File $runtime -Arguments @(
    "package", "game", (Join-Path $ProjectRoot "examples\test-game"),
    "ZSharpGameTest", "--unbytecode"
) -Description "Package the Z# test game"
$testGameOut = Join-Path $outRoot "Test Game"
New-Item -ItemType Directory -Path $testGameOut -Force | Out-Null
Copy-Item -LiteralPath `
    (Join-Path $ProjectRoot "examples\test-game\Packages\ZSharpGameTest.zgame") `
    -Destination $testGameOut -Force
Copy-Item -LiteralPath `
    (Join-Path $ProjectRoot "examples\test-game\Packages\ZSharpGameTest-unbytecoded.zgame") `
    -Destination $testGameOut -Force

$resourceRoot = Join-Path $ProjectRoot `
    "java\src\main\resources\META-INF\zsharp\runtime"
$runtimeDrop = Join-Path $outRoot "Native Runtimes"
$platforms = @(
    "windows-x86_64", "windows-aarch64",
    "linux-x86_64", "linux-aarch64",
    "macos-x86_64", "macos-aarch64"
)
foreach ($platform in $platforms) {
    $drop = Join-Path $runtimeDrop $platform
    $executableName = if ($platform.StartsWith("windows-")) {
        "zsharp.exe"
    } else { "zsharp" }
    $droppedRuntime = Join-Path $drop $executableName
    if (Test-Path -LiteralPath $droppedRuntime -PathType Leaf) {
        $support = @()
        $molten = Join-Path $drop "libMoltenVK.dylib"
        if (Test-Path -LiteralPath $molten -PathType Leaf) {
            $support += $molten
        }
        & (Join-Path $PSScriptRoot "stage-native-runtime.ps1") `
            -Platform $platform -Runtime $droppedRuntime -Support $support
        if ($LASTEXITCODE -ne 0) { throw "Could not stage $platform" }
    }
}

$missing = [Collections.Generic.List[string]]::new()
foreach ($platform in $platforms) {
    $platformRoot = Join-Path $resourceRoot $platform
    $versionFile = Join-Path $platformRoot "zsharp.version"
    $executableName = if ($platform.StartsWith("windows-")) {
        "zsharp.exe"
    } else { "zsharp" }
    $platformRuntime = Join-Path $platformRoot $executableName
    if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf) -or
        (Get-Content -LiteralPath $versionFile -Raw).Trim() -ne $version -or
        -not (Test-Path -LiteralPath $platformRuntime -PathType Leaf)) {
        $missing.Add($platform)
    }
}
if ($missing.Count -ne 0) {
    New-Item -ItemType Directory -Path $runtimeDrop -Force | Out-Null
    $status = @(
        "Z# $version publishing is not complete.",
        "",
        "Native Windows x86_64 files and test packages were built successfully.",
        "The following native game runtimes still need to be copied from the",
        "GitHub 'Build native game runtimes' workflow into:",
        "",
        "  $runtimeDrop\<platform>",
        "",
        "Missing: $($missing -join ', ')",
        "",
        "Extract each workflow artifact so its platform folder contains the",
        "zsharp executable (and libMoltenVK.dylib for macOS), then run",
        "'zsharp publish' again. Nothing was uploaded."
    ) -join [Environment]::NewLine
    [IO.File]::WriteAllText(
        $statusPath, $status + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false)
    )
    Write-Host "`nPublishing preparation folder: $outRoot" -ForegroundColor Yellow
    Write-Host "Missing current native runtimes: $($missing -join ', ')" `
        -ForegroundColor Yellow
    throw "Cross-platform runtime staging is incomplete; see $statusPath"
}

if (Test-Path -LiteralPath $statusPath) {
    Remove-Item -LiteralPath $statusPath -Force
}
$zig = Find-Tool -Command "zig" -FallbackPattern "zig.exe"
$testApp = Join-Path `
    ([Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)) `
    "Downloads\Z# Test App\Packages\ZSharp-Test-App.zapp"
$installerArguments = @(
    "-Zig", $zig, "-Version", $version, "-PublishingRoot", $outRoot
)
if (Test-Path -LiteralPath $testApp -PathType Leaf) {
    $installerArguments += @("-TestAppPackage", $testApp)
}
$buildInstallerArguments = @(
    "-NoProfile", "-File",
    (Join-Path $PSScriptRoot "build-installers.ps1")
) + $installerArguments
$hostShell = (Get-Process -Id $PID).Path
Invoke-Checked -File $hostShell -Arguments $buildInstallerArguments `
    -Description "Build installers and website update files"

$maven = Find-Tool -Command "mvn" -FallbackPattern "mvn.cmd"
Invoke-Checked -File $maven -Arguments @(
    "-f", (Join-Path $ProjectRoot "java\pom.xml"), "clean", "verify",
    "-Pcentral", "-Dcentral.skipPublishing=true"
) -Description "Build and sign Java/Maven artifacts"

$javaTarget = Join-Path $ProjectRoot "java\target"
$bundleStage = Join-Path $buildRoot "central-bundle-$version"
$bundlePath = Join-Path $bundleStage "com\zombieos\zsharp\$version"
if (Test-Path -LiteralPath $bundleStage) {
    Remove-Item -LiteralPath $bundleStage -Recurse -Force
}
New-Item -ItemType Directory -Path $bundlePath -Force | Out-Null
$centralNames = @(
    "zsharp-$version.jar",
    "zsharp-$version-sources.jar",
    "zsharp-$version-javadoc.jar",
    "zsharp-$version.pom"
)
foreach ($name in $centralNames) {
    $source = Join-Path $javaTarget $name
    $signature = "$source.asc"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
        -not (Test-Path -LiteralPath $signature -PathType Leaf)) {
        throw "Maven did not create signed artifact $name"
    }
    $target = Join-Path $bundlePath $name
    Copy-Item -LiteralPath $source, $signature -Destination $bundlePath -Force
    Write-HashSidecars -Path $target
}
$centralArchive = Join-Path $outRoot `
    "zsharp-java-$version-central-bundle.zip"
if (Test-Path -LiteralPath $centralArchive) {
    Remove-Item -LiteralPath $centralArchive -Force
}
Compress-Archive -Path (Join-Path $bundleStage "com") `
    -DestinationPath $centralArchive -CompressionLevel Optimal
Copy-Item -LiteralPath `
    (Join-Path $javaTarget "zsharp-$version.jar"),
    (Join-Path $javaTarget "zsharp-$version-sources.jar"),
    (Join-Path $javaTarget "zsharp-$version-javadoc.jar") `
    -Destination $outRoot -Force

$hashLines = Get-ChildItem -LiteralPath $outRoot -File -Recurse |
    Where-Object { $_.Name -ne "SHA256SUMS" } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($outRoot, $_.FullName) `
            -replace '\\', '/'
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        "$($hash.ToLowerInvariant())  $relative"
    }
[IO.File]::WriteAllLines(
    (Join-Path $outRoot "SHA256SUMS"), $hashLines, [Text.Encoding]::ASCII
)

Write-Host "`nZ# $version publishing files are ready:" -ForegroundColor Green
Write-Host $outRoot -ForegroundColor Green
Write-Host "Nothing was uploaded, tagged, pushed, or published."
