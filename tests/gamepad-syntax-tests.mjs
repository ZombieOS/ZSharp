import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-gamepad-syntax-'));
const exe = path.resolve('build/Release/zsharp.exe');
const settings = `zsharp = type.settings
Project: "Z# Gamepad Syntax Test":
PID: "zsharp_gamepad_syntax_test":
Version: [1.0.0.0]:
Authors: ["ZombieOS"]:
Description: "Gamepad syntax validation fixture.":
ZSharp: [1.3.0.0]:
Dependencies (
 zsharpgame:1.0.0.1
):
Window (
 StartScene: "Lab.zscene":
):
`;
const source = (body, imports = 'import ZSharp.Gamepad():') => `zsharp = type.script
noticed room Main[] (
 ${imports}
 noticed brain Start[] (
  ${body}
 )
)
`;
const compile = (text, config = settings) => {
    fs.writeFileSync(path.join(root, 'Main.zsharp'), text);
    fs.writeFileSync(path.join(root, 'project.zsettings'), config);
    return spawnSync(exe, [
        'compile', path.join(root, 'Main.zsharp'),
        '-o', path.join(root, 'Main.zbc'),
    ], { encoding: 'utf8', timeout: 15000 });
};
const diagnostic = (result) => result.stderr + result.stdout;

try {
    fs.copyFileSync('examples/pcvr-test/Cube.zobject', path.join(root, 'Cube.zobject'));
    fs.writeFileSync(path.join(root, 'Lab.zscene'), `zsharp = type.scene
noticed scene Lab[] (
 scene Lab (
  title: "Gamepad Syntax":
  background: #101522:
  gravityY: 0:
 )
 objects[JSON] (
  [{"id":"Cube","name":"Target","location":{"x":0,"y":0,"z":-2}}]
 )
)
`);

    const controls = [
        'south', 'east', 'west', 'north',
        'dpad.up', 'dpad.down', 'dpad.left', 'dpad.right',
        'left.stick', 'right.stick', 'left.trigger', 'right.trigger',
        'left.bumper', 'right.bumper', 'start', 'back', 'guide',
        'misc1', 'misc2', 'misc3', 'misc4', 'misc5', 'misc6',
        'left.paddle1', 'right.paddle1', 'left.paddle2', 'right.paddle2',
        'touchpad',
    ];
    const reads = ['connected', 'rumbleAvailable', 'name', 'type'];
    for (const control of controls) {
        for (const edge of ['held', 'pressed', 'released']) {
            reads.push(`${control}.${edge}`);
        }
    }
    for (const face of ['south', 'east', 'west', 'north']) reads.push(`${face}.label`);
    for (const side of ['left', 'right']) {
        for (const axis of ['x', 'y', 'angle', 'magnitude']) {
            reads.push(`${side}.stick.${axis}`);
        }
        reads.push(`${side}.trigger.value`);
    }
    let result = compile(source(reads.map((field) => `Print(Gamepad.${field}):`).join('\n')));
    assert.equal(result.status, 0, diagnostic(result));

    // Vibration accepts a numeric expression and a normal ms/s duration literal.
    result = compile(source(`number Strength = 70:
        Gamepad.vibrate(Strength + 5, 200ms):
        Gamepad.vibrate(25, 0.25s):
        Gamepad.vibrate(0, 0ms):`));
    assert.equal(result.status, 0, diagnostic(result));

    const cases = [
        [source('Print(Gamepad.connected):', ''), settings, /requires import ZSharp\.Gamepad/],
        [source('Print(Gamepad.connected):'), settings.replace('ZSharp: [1.3.0.0]', 'ZSharp: [1.2.2.0]'), /requires ZSharp: \[1\.3\.0\.0\]/],
        [source('Print(Gamepad.connected):'), settings.replace('Window (\n StartScene: "Lab.zscene":\n):\n', ''), /available only in games/],
        [source('Print(Gamepad.typo):'), settings, /unknown or read-only Gamepad/],
        [source('Print(Gamepad.south.value):'), settings, /unknown or read-only Gamepad/],
        [source('Print(Gamepad.left.trigger.angle):'), settings, /unknown or read-only Gamepad/],
        [source('Print(Gamepad.left.stick.label):'), settings, /unknown or read-only Gamepad/],
        [source('Gamepad.connected.set: alive:'), settings, /unknown or read-only Gamepad/],
        [source('number.set:Gamepad.left.stick.x = 1:'), settings, /unknown or read-only Gamepad/],
        [source('number.set:Gamepad.right.trigger.value = 1:'), settings, /unknown or read-only Gamepad/],
        [source('Gamepad.vibrate(75, 200):'), settings, /duration requires s or ms/],
        [source('number Duration = 200: Gamepad.vibrate(75, Duration):'), settings, /expected a number/],
        [source('Gamepad.vibrate(75):'), settings, /vibration strength/],
        [source('Gamepad.vibrate(75, 200ms, 1):'), settings, /after vibration/],
        [source('Gamepad.vibrate(75, 200ms):', ''), settings, /requires import ZSharp\.Gamepad/],
    ];
    for (const [text, config, expected] of cases) {
        result = compile(text, config);
        assert.notEqual(result.status, 0, text);
        assert.match(diagnostic(result), expected, text);
    }
    console.log('PASS: Gamepad property syntax, imports, version/game gates, read-only values and vibration forms');
} finally {
    fs.rmSync(root, { recursive: true, force: true });
}
