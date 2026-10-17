import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-xr-ui-model-'));
const exe = path.resolve('build/Release/zsharp.exe');
const source = path.resolve('examples/pcvr-test');
try {
  fs.cpSync(source, root, {
    recursive: true,
    filter: file => !path.relative(source, file).split(path.sep).includes('Packages'),
  });
  const settings = fs.readFileSync(path.join(root, 'project.zsettings'), 'utf8')
    .replace('zsharp_pcvr_lab', 'zsharp_xr_ui_model_test')
    .replace('Z# PCVR Lab', 'Z# XR UI Model Test');
  fs.writeFileSync(path.join(root, 'project.zsettings'), settings);
  fs.writeFileSync(path.join(root, 'Main.zsharp'),
    'zsharp = type.script noticed room Main[] ( noticed brain Start[] ( Print("XR UI MODEL START"): ) )');
  let result = spawnSync(exe, ['package', 'game', root, 'XR-UI-Model'], {
    encoding: 'utf8', timeout: 30000,
  });
  assert.equal(result.status, 0, result.stdout + result.stderr);
  result = spawnSync(exe, ['open', path.join(root, 'Packages', 'XR-UI-Model.zgame')], {
    encoding: 'utf8', timeout: 30000,
    env: {
      ...process.env,
      ZSHARP_XR_MODE: 'desktop',
      ZSHARP_GAME_AUTOCLOSE_MS: '400',
      ZSHARP_GAME_MODEL_UI_TEST: '1',
      ZSHARP_SKIP_UPDATE_CHECK: '1',
    },
  });
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout + result.stderr, /PASS: XR UI model snapshot/);
  assert.match(result.stdout, /XR UI MODEL START/);
  console.log('PASS: real model dual-controller hover, style transitions, scene changes, O(1) snapshot identity and desktop regression');
} finally {
  spawnSync(exe, ['hub', 'remove', 'zsharp_xr_ui_model_test'], {
    encoding: 'utf8', timeout: 15000,
  });
  fs.rmSync(root, {recursive: true, force: true});
}
