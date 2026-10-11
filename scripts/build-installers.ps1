[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Zig,

    [string] $Version = "1.2.0.0",

    [string] $BaseUrl = "https://www.zsharp.zombieos.com",

    [string] $ArchiveUrl = "",

    [string] $PublishingRoot = "",

    [string] $ReuseInstallersFrom = "",

    [string] $TestAppPackage = "",

    [string] $TestGamePackage = "",

    # Direct release preparation avoids duplicate website bundles on C:.
    [string] $WebsiteRoot = "",
    [string] $RuntimeRoot = "",
    [string] $WorkRoot = "",
    [string] $RuntimeArchivePath = "",

    [switch] $Beta
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$zigPath = (Resolve-Path -LiteralPath $Zig).Path
$source = Join-Path $projectRoot "installer\main.c"
$windowsResource = Join-Path $projectRoot "installer\windows.rc"
$installerIncludeRoot = Join-Path $projectRoot "installer"
$hashSource = Join-Path $projectRoot "native\src\hash.c"
$includeRoot = Join-Path $projectRoot "native\src"
$resourceRoot = Join-Path $projectRoot `
    "java\src\main\resources\META-INF\zsharp\runtime"
$buildRoot = Join-Path $projectRoot "build\installers"
$siteRoot = Join-Path $projectRoot "build\download-site"
$latestRoot = Join-Path $projectRoot "build\zvm-latest"
$directWebsite = -not [string]::IsNullOrWhiteSpace($WebsiteRoot)
if ($directWebsite) { $siteRoot = [IO.Path]::GetFullPath($WebsiteRoot) }
if ($RuntimeRoot) { $resourceRoot = (Resolve-Path -LiteralPath $RuntimeRoot).Path }
if ($WorkRoot) {
    $workDirectory = [IO.Path]::GetFullPath($WorkRoot)
    $buildRoot = Join-Path $workDirectory "installers"
    $latestRoot = Join-Path $workDirectory "zvm-latest"
}
$downloadsRoot = Join-Path `
    ([Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)) `
    "Downloads"
if ([string]::IsNullOrWhiteSpace($PublishingRoot)) {
    $outRoot = Join-Path (Join-Path $downloadsRoot "ZSharp Publishing") `
        $Version
} else {
    $outRoot = [IO.Path]::GetFullPath($PublishingRoot)
}
if (-not $directWebsite -and (-not $siteRoot.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $buildRoot.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not $latestRoot.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase))) {
    throw "Installer output paths escaped the Z# project"
}

