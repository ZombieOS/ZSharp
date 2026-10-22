// Actual-WASM regressions for inherited room-member visibility and local status.
// Run after rebuilding browser/dist: node browser/declaration-tests.mjs
import fs from 'node:fs';
import assert from 'node:assert/strict';

const binary = new WebAssembly.Module(fs.readFileSync(new URL('./dist/bzvm.wasm', import.meta.url)));
const encoder = new TextEncoder(), decoder = new TextDecoder();
const settings = (version = '1.4.0.0') => `zsharp = type.settings
Project: "Declaration Tests":
PID: "browser_decl":
Version: [1.0.0.0]:
Authors: ["Tester"]:
Description: "Declaration visibility and local status tests":
ZSharp: [${version}]:
Dependencies ():
`;
const script = body => 'zsharp = type.script\n' + body;
const local = body => script(`noticed room Main[] ( brain Start[] ( ${body} ) )`);

function harness() {
  let instance;
  const printed = [], timers = [], bindings = new Map(), writes = [];
  const read = pointer => {
    const bytes = new Uint8Array(instance.exports.memory.buffer); let end = pointer;
    while (bytes[end]) end++;
    return decoder.decode(bytes.subarray(pointer, end));
  };
  const put = text => {
    const bytes = encoder.encode(text), pointer = instance.exports.malloc(bytes.length + 1);
    assert.ok(pointer, 'WASM string allocation failed');
    const memory = new Uint8Array(instance.exports.memory.buffer);
    memory.set(bytes, pointer); memory[pointer + bytes.length] = 0;
    return pointer;
  };
  const wasi = {};
  for (const item of WebAssembly.Module.imports(binary)) {
    if (item.module === 'wasi_snapshot_preview1') wasi[item.name] = () => 0;
  }
  instance = new WebAssembly.Instance(binary, {bzvm: {
    now: () => performance.now(), input_session() {}, network: () => 0,
    wall_time: () => Date.now(), local_time: () => NaN, convert_local_time: () => NaN,
    read: () => -1,
    write: (path, value) => {writes.push([read(path), read(value)]); return 1;},
    bind: (id, target) => {bindings.set(read(id), read(target)); return 1;},
    schedule: (id, delay) => {timers.push({id, delay}); return 1;},
    raw: () => 1, print: pointer => printed.push(read(pointer))
  }, wasi_snapshot_preview1: wasi});
  instance.exports._initialize?.();
  const invoke = (method, ...values) => {
    const pointers = values.map(put);
    try {return instance.exports[method](...pointers);}
    finally {for (const pointer of pointers) instance.exports.free(pointer);}
  };
  const load = (source, name = 'Main') => invoke('bzvm_load', source, name);
  const error = () => read(instance.exports.bzvm_error());
  const resume = () => {
    const timer = timers.shift(); assert.ok(timer, 'Expected a suspended task');
    assert.equal(instance.exports.bzvm_resume(timer.id), 1, error()); return timer;
  };
  const close = () => instance.exports.bzvm_reset();
  return {instance, printed, timers, bindings, writes, invoke, load, error, resume, close};
}

function run(modules, {version = '1.4.0.0', configured = true, entries} = {}) {
  const result = harness();
  if (configured) assert.equal(result.invoke('bzvm_settings', settings(version)), 1, result.error());
  for (const [name, source] of modules) {
    if (!result.load(source, name)) return {...result, ok: 0, phase: 'load'};
  }
  if (entries) for (const index of entries) assert.equal(result.instance.exports.bzvm_entry(index), 1, result.error());
  return {...result, ok: result.instance.exports.bzvm_run(), phase: 'run'};
}
function succeeded(result, expected) {
  assert.equal(result.ok, 1, result.error());
  if (expected) assert.deepEqual(result.printed, expected);
}
function failed(result, pattern) {
  assert.equal(result.ok, 0, 'Expected a declaration/access/type failure');
  assert.match(result.error(), pattern);
}
const failures = [];
async function test(name, callback) {
  try {await callback(); console.log('PASS: ' + name);}
  catch (error) {failures.push(name); console.error('FAIL: ' + name + '\n' + error.stack);}
}

await test('unmarked fields and brain/number/text functions in a noticed room are public when imported', () => {
  const result = run([
    ['Main', script(`noticed room Main[] (
      import browser_decl.Library():
      brain Start[] (
        Print(Library.Shared.Title): Print(Library.Shared.Count): Print(Library.Shared.Ready):
        Function.call(Library:Shared:Speak):
        Print(Function.call(Library:Shared:Double [7])):
        Print(Function.call(Library:Shared:Greet ["Alex"])):
      )
    )`)],
    ['Library', script(`noticed room Shared[] (
      text Title = "public title": number Count = 3: status Ready = alive:
      brain Speak[] ( Print("public brain"): )
      number Double[number Value] ( feed(number.Value * 2): )
      text Greet[text Name] ( feed("Hello " + Name): )
    )`)]
  ]);
  succeeded(result, ['public title', '3', 'alive', 'public brain', '14', 'Hello Alex']);
  result.close();
});

