# Builds launcher programs only; release payload runtimes must be staged separately.
param([string]$OutputRoot = 'E:\ZSharp-Release-1.2.2.0')
$ErrorActionPreference = 'Stop'
$repo = 'D:\VS Code\Projects\Z#'
$zig = 'C:\Users\jj991\Downloads\ZSharp Publishing\Tools\zig-x86_64-windows-0.16.0\zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR = 'E:\ZSharp-Toolchains\zig-release-cache'
$targets = @{
 'windows-x86_64'='x86_64-windows-gnu'; 'windows-aarch64'='aarch64-windows-gnu';
 'linux-x86_64'='x86_64-linux-gnu.2.17'; 'linux-aarch64'='aarch64-linux-gnu.2.17';
 'macos-x86_64'='x86_64-macos.10.15.0'; 'macos-aarch64'='aarch64-macos.11.0.0'
}
foreach($id in ($targets.Keys | Sort-Object)) {
 $dest = Join-Path $OutputRoot "installers\$id"
 New-Item -ItemType Directory -Path $dest -Force | Out-Null
 $triple=$targets[$id]
 Write-Host "Building $id"
 if($id.StartsWith('windows')) {
  & $zig cc -target $triple -std=gnu17 -O2 "-I$repo\native\src" "-I$repo\installer" -DZSHARP_INSTALLER_GUI=1 -c "$repo\installer\main.c" -o "$dest\main.obj"
  if($LASTEXITCODE){throw "Compile failed: $id"}
  & $zig cc -target $triple -std=gnu17 -O2 "-I$repo\native\src" -c "$repo\native\src\hash.c" -o "$dest\hash.obj"
  if($LASTEXITCODE){throw "Hash compile failed: $id"}
  & $zig rc "$repo\installer\windows.rc" "$dest\windows.res"
  if($LASTEXITCODE){throw "Resources failed: $id"}
  & $zig build-exe -target $triple -O ReleaseSmall -lc --subsystem windows "-femit-bin=$dest\zsharp-installer.exe" "$dest\main.obj" "$dest\hash.obj" "$dest\windows.res" -lwinhttp -ladvapi32 -luser32 -lshell32 -lgdi32 -lcomctl32
 } else {
  & $zig cc -target $triple -std=gnu17 -O2 -s "-I$repo\native\src" -o "$dest\zsharp-installer" "$repo\installer\main.c" "$repo\native\src\hash.c"
 }
 if($LASTEXITCODE){throw "Link failed: $id"}
}
