[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Zig
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$zigPath = (Resolve-Path -LiteralPath $Zig).Path
$sourceRoot = Join-Path $projectRoot "native\src"
$includeRoot = Join-Path $projectRoot "native\include"
$outputRoot = Join-Path $projectRoot "build\embedded-runtimes"
$resourceRoot = Join-Path $projectRoot `
    "java\src\main\resources\META-INF\zsharp\runtime"
$sources = Get-ChildItem -LiteralPath $sourceRoot -Filter "*.c" -File |
    Sort-Object Name |
    ForEach-Object { $_.FullName }

$targets = @(
    @{
        Id = "windows-x86_64"
        Triple = "x86_64-windows-gnu"
        Executable = "zsharp.exe"
        Libraries = @()
    },
    @{
        Id = "windows-aarch64"
        Triple = "aarch64-windows-gnu"
        Executable = "zsharp.exe"
        Libraries = @()
    },
    @{
        Id = "linux-x86_64"
        Triple = "x86_64-linux-gnu.2.17"
        Executable = "zsharp"
        Libraries = @("-ldl")
    },
    @{
        Id = "linux-aarch64"
        Triple = "aarch64-linux-gnu.2.17"
        Executable = "zsharp"
        Libraries = @("-ldl")
    },
    @{
        Id = "macos-x86_64"
        Triple = "x86_64-macos.10.15.0"
        Executable = "zsharp"
        Libraries = @()
    },
    @{
        Id = "macos-aarch64"
        Triple = "aarch64-macos.11.0.0"
        Executable = "zsharp"
        Libraries = @()
    }
)

foreach ($target in $targets) {
    $outputDirectory = Join-Path $outputRoot $target.Id
    $resourceDirectory = Join-Path $resourceRoot $target.Id
    $output = Join-Path $outputDirectory $target.Executable
    $resource = Join-Path $resourceDirectory $target.Executable
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $resourceDirectory -Force | Out-Null

    Write-Host "Building $($target.Id)"
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
        "-o", $output
    ) + $sources + $target.Libraries

    & $zigPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Zig failed to build $($target.Id)"
    }

    Copy-Item -LiteralPath $output -Destination $resource -Force
    $checksum = (Get-FileHash -Algorithm SHA256 -LiteralPath $resource).Hash
    $checksum = $checksum.ToLowerInvariant()
    [System.IO.File]::WriteAllText(
        $resource + ".sha256",
        $checksum + [Environment]::NewLine,
        [System.Text.Encoding]::ASCII
    )
    Write-Host "$($target.Id): $checksum"
}
