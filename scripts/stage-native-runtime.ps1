[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "windows-x86_64", "windows-aarch64",
        "linux-x86_64", "linux-aarch64",
        "macos-x86_64", "macos-aarch64"
    )]
    [string] $Platform,

    [Parameter(Mandatory = $true)]
    [string] $Runtime,

    [string[]] $Support = @()
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$resourceRoot = Join-Path $projectRoot `
    "java\src\main\resources\META-INF\zsharp\runtime"
$destination = Join-Path $resourceRoot $Platform
$resolvedRuntime = (Resolve-Path -LiteralPath $Runtime).Path
$expectedName = if ($Platform.StartsWith("windows-")) {
    "zsharp.exe"
} else {
    "zsharp"
}
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$versionOutput = (& $resolvedRuntime --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or
    $versionOutput -notmatch '^Z# ([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)$') {
    throw "The staged runtime did not report a valid four-part Z# version"
}
$runtimeVersion = $Matches[1]

New-Item -ItemType Directory -Path $destination -Force | Out-Null

function Copy-VerifiedRuntimeFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Source,

        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $target = Join-Path $destination $Name
    Copy-Item -LiteralPath $Source -Destination $target -Force
    $checksum =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText(
        $target + ".sha256",
        $checksum + [Environment]::NewLine,
        $utf8NoBom
    )
    Write-Host "$Platform/${Name}: $checksum"
}

Copy-VerifiedRuntimeFile -Source $resolvedRuntime -Name $expectedName
[System.IO.File]::WriteAllText(
    (Join-Path $destination "zsharp.version"),
    $runtimeVersion + [Environment]::NewLine,
    $utf8NoBom
)

foreach ($supportPath in $Support) {
    $resolvedSupport = (Resolve-Path -LiteralPath $supportPath).Path
    Copy-VerifiedRuntimeFile -Source $resolvedSupport `
        -Name ([IO.Path]::GetFileName($resolvedSupport))
}