if (-not $directWebsite -and (Test-Path -LiteralPath $siteRoot)) {
    Remove-Item -LiteralPath $siteRoot -Recurse -Force
}
if (-not $WorkRoot -and (Test-Path -LiteralPath $buildRoot)) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}
if (-not $WorkRoot -and (Test-Path -LiteralPath $latestRoot)) {
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
        Support = "libMoltenVK.dylib"
        Installer = "zsharp-installer"
        Libraries = @()
    },
    @{
        Id = "macos-aarch64"
        Triple = "aarch64-macos.11.0.0"
        Runtime = "zsharp"
        Support = "libMoltenVK.dylib"
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
    if ($Beta) {
        $arguments = @($arguments[0],
            "-DINSTALLER_UPDATE_ENDPOINT=`"https://www.zsharp.zombieos.com/beta.js?v=`"") +
            $arguments[1..($arguments.Count - 1)]
    }
    if ([string]::IsNullOrWhiteSpace($ReuseInstallersFrom)) {
        if ($target.Id.StartsWith("windows-")) {
            # Compile the same verified installer as a real GUI-subsystem app.
            # Automatic updates still pass CLI arguments to this executable.
            $mainObject = (Join-Path $targetBuild "main.obj").Replace('\', '/')
            $hashObject = (Join-Path $targetBuild "hash.obj").Replace('\', '/')
            $resourceFile = (Join-Path $targetBuild "windows.res").Replace('\', '/')
            $outputFile = $installerPath.Replace('\', '/')
            $guiDefines = @("-DZSHARP_INSTALLER_GUI=1")
            if ($Beta) {
                $guiDefines += '-DINSTALLER_UPDATE_ENDPOINT="https://www.zsharp.zombieos.com/beta.js?v="'
            }
            & $zigPath cc -target $target.Triple -std=gnu17 -O2 `
                -Wall -Wextra -Wpedantic "-I$includeRoot" `
                "-I$installerIncludeRoot" @guiDefines `
                -c $source -o $mainObject
            if ($LASTEXITCODE -ne 0) { throw "Could not compile $($target.Id) setup" }
            & $zigPath cc -target $target.Triple -std=gnu17 -O2 `
                "-I$includeRoot" -c $hashSource -o $hashObject
            if ($LASTEXITCODE -ne 0) { throw "Could not compile $($target.Id) hash" }
            & $zigPath rc $windowsResource $resourceFile
            if ($LASTEXITCODE -ne 0) { throw "Could not compile $($target.Id) resources" }
            & $zigPath build-exe -target $target.Triple -O ReleaseSmall -lc `
                --subsystem windows "-femit-bin=$outputFile" `
                $mainObject $hashObject $resourceFile `
                -lwinhttp -ladvapi32 -luser32 -lshell32 -lgdi32 -lcomctl32
        } else {
            & $zigPath @arguments
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Zig failed to build the $($target.Id) installer"
        }
    } else {
        $previousInstaller = Join-Path `
            (Join-Path $ReuseInstallersFrom $target.Id) $target.Installer
        if (-not (Test-Path -LiteralPath $previousInstaller -PathType Leaf)) {
            throw "Reusable installer not found: $previousInstaller"
        }
        Copy-Item -LiteralPath $previousInstaller -Destination $installerPath -Force
    }

    $runtimeSource = Join-Path (Join-Path $resourceRoot $target.Id) `
        $target.Runtime
    $runtimeVersionFile = Join-Path `
        (Join-Path $resourceRoot $target.Id) "zsharp.version"
    if (-not (Test-Path -LiteralPath $runtimeVersionFile -PathType Leaf) -or
        (Get-Content -LiteralPath $runtimeVersionFile -Raw).Trim() -ne $Version) {
        throw "The embedded $($target.Id) runtime is not staged for Z# $Version"
    }
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

    $platformMetadata = [ordered]@{
        path = "runtimes/$($target.Id)/$($target.Runtime)"
        sha256 = $runtimeChecksum
        size = (Get-Item -LiteralPath $runtimeSource).Length
    }
    $pythonSource = Join-Path (Join-Path $resourceRoot $target.Id) `
        "python-runtime.tar.gz"
    $pythonChecksumFile = $pythonSource + ".sha256"
    if (-not (Test-Path -LiteralPath $pythonSource -PathType Leaf) -or
        -not (Test-Path -LiteralPath $pythonChecksumFile -PathType Leaf)) {
        throw "The embedded $($target.Id) Python runtime is missing"
    }
    $pythonChecksum = (Get-Content -LiteralPath $pythonChecksumFile -Raw).Trim()
    $actualPythonChecksum =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $pythonSource).Hash.ToLowerInvariant()
    if ($pythonChecksum -ne $actualPythonChecksum) {
        throw "The embedded $($target.Id) Python runtime checksum does not match"
    }
    Copy-Item -LiteralPath $pythonSource -Destination $archiveRuntimeDirectory
    if (-not $directWebsite) {
        Copy-Item -LiteralPath $pythonSource, $pythonChecksumFile `
            -Destination $siteInstallerDirectory -Force
    }
    $platformMetadata.pythonPath =
        "runtimes/$($target.Id)/python-runtime.tar.gz"
    $platformMetadata.pythonSha256 = $pythonChecksum
    $platformMetadata.pythonSize = (Get-Item -LiteralPath $pythonSource).Length
    if ($target.ContainsKey("Support")) {
        $supportSource = Join-Path (Join-Path $resourceRoot $target.Id) `
            $target.Support
        $supportChecksumFile = $supportSource + ".sha256"
        if (-not (Test-Path -LiteralPath $supportSource -PathType Leaf) -or
            -not (Test-Path -LiteralPath $supportChecksumFile -PathType Leaf)) {
            throw "The embedded $($target.Id) game support files are missing"
        }
        $supportChecksum =
            (Get-Content -LiteralPath $supportChecksumFile -Raw).Trim()
        $actualSupportChecksum =
            (Get-FileHash -Algorithm SHA256 -LiteralPath $supportSource).Hash.ToLowerInvariant()
        if ($supportChecksum -ne $actualSupportChecksum) {
            throw "The embedded $($target.Id) game support checksum does not match"
        }
        Copy-Item -LiteralPath $supportSource `
            -Destination $archiveRuntimeDirectory
        $platformMetadata.supportPath =
            "runtimes/$($target.Id)/$($target.Support)"
        $platformMetadata.supportSha256 = $supportChecksum
        $platformMetadata.supportSize =
            (Get-Item -LiteralPath $supportSource).Length
    }
    $platforms[$target.Id] = $platformMetadata

    $publicInstallerName = "zsharp-installer-$Version-$($target.Id)"
    if ($target.Installer.EndsWith(".exe")) {
        $publicInstallerName += ".exe"
    }
    $publicInstaller = Join-Path $outRoot $publicInstallerName
    if ($directWebsite) { $publicInstaller = Join-Path $siteInstallerDirectory $target.Installer }
    else { Copy-Item -LiteralPath $installerPath -Destination $publicInstaller -Force }
    $installerChecksum =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $publicInstaller).Hash.ToLowerInvariant()
    $checksumName = if ($directWebsite) { "installers/$Version/$($target.Id)/$($target.Installer)" } else { $publicInstallerName }
    $installerChecksums.Add("$installerChecksum  $checksumName")
    if ($target.Id.StartsWith("macos-")) {
        # Finder opens this bundle without a Terminal window. Keep the raw
        # executable too: the ZVM uses it for unattended updates.
        $appName = "zsharp-setup-$Version-$($target.Id).app.zip"
        $appArchive = Join-Path $outRoot $appName
        if ($directWebsite) { $appArchive = Join-Path $siteInstallerDirectory $appName }
        $zip = [IO.Compression.ZipFile]::Open(
            $appArchive, [IO.Compression.ZipArchiveMode]::Create)
        try {
            $executable = $zip.CreateEntry(
                'ZSharp Setup.app/Contents/MacOS/zsharp-installer')
            $executable.ExternalAttributes = [int]((33261L -shl 16) - 4294967296L)
            $inputStream = [IO.File]::OpenRead($installerPath)
            $outputStream = $executable.Open()
            try { $inputStream.CopyTo($outputStream) }
            finally { $outputStream.Dispose(); $inputStream.Dispose() }
            $plistTemplate = Get-Content -LiteralPath `
                (Join-Path $projectRoot 'installer\macos-setup-Info.plist') -Raw
            $plist = $zip.CreateEntry(
                'ZSharp Setup.app/Contents/Info.plist')
            $plist.ExternalAttributes = [int]((33188L -shl 16) - 4294967296L)
            $writer = [IO.StreamWriter]::new($plist.Open(), $utf8NoBom)
            try {
                $macBundleVersion = (($Version -split '\.')[0..2] -join '.')
                $writer.Write($plistTemplate.Replace(
                    '@ZSHARP_VERSION@', $macBundleVersion))
            } finally { $writer.Dispose() }
        } finally { $zip.Dispose() }
        # ZipArchive writes Windows creator metadata even when POSIX mode bits
        # are present. Mark the central-directory entries as Unix so Finder's
        # unzip preserves the executable bit on the app binary.
        $zipBytes = [IO.File]::ReadAllBytes($appArchive)
        $directoryEnd = -1
        for ($position = $zipBytes.Length - 22;
             $position -ge [Math]::Max(0, $zipBytes.Length - 65557);
             $position--) {
            if ([BitConverter]::ToUInt32($zipBytes, $position) -eq 0x06054b50) {
                $directoryEnd = $position
                break
            }
        }
        if ($directoryEnd -lt 0) { throw "Invalid macOS setup archive" }
        $entryCount = [BitConverter]::ToUInt16($zipBytes, $directoryEnd + 10)
        $entryOffset = [int][BitConverter]::ToUInt32($zipBytes, $directoryEnd + 16)
        for ($entryIndex = 0; $entryIndex -lt $entryCount; $entryIndex++) {
            if ($entryOffset + 46 -gt $zipBytes.Length -or
                [BitConverter]::ToUInt32($zipBytes, $entryOffset) -ne 0x02014b50) {
                throw "Invalid macOS setup archive directory"
            }
            $zipBytes[$entryOffset + 5] = 3
            $entryOffset += 46 +
                [BitConverter]::ToUInt16($zipBytes, $entryOffset + 28) +
                [BitConverter]::ToUInt16($zipBytes, $entryOffset + 30) +
                [BitConverter]::ToUInt16($zipBytes, $entryOffset + 32)
        }
        [IO.File]::WriteAllBytes($appArchive, $zipBytes)
        if (-not $directWebsite) { Copy-Item -LiteralPath $appArchive -Destination $siteInstallerDirectory }
        $appChecksum = (Get-FileHash -Algorithm SHA256 -LiteralPath $appArchive).Hash.ToLowerInvariant()
        $checksumName = if ($directWebsite) { "installers/$Version/$($target.Id)/$appName" } else { $appName }
        $installerChecksums.Add("$appChecksum  $checksumName")
    }
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
$resolvedTestGame = $null
if (-not [string]::IsNullOrWhiteSpace($TestGamePackage)) {
    $resolvedTestGame = (Resolve-Path -LiteralPath $TestGamePackage).Path
} else {
    $localTestGame = Join-Path $projectRoot `
        "examples\test-game\Packages\ZSharpGameTest.zgame"
    if (Test-Path -LiteralPath $localTestGame -PathType Leaf) {
        $resolvedTestGame = (Resolve-Path -LiteralPath $localTestGame).Path
    }
}
if ($null -ne $resolvedTestGame) {
    if (-not $resolvedTestGame.EndsWith(".zgame", `
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "The test game package must use the .zgame extension"
    }
    Copy-Item -LiteralPath $resolvedTestGame -Destination `
        (Join-Path $archiveDirectory "ZSharpGameTest.zgame") -Force
} else {
    Write-Warning "No test game package was supplied; the download-site bundle will not include it"
}
$latestArchiveName = if ($Beta) { "ZVM-BETA.zip" } else { "ZVM-LATEST.zip" }
$latestArchive = Join-Path $archiveDirectory $latestArchiveName
if ($RuntimeArchivePath) {
    $latestArchive = [IO.Path]::GetFullPath($RuntimeArchivePath)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $latestArchive) | Out-Null
}
Push-Location $latestRoot
Compress-Archive -Path "runtimes" -DestinationPath $latestArchive `
    -CompressionLevel NoCompression
Pop-Location

$archiveChecksum =
    (Get-FileHash -Algorithm SHA256 -LiteralPath $latestArchive).Hash.ToLowerInvariant()
$archiveSize = (Get-Item -LiteralPath $latestArchive).Length
$archiveUrl = if ([string]::IsNullOrWhiteSpace($ArchiveUrl)) {
    "$BaseUrl/assets/download/$latestArchiveName"
} else {
    $ArchiveUrl
}
$manifest = [ordered]@{
    schema = 1
    latestVersion = $Version
    channel = $(if ($Beta) { "beta" } else { "stable" })
    download = [ordered]@{
        url = $archiveUrl
        sha256 = $archiveChecksum
        size = $archiveSize
    }
    platforms = $platforms
}
$updateManifest = ($manifest | ConvertTo-Json -Depth 5) +
    [Environment]::NewLine
if (-not $directWebsite) { [System.IO.File]::WriteAllText(
    (Join-Path $outRoot "zsharp-update-$Version.js"),
    $updateManifest,
    $utf8NoBom
) }
[System.IO.File]::WriteAllText(
    (Join-Path $siteRoot $(if ($Beta) { "beta.js" } else { "update.js" })),
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
if (-not $directWebsite -and (Test-Path -LiteralPath $siteArchive)) {
    Remove-Item -LiteralPath $siteArchive -Force
}
if (-not $directWebsite) {
    Compress-Archive -Path (Join-Path $siteRoot "*") `
        -DestinationPath $siteArchive -CompressionLevel Optimal
}

if ($directWebsite) { Write-Host "Website files: $siteRoot" }
else { Write-Host "Website upload bundle: $siteArchive" }
Write-Host "Runtime archive: $latestArchive"
Write-Host "Installer checksums: $(Join-Path $outRoot "zsharp-installer-$Version-SHA256SUMS.txt")"
Write-Host "Publishing folder: $outRoot"
