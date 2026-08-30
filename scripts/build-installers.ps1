[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Zig,

    [string] $Version = "1.0.1.1",

    [string] $BaseUrl = "https://www.zsharp.zombieos.com",

    [string] $PublishingRoot = "",

    [string] $TestAppPackage = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$zigPath = (Resolve-Path -LiteralPath $Zig).Path
$source = Join-Path $projectRoot "installer\main.c"
$hashSource = Join-Path $projectRoot "native\src\hash.c"
$includeRoot = Join-Path $projectRoot "native\src"
$resourceRoot = Join-Path $projectRoot `
    "java\src\main\resources\META-INF\zsharp\runtime"
$buildRoot = Join-Path $projectRoot "build\installers"
$siteRoot = Join-Path $projectRoot "build\download-site"
$latestRoot = Join-Path $projectRoot "build\zvm-latest"
$downloadsRoot = Join-Path `
    ([Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)) `
    "Downloads"
if ([string]::IsNullOrWhiteSpace($PublishingRoot)) {
    $outRoot = Join-Path (Join-Path $downloadsRoot "ZSharp Publishing") `
        $Version
} else {
    $outRoot = [IO.Path]::GetFullPath($PublishingRoot)
}
if (-not $siteRoot.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $buildRoot.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $latestRoot.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Installer output paths escaped the Z# project"
}