await test('inherited public visibility still requires an explicit cross-file import', () => {
  const result = run([
    ['Main', local('Function.call(Library:Shared:Speak):')],
    ['Library', script('noticed room Shared[] ( brain Speak[] ( Print("must not run"): ) )')]
  ]);
  failed(result, /requires an import/i);
  assert.deepEqual(result.printed, []); result.close();
});

await test('explicit silent field overrides an otherwise public containing room', () => {
  const result = run([['Main', script(`noticed room Shared[] ( silent text Secret = "private": )
    noticed room Main[] ( brain Start[] ( Print(Shared.Secret): ) )`)]]);
  failed(result, /silent|not visible/i); result.close();
});

await test('explicit silent function overrides an otherwise public containing room', () => {
  const result = run([['Main', script(`noticed room Shared[] ( silent brain Hidden[] ( Print("must not run"): ) )
    noticed room Main[] ( brain Start[] ( Function.call(Main:Shared:Hidden): ) )`)]]);
  failed(result, /not visible|silent/i);
  assert.deepEqual(result.printed, []); result.close();
});

await test('same-room calls can still use explicitly private members', () => {
  const result = run([['Main', script(`noticed room Main[] (
    silent text Secret = "private but local":
    silent brain Hidden[] ( Print(Secret): )
    brain Start[] ( Function.call(Main:Main:Hidden): )
  )`)]]);
  succeeded(result, ['private but local']); result.close();
});

await test('unmarked file-only room fields and functions are usable by other same-file rooms', () => {
  const result = run([['Main', script(`room Helpers[] (
    text Title = "same file":
    brain Speak[] ( Print("same-file brain"): )
  )
  noticed room Main[] ( brain Start[] ( Print(Helpers.Title): Function.call(Main:Helpers:Speak): ) )`)]]);
  succeeded(result, ['same file', 'same-file brain']); result.close();
});

for (const explicit of ['', 'noticed ']) {
  await test((explicit ? 'explicit public' : 'inherited') + ' members do not bypass a file-only room across files', () => {
    const result = run([
      ['Main', script('noticed room Main[] ( import browser_decl.Library(): brain Start[] ( Function.call(Library:Helpers:Speak): ) )')],
      ['Library', script(`room Helpers[] ( ${explicit}brain Speak[] ( Print("must not run"): ) )`)]
    ]);
    failed(result, /not visible|silent/i); result.close();
  });
}

await test('unmarked members in a nested silent room remain private even to its parent', () => {
  const result = run([['Main', script(`noticed room Parent[] (
    brain Start[] ( Function.call(Main:Child:Hidden): )
    silent room Child[] ( brain Hidden[] ( Print("must not run"): ) )
  )`)]]);
  failed(result, /not visible|silent/i); result.close();
});

await test('unmarked fields in a nested silent room remain private even to its parent', () => {
  const result = run([['Main', script(`noticed room Parent[] (
    brain Start[] ( Print(Parent.Child.Secret): )
    silent room Child[] ( text Secret = "private": )
  )`)]]);
  failed(result, /silent|not visible/i); result.close();
});

await test('explicit noticed members in a reachable silent child override its private member default', () => {
  const result = run([['Main', script(`noticed room Parent[] (
    brain Start[] ( Print(Parent.Child.Exposed): Function.call(Main:Child:Speak): )
    silent room Child[] (
      noticed text Exposed = "explicit field":
      text Secret = "child internal":
      noticed brain Speak[] ( Print(Secret): )
    )
  )`)]]);
  succeeded(result, ['explicit field', 'child internal']); result.close();
});

await test('explicit noticed members do not bypass an unreachable top-level silent room', () => {
  const result = run([['Main', script(`silent room Hidden[] ( noticed brain Speak[] ( Print("must not run"): ) )
    noticed room Main[] ( brain Start[] ( Function.call(Main:Hidden:Speak): ) )`)]]);
  failed(result, /not visible|silent/i); result.close();
});

await test('unmarked Start auto-runs in noticed, file-only and silent rooms; DR still opts out', () => {
  const result = run([['Main', script(`noticed room Public[] ( brain Start[] ( Print("public Start"): ) )
    room FileOnly[] ( brain Start[] ( Print("file Start"): ) )
    silent room Private[] ( text Secret = "private Start": brain Start[] ( Print(Secret): ) )
    noticed room Disabled[] ( brain Start[DR] ( Print("must not auto-run"): ) )`)]]);
  succeeded(result, ['public Start', 'file Start', 'private Start']); result.close();
});

