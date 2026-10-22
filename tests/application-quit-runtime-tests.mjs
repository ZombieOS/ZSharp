// No external apps are controlled. Windows GUI tests create their own short-lived window.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';

const exe = path.resolve(process.argv[2] || 'build/Release/zsharp.exe');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-application-quit-'));
const settings = `zsharp = type.settings
Project: "Application Quit Test":
PID: "application_quit_test":
Version: [1.4.0.0]:
Authors: ["Z# Tests"]:
Description: "Isolated application lifecycle regression tests":
ZSharp: [1.4.0.0]:
Dependencies ():
`;
const source = (body, imports = 'import ZSharp.Application():') => `zsharp = type.script
noticed room Actions[] (
 ${imports}
 noticed brain Start[] ( ${body} )
 noticed brain Shutdown[] ( Print("CLEANUP ONCE"): )
)
`;
const environment = {...process.env, ZSHARP_SKIP_UPDATE_CHECK: '1',
  ZSHARP_SKIP_DESKTOP_INTEGRATION: '1', ZSHARP_DISABLE_PROJECT_STARTS: undefined};
delete environment.ZSHARP_DISABLE_PROJECT_STARTS;
const run = (...args) => {
  const result = spawnSync(exe, args, {cwd: root, env: environment, encoding: 'utf8', timeout: 15000});
  assert.equal(result.error, undefined, String(result.error));
  return {...result, combined: result.stdout + result.stderr};
};
let checks = 0;
try {
  fs.writeFileSync(path.join(root, 'project.zsettings'), settings);
  const script = path.join(root, 'Actions.zsharp');
  for (const [body, imports, expected] of [
    ['Application.Quit("regular"):', '', /requires import ZSharp\.Application/],
    ['Application.Quit:', 'import ZSharp.Application():', /explicit mode/],
    ['Application.Quit():', 'import ZSharp.Application():', /expected/],
    ['Application.Quit("regular", "tray"):', 'import ZSharp.Application():', /single Application\.Quit mode/],
    ['Application.Quit("unknown"):', 'import ZSharp.Application():', /mode must be "regular" or "tray"/],
    ['Application.Quit(100):', 'import ZSharp.Application():', /mode must be text/],
    ['Application.Quit("regular"):', 'import ZSharp.Application():', /requires a running app or game/],
    ['Application.Quit("tray"):', 'import ZSharp.Application():', /requires a running desktop app/]
  ]) {
    fs.writeFileSync(script, source(body, imports));
    const result = run('run', script);
    assert.notEqual(result.status, 0, body);
    assert.match(result.combined, expected, result.combined);
    checks++;
  }
  // Value/type errors remain validated after a source -> bytecode round trip.
  for (const [mode, expected] of [['"bad"', /mode must be "regular" or "tray"/], ['12', /mode must be text/]]) {
    fs.writeFileSync(script, source(`Application.Quit(${mode}):`));
    const bytecode = path.join(root, 'Invalid.zbc');
    const compiled = run('compile', script, '-o', bytecode);
    assert.equal(compiled.status, 0, compiled.combined);
    const result = run('run-bytecode', bytecode);
    assert.notEqual(result.status, 0);
    assert.match(result.combined, expected, result.combined);
    checks++;
  }
  fs.writeFileSync(path.join(root, 'project.zsettings'), settings.replace('ZSharp: [1.4.0.0]:', 'ZSharp: [1.3.0.0]:'));
  fs.writeFileSync(script, source('Application.Quit("regular"):'));
  const oldVersion = run('compile', script, '-o', path.join(root, 'Old.zbc'));
  assert.notEqual(oldVersion.status, 0);
  assert.match(oldVersion.combined, /requires ZSharp: \[1\.4\.0\.0\]/);
  checks++;

  if (process.platform === 'win32') {
    fs.writeFileSync(path.join(root, 'project.zsettings'),
      settings.replace('Dependencies ():', 'Dependencies ( zsharpwindow:1.0.0.0 ):') +
      'Window ( Startup: "Startup.zsharp": ):\n');
    const startup = path.join(root, 'Startup.zsharp');
    fs.writeFileSync(startup, `zsharp = type.script:window
noticed Window Startup[] (
 import ZSharp.Window.*():
 noticed design Design[] (
  title: "Z# Quit Lifecycle Test": width: 320px: height: 160px: background: #18241D:
 )
)
`);
    for (const tray of [false, true]) {
      fs.writeFileSync(script, source(`Print("BEFORE"):
        ${tray ? 'Application.Quit("tray"): Print("TRAY RETURNED"): wait(150ms): Application.Quit("tray"): Print("TRAY REPEATED"): wait(150ms):' : ''}
        Application.Quit("regular"):
        Print("UNREACHABLE AFTER REGULAR"):`));
      const result = run('run', startup);
      assert.equal(result.status, 0, result.combined);
      assert.match(result.stdout, /BEFORE/);
      assert.doesNotMatch(result.stdout, /UNREACHABLE AFTER REGULAR/);
      assert.equal((result.stdout.match(/CLEANUP ONCE/g) || []).length, 1, result.combined);
      if (tray) {
        assert.match(result.stdout, /TRAY RETURNED/);
        assert.match(result.stdout, /TRAY REPEATED/);
        assert.ok(result.stdout.indexOf('CLEANUP ONCE') > result.stdout.indexOf('TRAY REPEATED'));
      }
      checks++;
    }
  }
  console.log(`PASS: ${checks} Application.Quit diagnostics, version, bytecode and lifecycle checks`);
} finally {
  fs.rmSync(root, {recursive: true, force: true});
}