if (Test-Path -LiteralPath $siteRoot) {
    Remove-Item -LiteralPath $siteRoot -Recurse -Force
}
if (Test-Path -LiteralPath $buildRoot) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}
if (Test-Path -LiteralPath $latestRoot) {
    Remove-Item -LiteralPath $latestRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $siteRoot, $buildRoot, $latestRoot, `
    $outRoot -Force |
    Out-Null

$targets = @(
    @{
        Id = "windows-x86_64"
        Triple = "x86_64-windows-gnu"
        Runtime = "zsharp.exe"
        Installer = "zsharp-installer.exe"
        Libraries = @("-lwinhttp", "-ladvapi32", "-luser32", "-lshell32")
    },
    @{
        Id = "windows-aarch64"
        Triple = "aarch64-windows-gnu"
        Runtime = "zsharp.exe"
        Installer = "zsharp-installer.exe"
        Libraries = @("-lwinhttp", "-ladvapi32", "-luser32", "-lshell32")
    },
    @{
        Id = "linux-x86_64"
        Triple = "x86_64-linux-gnu.2.17"
        Runtime = "zsharp"
        Installer = "zsharp-installer"
        Libraries = @()
    },
    @{
        Id = "linux-aarch64"
        Triple = "aarch64-linux-gnu.2.17"
        Runtime = "zsharp"
        Installer = "zsharp-installer"
        Libraries = @()
    },
    @{
        Id = "macos-x86_64"
        Triple = "x86_64-macos.10.15.0"
        Runtime = "zsharp"
        Installer = "zsharp-installer"
        Libraries = @()
    },
    @{
        Id = "macos-aarch64"
        Triple = "aarch64-macos.11.0.0"
        Runtime = "zsharp"
        Installer = "zsharp-installer"
        Libraries = @()
    }
)

$platforms = [ordered]@{}
$installerChecksums = [System.Collections.Generic.List[string]]::new()
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

foreach ($target in $targets) {
    Write-Host "Building installer for $($target.Id)"
    $targetBuild = Join-Path $buildRoot $target.Id
    $installerPath = Join-Path $targetBuild $target.Installer
    New-Item -ItemType Directory -Path $targetBuild -Force | Out-Null
    $arguments = @(
        "cc",
        "-target", $target.Triple,
        "-std=gnu17",
        "-O2",
        "-s",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-I$includeRoot",
        "-o", $installerPath,
        $source,
        $hashSource
    ) + $target.Libraries
    & $zigPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Zig failed to build the $($target.Id) installer"
    }

    $runtimeSource = Join-Path (Join-Path $resourceRoot $target.Id) `
        $target.Runtime
    $runtimeChecksumFile = $runtimeSource + ".sha256"
    $runtimeChecksum = (Get-Content -LiteralPath $runtimeChecksumFile -Raw).Trim()
    $actualRuntimeChecksum =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $runtimeSource).Hash.ToLowerInvariant()
    if ($runtimeChecksum -ne $actualRuntimeChecksum) {
        throw "The embedded $($target.Id) runtime checksum does not match"
    }

    $archiveRuntimeDirectory = Join-Path $latestRoot `
        "runtimes\$($target.Id)"
    $siteInstallerDirectory = Join-Path $siteRoot `
        "assets\download\installers\$Version\$($target.Id)"
    New-Item -ItemType Directory -Path $archiveRuntimeDirectory, `
        $siteInstallerDirectory -Force | Out-Null
    Copy-Item -LiteralPath $runtimeSource -Destination $archiveRuntimeDirectory
    Copy-Item -LiteralPath $installerPath -Destination $siteInstallerDirectory

    $platforms[$target.Id] = [ordered]@{
        path = "runtimes/$($target.Id)/$($target.Runtime)"
        sha256 = $runtimeChecksum
        size = (Get-Item -LiteralPath $runtimeSource).Length
    }

    $publicInstallerName = "zsharp-installer-$Version-$($target.Id)"
    if ($target.Installer.EndsWith(".exe")) {
        $publicInstallerName += ".exe"
    }
    $publicInstaller = Join-Path $outRoot $publicInstallerName
    Copy-Item -LiteralPath $installerPath -Destination $publicInstaller -Force
    $installerChecksum =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $publicInstaller).Hash.ToLowerInvariant()
    $installerChecksums.Add("$installerChecksum  $publicInstallerName")
}

$archiveDirectory = Join-Path $siteRoot "assets\download"
New-Item -ItemType Directory -Path $archiveDirectory -Force | Out-Null
$resolvedTestApp = $null
if (-not [string]::IsNullOrWhiteSpace($TestAppPackage)) {
    $resolvedTestApp = (Resolve-Path -LiteralPath $TestAppPackage).Path
} else {
    $localTestApp = Join-Path $downloadsRoot `
        "Z# Test App\Packages\ZSharp-Test-App.zapp"
    if (Test-Path -LiteralPath $localTestApp -PathType Leaf) {
        $resolvedTestApp = (Resolve-Path -LiteralPath $localTestApp).Path
    }
}
if ($null -ne $resolvedTestApp) {
    if (-not $resolvedTestApp.EndsWith(".zapp", `
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "The test app package must use the .zapp extension"
    }
    Copy-Item -LiteralPath $resolvedTestApp -Destination `
        (Join-Path $archiveDirectory "ZSharp-Test-App.zapp") -Force
} else {
    Write-Warning "No test app package was supplied; the download-site bundle will not include it"
}
$latestArchive = Join-Path $archiveDirectory "ZVM-LATEST.zip"
Push-Location $latestRoot
Compress-Archive -Path "runtimes" -DestinationPath $latestArchive `
    -CompressionLevel NoCompression
Pop-Location

$archiveChecksum =
    (Get-FileHash -Algorithm SHA256 -LiteralPath $latestArchive).Hash.ToLowerInvariant()
$archiveSize = (Get-Item -LiteralPath $latestArchive).Length
$archiveUrl = "$BaseUrl/assets/download/ZVM-LATEST.zip"
$manifest = [ordered]@{
    schema = 1
    latestVersion = $Version
    download = [ordered]@{
        url = $archiveUrl
        sha256 = $archiveChecksum
        size = $archiveSize
    }
    platforms = $platforms
}
$updateManifest = ($manifest | ConvertTo-Json -Depth 5) +
    [Environment]::NewLine
[System.IO.File]::WriteAllText(
    (Join-Path $outRoot "zsharp-update-$Version.js"),
    $updateManifest,
    $utf8NoBom
)
[System.IO.File]::WriteAllText(
    (Join-Path $siteRoot "update.js"),
    $updateManifest,
    $utf8NoBom
)
$exampleResponse = $manifest | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText(
    (Join-Path $buildRoot "update-response.json"),
    $exampleResponse + [Environment]::NewLine,
    $utf8NoBom
)
[System.IO.File]::WriteAllLines(
    (Join-Path $outRoot "zsharp-installer-$Version-SHA256SUMS.txt"),
    $installerChecksums,
    [System.Text.Encoding]::ASCII
)

$siteArchive = Join-Path $outRoot "zsharp-download-site-$Version.zip"
if (Test-Path -LiteralPath $siteArchive) {
    Remove-Item -LiteralPath $siteArchive -Force
}
Compress-Archive -Path (Join-Path $siteRoot "*") `
    -DestinationPath $siteArchive -CompressionLevel Optimal

Write-Host "Website upload bundle: $siteArchive"
Write-Host "Installer checksums: $(Join-Path $outRoot "zsharp-installer-$Version-SHA256SUMS.txt")"
Write-Host "Publishing folder: $outRoot"
