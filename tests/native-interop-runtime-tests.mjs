import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const executable = path.resolve(process.argv[2] || path.join(root, 'build/Release/zsharp.exe'));
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-native-interop-'));
const suffix = process.platform === 'win32' ? '.dll' : process.platform === 'darwin' ? '.dylib' : '.so';
const localZig = process.platform === 'win32'
  ? path.join(process.env.USERPROFILE || '', 'Downloads/ZSharp Publishing/Tools/zig-x86_64-windows-0.16.0/zig.exe') : '';
const zig = process.argv[3] || process.env.ZSHARP_TEST_ZIG || (fs.existsSync(localZig) ? localZig : null);
const environment = { ...process.env };
let checks = 0;

function command(program, arguments_, timeout = 90000) {
  const result = spawnSync(program, arguments_, { cwd: temporary, encoding: 'utf8', timeout,
    windowsHide: true, env: environment });
  assert.equal(result.error, undefined, String(result.error));
  assert.equal(result.status, 0, result.stdout + result.stderr);
  return result;
}
function buildC(source, output, cpp = false, definitions = []) {
  const program = zig || (cpp ? process.env.CXX || 'c++' : process.env.CC || 'cc');
  const args = [...(zig ? [cpp ? 'c++' : 'cc'] : []), cpp ? '-std=c++17' : '-std=c17',
    '-shared', ...(process.platform === 'win32' ? [] : ['-fPIC', '-pthread']),
    '-I' + path.join(root, 'native/include'), ...definitions, source, '-o', output];
  command(program, args);
}
function write(filename, text) { fs.writeFileSync(path.join(temporary, filename), text); }
function script(language, body, importModule = true) {
  return `zsharp = type.script
noticed room Main[] (
 ${importModule ? `import ${language}:native_fixture.Counter():` : ''}
 noticed number Callback[number Value] ( feed(Value + 1): )
 noticed brain Start[] (${body})
)
`;
}
function run(source, expected, failure) {
  write('Main.zsharp', source);
  const result = spawnSync(executable, ['run', path.join(temporary, 'Main.zsharp')], {
    cwd: temporary, encoding: 'utf8', timeout: 20000, windowsHide: true, env: environment });
  assert.equal(result.error, undefined, String(result.error));
  if (failure) {
    assert.notEqual(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout + result.stderr, failure);
  } else {
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.deepEqual(result.stdout.trim().split(/\r?\n/), expected);
  }
  checks++;
}

try {
  write('project.zsettings', `zsharp = type.settings
Project: "Native interoperability runtime tests":
PID: "native_fixture":
Version: [1.4.0.0]:
Authors: ["ZSharp tests"]:
Description: "Owned isolated native bridge fixtures":
ZSharp: [1.4.0.0]:
Dependencies ():
`);
  const fixture = path.join(root, 'tests/native-interop-v2');
  buildC(path.join(fixture, 'fixture.c'), path.join(temporary, 'Counter.zc' + suffix));
  buildC(path.join(fixture, 'fixture.cpp'), path.join(temporary, 'Counter.zcpp' + suffix), true);
  buildC(path.join(fixture, 'fixture.c'), path.join(temporary, 'Counter.zkt' + suffix), false,
    ['-Dzsharp_c_call_v2=zsharp_kotlin_call_v2']);
  command('rustc', ['--edition', '2021', '--crate-type', 'cdylib', '-A', 'dead_code',
    path.join(fixture, 'fixture.rs'), '-o', path.join(temporary, 'Counter.zrust' + suffix)]);
  for (const extension of ['c', 'cpp', 'rs', 'kt']) {
    write('Counter.' + extension, '// The separately compiled owned test fixture is used at runtime.\n');
  }
  const exact = '123456789012345678901234567890.000000000000000000000001';
  for (const language of ['c', 'cpp', 'rust', 'kt']) {
    run(script(language, `
      object First = Foreign.new(${language}:Counter [10]):
      object Second = Foreign.new(${language}:Counter [70]):
      Print(First.value):
      First.set.value = 20:
      number.set:First.value = First.value + 5:
      Print(First.value):
      Print(Second.value):
      First.echo["method"]:
      object Numbers = Function.call(${language}:Counter:echo [${exact}, 3]):
      Print(Numbers.Length): Print(Numbers[0]): Print(Numbers[1]):
      Print(Function.call(${language}:Counter:callback [41])):
      Foreign.release(First): Foreign.release(Second):
    `), ['10', '25', '70', '2', exact, '3', '42']);
    run(script(language, `object First = Foreign.new(${language}:Counter [10]):
      object Alias = First: Foreign.release(First): Print(Alias.value):`), null, /released/i);
    run(script(language, `object First = Foreign.new(${language}:Counter [10]):
      Foreign.release(First): Foreign.release(First):`), null, /released/i);
    run(script(language, `Print(Function.call(${language}:Counter:callback [1])):`, false), null, /import/i);
    run(script(language, `object First = Foreign.new(${language}:Counter [10]): Print(First.missing):`), null, /member|field|operation/i);
  }
  // The kt case verifies the native Kotlin ABI route using a C adapter. It is
  // deliberately not evidence that a Kotlin/Native compiler ran on this PC.
  buildC(path.join(root, 'tests/c_project/C/Utilities.c'), path.join(temporary, 'Legacy.zc' + suffix));
  write('Legacy.c', '// Unchanged v1 fixture source is compiled separately.\n');
  run(`zsharp = type.script noticed room Main[] (import c:native_fixture.Legacy():
    noticed brain Start[] (
      Print(Function.call(c:Legacy:greeting ["Tester"])):
      Print(Function.call(c:Legacy:counter [])):
      Print(Function.call(c:Legacy:counter [])):
    ))`, ['Hello from C, Tester!', '1', '2']);
  run(script('c', 'Print(Function.call(c:Counter:legacy [])):'), null, /unknown exported C function/i);
  console.log(`PASS: ${checks} native v2 ZVM integration cases (C, C++, Rust; Kotlin ABI C adapter) plus v1 compatibility`);
} finally {
  // Only this process's fresh, validated fixture directory may be removed.
  assert.equal(path.dirname(temporary), path.resolve(os.tmpdir()));
  assert.ok(path.basename(temporary).startsWith('zsharp-native-interop-'));
  fs.rmSync(temporary, { recursive: true, force: true });
}