await test('local status accepts alive/dead and comparison expressions, and direct reassignment retains status semantics', () => {
  const result = run([['Main', local(`status Ready = alive: status Stopped = dead:
    number Count = 4: status Compared = Count >= 4:
    Print(Ready): Print(Stopped): Print(Compared):
    Ready = dead: Stopped = alive: Compared = Count < 4:
    Print(Ready): Print(Stopped): Print(Compared):
    if[Stopped] ( Print("conditional alive"): )
    if[Ready == dead] ( Print("conditional dead"): )`)]]);
  succeeded(result, ['alive', 'dead', 'alive', 'dead', 'alive', 'dead', 'conditional alive', 'conditional dead']);
  result.close();
});

await test('local status can shadow a room status without mutating the persistent field', () => {
  const result = run([['Main', script(`noticed room Main[] (
    status Ready = dead:
    brain Start[] (
      status Ready = alive: Print(Ready):
      Function.call(Main:Main:Report):
      Ready = dead: Print(Ready):
    )
    brain Report[] ( Print(Ready): )
  )`)]]);
  succeeded(result, ['alive', 'dead', 'dead']); result.close();
});

await test('local status retains its value across wait continuations in independent entry tasks', () => {
  const result = run([
    ['First', local('status Ready = alive: wait(2ms): Print(Ready): Ready = dead: Print(Ready):')],
    ['Second', local('status Ready = dead: wait(3ms): Print(Ready): Ready = alive: Print(Ready):')]
  ], {entries: [0, 1]});
  succeeded(result, []);
  assert.equal(result.timers.length, 2);
  assert.equal(result.resume().delay, 2);
  assert.equal(result.resume().delay, 3);
  assert.deepEqual(result.printed, ['alive', 'dead', 'dead', 'alive']);
  result.close();
});

await test('local status works as an ordinary typed function argument', () => {
  const result = run([['Main', script(`noticed room Main[] (
    brain Start[] ( status Ready = 3 > 2: Function.call(Main:Main:Report [Ready]): )
    brain Report[status Flag] ( Print(Flag): )
  )`)]]);
  succeeded(result, ['alive']); result.close();
});

await test('wrong-type reassignment warns and skips instead of changing a status local to text/number/null', () => {
  const result = run([['Main', local(`status Ready = alive:
    Ready = "dead": Print(Ready): Ready = 0: Print(Ready): Ready = null: Print(Ready):
    Ready = dead: Print(Ready): Ready = "alive": Print(Ready):`)]]);
  succeeded(result);
  const warnings = result.printed.filter(value => /^runtime warning:/i.test(value));
  assert.equal(warnings.length, 4);
  for (const warning of warnings) assert.match(warning, /status.*wrong type.*skip/i);
  assert.deepEqual(result.printed.filter(value => !/^runtime warning:/i.test(value)), ['alive', 'alive', 'alive', 'dead', 'dead']);
  result.close();
});

for (const expression of ['"alive"', '1', 'null']) {
  await test('local status rejects non-status initializer ' + expression + ' with a clear runtime error', () => {
    const result = run([['Main', local(`status Ready = ${expression}: Print("must not run"):`)]]);
    failed(result, /local status.*requires.*alive.*dead.*status/i);
    assert.deepEqual(result.printed, []); result.close();
  });
}

await test('local status requires an initializer', () => {
  const result = run([['Main', local('status Ready:')]]);
  failed(result, /expected.*=.*local status/i); result.close();
});

await test('a function local is not exposed to another function through inherited public visibility', () => {
  const result = run([['Main', script(`noticed room Main[] (
    brain Start[] ( status Ready = alive: Function.call(Main:Main:Other): )
    brain Other[] ( Print(Ready): )
  )`)]]);
  failed(result, /variable.*does not exist|no.*value|not.*defined/i); result.close();
});

await test('old browser project versions reject the new local-status opcode', () => {
  const result = run([['Main', local('status Ready = alive: Print(Ready):')]], {version: '1.3.0.0'});
  failed(result, /status.*1\.4\.0\.0|1\.4\.0\.0.*status/i);
  assert.equal(result.phase, 'load');
  assert.deepEqual(result.printed, []); result.close();
});

await test('unconfigured standalone scripts can use local status', () => {
  const result = run([['Main', local('status Ready = alive: Print(Ready):')]], {configured: false});
  succeeded(result, ['alive']); result.close();
});

assert.equal(failures.length, 0, failures.join('\n'));
console.log('PASS: all inherited-visibility/local-status browser regressions');
