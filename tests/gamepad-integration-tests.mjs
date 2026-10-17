import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import assert from 'node:assert/strict';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-gamepad-'));
const exe = path.resolve('build/Release/zsharp.exe');
const settings = fs.readFileSync('examples/pcvr-test/project.zsettings', 'utf8')
  .replace('PID: "zsharp_pcvr_lab"', 'PID: "zsharp_gamepad_routing_test"')
  .replace('Project: "Z# PCVR Lab"', 'Project: "Z# Gamepad Routing Test"');
const source = `zsharp = type.script
noticed room Main[] (
 import ZSharp.Gamepad():
 noticed brain Start[] (
  Print("Gamepad runtime: " + Gamepad.connected + "," + Gamepad.name + "," + Gamepad.type + "," + Gamepad.south.label):
  Print("Gamepad axes: " + Gamepad.left.stick.x + "," + Gamepad.left.stick.y + "," + Gamepad.left.stick.angle + "," + Gamepad.left.stick.magnitude):
  Print("Gamepad edges: " + Gamepad.south.held + "," + Gamepad.south.pressed + "," + Gamepad.south.released + "," + Gamepad.right.trigger.value + "," + Gamepad.right.trigger.pressed):
  Print("Gamepad buttons: " + Gamepad.dpad.up.held + "," + Gamepad.left.stick.pressed + "," + Gamepad.north.pressed + "," + Gamepad.right.bumper.held):
  Gamepad.vibrate(20, 25ms):
 )
 noticed brain Shutdown[] ( Print("Gamepad shutdown completed"): )
)
`;
const compile = (text) => {
  fs.writeFileSync(path.join(root, 'Main.zsharp'), text);
  return spawnSync(exe, ['compile', path.join(root, 'Main.zsharp'), '-o', path.join(root, 'Main.zbc')],
    {encoding: 'utf8', timeout: 20000});
};
try {
  fs.writeFileSync(path.join(root, 'project.zsettings'), settings);
  for (const name of ['Cube.zobject', 'Prism.zmodel', 'BlueButton.zobject', 'CoralButton.zobject', 'Lab.zscene'])
    fs.copyFileSync(path.join('examples/pcvr-test', name), path.join(root, name));
  fs.cpSync('examples/pcvr-test/Assets', path.join(root, 'Assets'), {recursive: true});
  let result = compile(source);
  assert.equal(result.status, 0, result.stderr + result.stdout);
  result = spawnSync(exe, ['package', 'game', root, 'Gamepad-Routing'],
    {encoding: 'utf8', timeout: 30000});
  assert.equal(result.status, 0, result.stderr + result.stdout);
  result = spawnSync(exe, ['open', path.join(root, 'Packages', 'Gamepad-Routing.zgame')], {
    encoding: 'utf8', timeout: 30000,
    env: {...process.env, ZSHARP_XR_MODE: 'desktop', ZSHARP_GAMEPAD_TEST: '1',
      ZSHARP_GAME_AUTOCLOSE_MS: '500'},
  });
  assert.equal(result.status, 0, result.stderr + result.stdout);
  assert.match(result.stdout, /PASS: SDL gamepad hotplug, queued quick taps, independent cursors, sticks, focus, rumble and reconnect/);
  assert.match(result.stdout, /Gamepad runtime: alive,Z# Virtual Gamepad,xbox,A/);
  assert.match(result.stdout, /Gamepad axes: 1(?:\.0+)?,0(?:\.0+)?,90(?:\.0+)?,1(?:\.0+)?/);
  assert.match(result.stdout, /Gamepad edges: dead,dead,dead,0(?:\.0+)?,dead/);
  assert.match(result.stdout, /Gamepad buttons: dead,dead,dead,dead/);
  assert.match(result.stdout, /Gamepad shutdown completed/);
  for (const [bad, expected] of [
    [source.replace('import ZSharp.Gamepad():', ''), /requires import ZSharp.Gamepad/],
    [source.replace('Gamepad.connected', 'Gamepad.typo'), /unknown or read-only Gamepad/],
    [source.replace('Gamepad.vibrate(20, 25ms):', 'number.set:Gamepad.left.stick.x = 1:'), /unknown or read-only Gamepad/],
  ]) {
    result = compile(bad);
    assert.notEqual(result.status, 0, result.stderr + result.stdout);
    assert.match(result.stderr + result.stdout, expected);
  }
  console.log('PASS: Gamepad import, bytecode/property routing, physical labels, SDL virtual device and game lifecycle');
} finally {
  spawnSync(exe, ['hub', 'remove', 'zsharp_gamepad_routing_test'], {encoding: 'utf8', timeout: 15000});
  fs.rmSync(root, {recursive: true, force: true});
}
