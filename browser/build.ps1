param([Parameter(Mandatory=$true)][string]$Zig)
$ErrorActionPreference = 'Stop'
$browserRoot = $PSScriptRoot
$repoRoot = Split-Path $browserRoot -Parent
New-Item -ItemType Directory -Force -Path "$browserRoot/dist" | Out-Null
$sources = @('browser/runtime.c', 'native/src/settings.c', 'native/src/lexer.c', 'native/src/parser.c', 'native/src/bytecode.c', 'native/src/decimal.c', 'native/src/hash.c') | ForEach-Object { Join-Path $repoRoot $_ }
$quickjsRoot = Join-Path $repoRoot 'build/_deps/quickjs-src'
if (!(Test-Path -LiteralPath "$quickjsRoot/quickjs.c")) { throw 'Configure the native CMake build first to obtain its pinned QuickJS sources.' }
$sources += @('dtoa.c','libregexp.c','libunicode.c','quickjs.c') | ForEach-Object { Join-Path $quickjsRoot $_ }
& $Zig cc -target wasm32-wasi -O2 -D_GNU_SOURCE -mexec-model=reactor -I "$repoRoot/native/src" -I "$repoRoot/native/include" -I $quickjsRoot @sources '-Wl,-z,stack-size=2097152' '-Wl,--export=bzvm_reset' '-Wl,--export=bzvm_load' '-Wl,--export=bzvm_import_count' '-Wl,--export=bzvm_import' '-Wl,--export=bzvm_run' '-Wl,--export=bzvm_settings' '-Wl,--export=bzvm_project_id' '-Wl,--export=bzvm_project_name' '-Wl,--export=bzvm_start' '-Wl,--export=bzvm_call' '-Wl,--export=bzvm_resume' '-Wl,--export=bzvm_cancel' '-Wl,--export=bzvm_error' '-Wl,--export=malloc' '-Wl,--export=free' '-Wl,--export-memory' -o "$browserRoot/dist/bzvm.wasm"
if ($LASTEXITCODE) { throw 'bZVM WebAssembly build failed.' }
Copy-Item -LiteralPath "$browserRoot/bzvm.js" -Destination "$browserRoot/dist/bzvm.js" -Force
Copy-Item -LiteralPath "$quickjsRoot/LICENSE" -Destination "$browserRoot/dist/QUICKJS-LICENSE.txt" -Force
