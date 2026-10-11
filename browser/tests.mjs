import fs from 'node:fs';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const module = new WebAssembly.Module(fs.readFileSync(new URL('./dist/bzvm.wasm', import.meta.url)));
let instance;
const writes = [], timers = [], bindings = new Map(), printed = [];
const decode = pointer => {
  const bytes = new Uint8Array(instance.exports.memory.buffer);
  let end = pointer; while (bytes[end]) end++;
  return new TextDecoder().decode(bytes.subarray(pointer, end));
};
const put = text => {
  const bytes = Buffer.from(text + '\0'), pointer = instance.exports.malloc(bytes.length);
  new Uint8Array(instance.exports.memory.buffer).set(bytes, pointer); return pointer;
};
const wasi = {};
for (const item of WebAssembly.Module.imports(module)) {
  if (item.module === 'wasi_snapshot_preview1') wasi[item.name] = () => 0;
}
instance = new WebAssembly.Instance(module, {bzvm: {
  now: () => performance.now(),
  read: (path, output) => {
    assert.equal(decode(path), 'Browser.input.value');
    const bytes = Buffer.from('test\0');
    new Uint8Array(instance.exports.memory.buffer).set(bytes, output); return bytes.length - 1;
  },
  write: (path, value) => { writes.push([decode(path), decode(value)]); return 1; },
  bind: (id, target) => { bindings.set(decode(id), decode(target)); return 1; },
  schedule: (id, delay) => { timers.push({id, delay}); return 1; },
  raw: () => 1, print: pointer => printed.push(decode(pointer))
}, wasi_snapshot_preview1: wasi});
instance.exports._initialize?.();
const invoke = (method, ...args) => {
  const pointers = args.map(put);
  try { assert.equal(instance.exports[method](...pointers), 1, decode(instance.exports.bzvm_error())); }
  finally { pointers.forEach(pointer => instance.exports.free(pointer)); }
};
const settingsText = `zsharp = type.settings
Project: "Browser Test":
PID: "browser_test":
Version: [1.0.0.0]:
Authors: ["Tester"]:
Description: "Local website":
ZSharp: [1.2.2.0]:
Dependencies ():
`;
invoke('bzvm_settings', settingsText);
assert.equal(decode(instance.exports.bzvm_project_id()), 'browser_test');
assert.equal(decode(instance.exports.bzvm_project_name()), 'Browser Test');
for (const [source, expected] of [
  [settingsText.replace('PID: "browser_test":', ''), 'must define'],
  [settingsText.replace('Dependencies ():', 'Dependencies ( playfab:1.0.0.0: ):'), 'Store dependencies'],
  [settingsText.replace('[1.2.2.0]', '[1.2.3.0]'), 'newer bZVM']
]) {
  const pointer=put(source);
  try {assert.equal(instance.exports.bzvm_settings(pointer),0);assert.ok(decode(instance.exports.bzvm_error()).includes(expected));}
  finally {instance.exports.free(pointer);}
}
const resume = () => {
  const timer = timers.shift(); assert.ok(timer);
  assert.equal(instance.exports.bzvm_resume(timer.id), 1, decode(instance.exports.bzvm_error()));
  return timer;
};
invoke('bzvm_start', `zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (
  if[Browser.input.value != ""] (
   Browser.output.content.set: "A " + Browser.input.value + " " + 10 + " " + alive:
  ) else ( Browser.output.content.set: "wrong": )
  Browser.button.clicked: (
   text Message = "after":
   wait(5s):
   Browser.output.content.set: Message + " click":
  ):
  Browser.output.content.set: "registered":
  Function.call(Main:Main:Child):
  Browser.output.content.set: "parent resumed":
 )
 noticed brain Child[] (
  text Message = "child":
  wait(5ms):
  Browser.output.content.set: Message:
  wait(0ms):
  Browser.output.content.set: "child done":
 )
)`, 'Main');
assert.deepEqual(writes.map(item => item[1]), ['A test 10 alive', 'registered']);
assert.equal(timers[0].delay, 5);
assert.equal(bindings.size, 1);
// A separate click can run while startup is waiting; locals belong to each task.
invoke('bzvm_call', bindings.get('button'));
assert.equal(timers[1].delay, 5000);
assert.equal(resume().delay, 5);
assert.equal(writes.at(-1)[1], 'child');
assert.equal(resume().delay, 5000);
assert.equal(writes.at(-1)[1], 'after click');
assert.equal(resume().delay, 0);
assert.deepEqual(writes.slice(-2).map(item => item[1]), ['child done', 'parent resumed']);
// Cancellation invalidates suspended continuations.
invoke('bzvm_call', bindings.get('button'));
const cancelled = timers.shift(); instance.exports.bzvm_cancel();
assert.equal(instance.exports.bzvm_resume(cancelled.id), 0);
// Both branches, else-if, multiple mixed fields, and repeated loop waits.
writes.length = 0;
invoke('bzvm_start', `zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (
  if[1 == 2] ( Browser.output.content.set: "wrong": )
  else if[2 == 2] ( Browser.output.content.set: "else if": )
  else ( Browser.output.content.set: "wrong": )
  if[alive == dead] ( Print("wrong"): ) else ( Browser.output.content.set: "else": )
  number Count = 0:
  loop (
   Browser.output.content.set: "count " + Count:
   wait(1ms):
   number.set:Count = Count + 1:
  )
 )
)`, 'Main');
assert.deepEqual(writes.map(item => item[1]), ['else if', 'else', 'count 0']);
resume(); assert.equal(writes.at(-1)[1], 'count 1');
resume(); assert.equal(writes.at(-1)[1], 'count 2');
instance.exports.bzvm_cancel(); timers.length = 0;
// Invalid delays and tight loops fail without hanging or scheduling bad timers.
for (const [body, expected] of [
  ['wait(2147483648ms):', 'browser wait must be between'],
  ['loop ( Print("busy"): )', 'browser instruction budget exceeded']
]) {
  const sourcePointer=put(`zsharp = type.script noticed room Main[] ( noticed brain Start[] ( ${body} ) )`);
  const namePointer=put('Main');
  try {
    assert.equal(instance.exports.bzvm_start(sourcePointer,namePointer),0);
    assert.ok(decode(instance.exports.bzvm_error()).includes(expected));
    assert.equal(timers.length,0);
  } finally {instance.exports.free(sourcePointer);instance.exports.free(namePointer);}
}
console.log('PASS: if/else-if/else, mixed text fields, inline clicks, nested waits, concurrent tasks, loop continuations and cancellation');
// Imported scripts retain separate rooms/callback identities, including after waits.
instance.exports.bzvm_reset();writes.length=0;bindings.clear();
const load = (source,name) => {
  const s=put(source),n=put(name);
  try {const result=instance.exports.bzvm_load(s,n);assert.ok(result>0,decode(instance.exports.bzvm_error()));return result;}
  finally {instance.exports.free(s);instance.exports.free(n);}
};
load(`zsharp = type.script noticed room Main[] (
 import browser_test.Messages():
 noticed brain Start[] (
  Function.call(Messages:Messages:Show):
  Browser.output.content.set: "main after foreign wait":
  Browser.button.clicked: Function.call(Messages:Messages:Show):
 )
)`,'Main');
load(`zsharp = type.script noticed room Messages[] (
 import browser_test.Main():
 noticed brain Start[] ( Browser.output.content.set: "must not auto-run": )
 noticed brain Show[] (
  text Message = "from Messages":
  wait(2ms):
  Browser.output.content.set: Message:
 )
)`,'Messages');
assert.equal(instance.exports.bzvm_import_count(0),1);
assert.equal(decode(instance.exports.bzvm_import(0,0)),'browser_test.Messages');
assert.equal(instance.exports.bzvm_run(),1,decode(instance.exports.bzvm_error()));
assert.equal(writes.length,0);resume();
assert.deepEqual(writes.map(item=>item[1]),['from Messages','main after foreign wait']);
invoke('bzvm_call',bindings.get('button'));resume();
assert.equal(writes.at(-1)[1],'from Messages');
// A loaded file is not automatically authorized in every calling room.
instance.exports.bzvm_reset();
load('zsharp = type.script noticed room Main[] ( noticed brain Start[] ( Function.call(Messages:Messages:Show): ) )','Main');
load('zsharp = type.script noticed room Messages[] ( noticed brain Show[] () )','Messages');
assert.equal(instance.exports.bzvm_run(),0);
assert.ok(decode(instance.exports.bzvm_error()).includes('requires an import'));
// Duplicate filenames cannot silently resolve to an arbitrary script.
const duplicate=put('zsharp = type.script noticed room Main[] ()'),duplicateName=put('Messages');
assert.equal(instance.exports.bzvm_load(duplicate,duplicateName),0);
assert.ok(decode(instance.exports.bzvm_error()).includes('duplicate script filenames'));
instance.exports.free(duplicate);instance.exports.free(duplicateName);
console.log('PASS: multi-file imports, cross-file functions/clicks, nested cross-file waits, entry-only startup, import authorization and duplicate detection');
printed.length=0;
invoke('bzvm_start',fs.readFileSync(new URL('./fixtures/Core.zsharp',import.meta.url),'utf8'),'Core');
assert.deepEqual(printed,['30','Hello, Alex','2','updated','inactive','7','changed','25','2','1000','9','4','0.3','100000000000000000002','1','numeric condition','alive','12','2','a# b#']);
if(process.argv.includes('--native')){
  const native=spawnSync(fileURLToPath(new URL('../build/Release/zsharp.exe',import.meta.url)),['run',fileURLToPath(new URL('./fixtures/Core.zsharp',import.meta.url))],{encoding:'utf8'});
  assert.equal(native.status,0,native.stderr);assert.deepEqual(native.stdout.trim().split(/\r?\n/),printed);
  console.log('PASS: identical output from desktop ZVM and bZVM for the shared language fixture');
}
invoke('bzvm_call','Main:Increment');invoke('bzvm_call','Main:Increment');invoke('bzvm_call','Main:Report');
assert.equal(printed.at(-1),'4');
console.log('PASS: typed parameters, returns, persistent state, callback parameters, arrays, Math, Regex and exact decimals');
// Returned values survive both nested calls and task suspension.
printed.length=0;timers.length=0;
invoke('bzvm_start',`zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (
  number Answer = Function.call(WaitReturn:Main:Outer [7]):
  Print("answer " + Answer):
 )
 noticed number Outer[number N] (
  number Inner = Function.call(WaitReturn:Main:Inner [N]):
  feed(Inner + 1):
 )
 noticed number Inner[number N] (
  wait(5ms):
  feed(N * 2):
 )
)`,'WaitReturn');
assert.equal(printed.length,0);resume();assert.deepEqual(printed,['answer 15']);
console.log('PASS: typed parameters and nested returned values across waits');
