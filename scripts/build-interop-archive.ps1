[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SourceDirectory,
    [Parameter(Mandatory = $true)]
    [string] $Archive
)

$ErrorActionPreference = 'Stop'
$interopSource = (Resolve-Path -LiteralPath $SourceDirectory).Path
$interopArchive = [IO.Path]::GetFullPath($Archive)
$requiredHelpers = @(
    'java/zsharp-java-worker.jar',
    'csharp/ZSharp.Interop.Worker.dll',
    'csharp/ZSharp.Interop.Worker.deps.json',
    'csharp/ZSharp.Interop.Worker.runtimeconfig.json',
    'csharp/ZSharp.Interop.dll',
    'python/zsharp_worker.py',
    'ruby/zsharp.rb',
    'ruby/zsharp_worker.rb',
    'go/go.mod',
    'go/zsharp.go',
    'include/zsharp_interop.h',
    'include/zsharp_c.h',
    'include/zsharp_cpp.h',
    'include/zsharp_rust.h',
    'include/zsharp_kotlin.h',
    'rust/zsharp_ffi.rs',
    'kotlin/zsharp_interop.def',
    'kotlin/README.md'
)
foreach ($helper in $requiredHelpers) {
    if (-not (Test-Path -LiteralPath (Join-Path $interopSource $helper) -PathType Leaf)) {
        throw "Required interop helper is missing: $helper"
    }
}
if ((Get-Item -LiteralPath $interopSource).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'The interop source directory must not be a symbolic link or junction'
}
$archiveParent = Split-Path -Parent $interopArchive
New-Item -ItemType Directory -Path $archiveParent -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression
$archiveStream = [IO.File]::Open($interopArchive, [IO.FileMode]::Create, [IO.FileAccess]::Write)
$zip = [IO.Compression.ZipArchive]::new($archiveStream, [IO.Compression.ZipArchiveMode]::Create)
try {
    $members = @(Get-ChildItem -LiteralPath $interopSource -Recurse -File | Sort-Object FullName)
    $archiveNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($member in $members) {
        if ($member.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Interop archives cannot contain links: $($member.Name)"
        }
        $relative = [IO.Path]::GetRelativePath($interopSource, $member.FullName).Replace('\', '/')
        if ($relative -notmatch '^[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$' -or
            ($relative.Split('/') | Where-Object { $_ -eq '.' -or $_ -eq '..' -or $_.EndsWith('.') })) {
            throw "Invalid interop archive member: $relative"
        }
        if (-not $archiveNames.Add($relative)) {
            throw "Case-colliding interop archive member: $relative"
        }
        $entry = $zip.CreateEntry('interop/' + $relative, [IO.Compression.CompressionLevel]::NoCompression)
        $destination = $entry.Open()
        $source = [IO.File]::OpenRead($member.FullName)
        try { $source.CopyTo($destination) }
        finally { $source.Dispose(); $destination.Dispose() }
    }
}
finally { $zip.Dispose(); $archiveStream.Dispose() }
$digest = (Get-FileHash -LiteralPath $interopArchive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($interopArchive + '.sha256', $digest + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))
Write-Host "Interop helper archive: $interopArchive ($($members.Count) files, SHA-256 $digest)"
