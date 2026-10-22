// Isolated CLI-only fixtures; no GUI, network, user projects or installs.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';

const exe = path.resolve(process.argv[2] || 'build/Release/zsharp.exe');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-members-status-'));
const env = {...process.env, ZSHARP_SKIP_UPDATE_CHECK: '1', ZSHARP_SKIP_DESKTOP_INTEGRATION: '1'};
const write = (name, contents) => fs.writeFileSync(path.join(root, name), contents);
const run = (...args) => {
  const result = spawnSync(exe, args, {cwd: root, env, encoding: 'utf8', timeout: 15000});
  assert.equal(result.error, undefined, String(result.error));
  return {...result, combined: result.stdout + result.stderr};
};
let checks = 0;
const success = (label, result) => {assert.equal(result.status, 0, label + '\n' + result.combined); checks++; return result;};
const lines = result => result.stdout.trim().split(/\r?\n/).filter(Boolean);
const rejected = (label, result, expected) => {
  assert.notEqual(result.status, 0, label + ' unexpectedly succeeded\n' + result.combined);
  assert.match(result.combined, expected, label + '\n' + result.combined); checks++;
};
const settings = `zsharp = type.settings
Project: "Member Visibility and Status Tests": PID: "visibility_status_test":
Version: [1.4.0.0]: Authors: ["Z# Tests"]:
Description: "Isolated language regressions": ZSharp: [1.4.0.0]: Dependencies ():
`;
const target = `zsharp = type.script
noticed room Public[] (
 text Value = "cross-file inherited value":
 brain Speak[] (Print("cross-file inherited function"):)
 silent text Hidden = "private member":
 silent brain HiddenBrain[] (Print("must not run"):)
 noticed text Exposed = "explicit public value":
 noticed brain ExposedBrain[] (Print("explicit public function"):)
)
room FileOnly[] (
 text Value = "must not cross files":
 brain Speak[] (Print("must not run"):)
 noticed text Exposed = "still a file-only room":
 noticed brain ExposedBrain[] (Print("must not run"):)
)
silent room Private[] (
 text Value = "private room":
 brain Speak[] (Print("must not run"):)
 noticed text Exposed = "still a private room":
 noticed brain ExposedBrain[] (Print("must not run"):)
)
`;
const fixture = body => `zsharp = type.script
noticed room Main[] (
 import visibility_status_test.Targets():
 brain Start[] (${body})
)
`;
try {
  write('project.zsettings', settings);
  write('Targets.zsharp', target);
  write('Main.zsharp', `zsharp = type.script
noticed room Main[] (
 import visibility_status_test.Targets():
 status Ready = dead:
 brain Start[] (
  Print(Targets.Public.Value): Function.call(Targets:Public:Speak):
  Print(Targets.Public.Exposed): Function.call(Targets:Public:ExposedBrain):
  Print(FileOnly.Value): Function.call(Main:FileOnly:Speak):
  status Ready = alive: status Disabled = dead:
  status Calculated = (2 < 3) and not Disabled:
  Print(Ready): Print(Disabled): Print(Calculated): Print(Main.Ready):
  Ready = dead: Print(Ready):
  Ready = 3 == 3: if[Ready and Calculated] (Print("status condition"):)
  Function.call(Main:Main:Receive [Ready]):
  wait(1ms): Print(Ready):
 )
 brain Receive[status Value] (Print(Value):)
)
room FileOnly[] (
 text Value = "same-file inherited value":
 brain Speak[] (Print("same-file inherited function"):)
)
silent room PrivateSelf[] (
 text Value = "private own value":
 brain Start[] (Print(Value): Function.call(Main:PrivateSelf:Speak):)
 brain Speak[] (Print("private own function"):)
)
noticed room ExplicitSilentStart[] (
 silent brain Start[] (Print("explicit silent Start"):)
)
silent room DisabledStart[] (
 brain Start[DR] (Print("must not auto-start"):)
)
`);
  const expected = ['cross-file inherited value', 'cross-file inherited function',
    'explicit public value', 'explicit public function', 'same-file inherited value',
    'same-file inherited function', 'alive', 'dead', 'alive', 'dead', 'dead',
    'status condition', 'alive', 'alive', 'private own value', 'private own function', 'explicit silent Start'];
  assert.deepEqual(lines(success('positive source', run('run', 'Main.zsharp'))), expected);
  success('positive compilation', run('compile', 'Main.zsharp', '-o', 'Main.zbc'));
  assert.deepEqual(lines(success('positive bytecode', run('run-bytecode', 'Main.zbc'))), expected);

  // Room visibility still bounds an explicitly noticed member; silent members
  // remain private even when their enclosing room is noticed.
  for (const [label, body] of [
    ['private cross-file field', 'Print(Targets.Public.Hidden):'],
    ['private cross-file function', 'Function.call(Targets:Public:HiddenBrain):'],
    ['file-only cross-file field', 'Print(Targets.FileOnly.Value):'],
    ['file-only cross-file function', 'Function.call(Targets:FileOnly:Speak):'],
    ['noticed field in file-only room', 'Print(Targets.FileOnly.Exposed):'],
    ['noticed function in file-only room', 'Function.call(Targets:FileOnly:ExposedBrain):'],
    ['private room cross-file field', 'Print(Targets.Private.Exposed):'],
    ['private room cross-file function', 'Function.call(Targets:Private:ExposedBrain):']
  ]) {
    write('Negative.zsharp', fixture(body));
    rejected(label + ' source', run('run', 'Negative.zsharp'), /silent|not visible|visibility/i);
    rejected(label + ' compile', run('compile', 'Negative.zsharp', '-o', 'Negative.zbc'), /silent|not visible|visibility/i);
  }
  for (const [label, body] of [
    ['private same-file field', 'Print(Public.Hidden):'],
    ['private same-file function', 'Function.call(Negative:Public:HiddenBrain):'],
    ['private same-file room field', 'Print(Private.Exposed):'],
    ['private same-file room function', 'Function.call(Negative:Private:ExposedBrain):']
  ]) {
    write('Negative.zsharp', fixture(body) + target.slice(target.indexOf('noticed room Public')));
    rejected(label + ' source', run('run', 'Negative.zsharp'), /silent|not visible|visibility/i);
    rejected(label + ' compile', run('compile', 'Negative.zsharp', '-o', 'Negative.zbc'), /silent|not visible|visibility/i);
  }

  for (const value of ['"alive"', '1', 'null', '1 + 2']) {
    write('Negative.zsharp', fixture(`status Invalid = ${value}: Print("unreachable"):`));
    rejected('invalid local status initializer ' + value, run('run', 'Negative.zsharp'), /status.*(?:value|type)|requires.*status/i);
    success('invalid initializer serializes for runtime validation', run('compile', 'Negative.zsharp', '-o', 'Negative.zbc'));
    rejected('invalid initializer bytecode ' + value, run('run-bytecode', 'Negative.zbc'), /status.*(?:value|type)|requires.*status/i);
  }
  // Existing generic assignment semantics warn/skip mismatches; a failed write
  // must never change a local status into text/number/null.
  write('Negative.zsharp', fixture(`status Flag = alive:
    Flag = "dead": Print(Flag): Flag = 0: Print(Flag): Flag = null: Print(Flag):`));
  const reassigned = success('wrong-type reassignment source', run('run', 'Negative.zsharp'));
  assert.deepEqual(lines(reassigned), ['alive', 'alive', 'alive']);
  assert.match(reassigned.stderr, /warning|status|type/i);
  success('reassignment compilation', run('compile', 'Negative.zsharp', '-o', 'Negative.zbc'));
  const reassignedBytecode = success('wrong-type reassignment bytecode', run('run-bytecode', 'Negative.zbc'));
  assert.deepEqual(lines(reassignedBytecode), ['alive', 'alive', 'alive']);
  assert.match(reassignedBytecode.stderr, /warning|status|type/i);
  write('Negative.zsharp', fixture('status Flag = alive: Print(Flag):'));
  write('project.zsettings', settings.replace('ZSharp: [1.4.0.0]:', 'ZSharp: [1.3.0.0]:'));
  rejected('local status minimum version', run('compile', 'Negative.zsharp', '-o', 'OldStatus.zbc'), /require.*1\.4\.0\.0/i);
  write('project.zsettings', settings);
  console.log(`PASS: ${checks} inherited visibility, source/bytecode access, silent startup and typed local status regressions`);
} finally {
  fs.rmSync(root, {recursive: true, force: true});
}
