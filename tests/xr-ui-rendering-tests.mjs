import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-xr-ui-render-'));
const exe = path.resolve('build/Release/zsharp.exe');
const projectId = 'zsharp_xr_ui_render_test';

try {
    const settings = fs.readFileSync('examples/pcvr-test/project.zsettings', 'utf8')
        .replace('zsharp_pcvr_lab', projectId)
        .replace('Z# PCVR Lab', 'Z# XR UI Render Test');
    fs.writeFileSync(path.join(root, 'project.zsettings'), settings);
    fs.copyFileSync('examples/pcvr-test/Cube.zobject', path.join(root, 'Cube.zobject'));
    fs.writeFileSync(path.join(root, 'Lab.zscene'), `zsharp = type.scene
noticed scene Lab[] (
 scene Lab ( background: #101522: gravityY: 0: cameraY: 1.6: cameraZ: 4: )
 objects[JSON] (
  [{"id":"Cube","name":"Target","location":{"x":0,"y":1.3,"z":-2}}]
 )
)
`);
    fs.writeFileSync(path.join(root, 'Main.zsharp'), `zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (Print("XR UI RENDER TEST START"):)
)
`);
    let result = spawnSync(exe, ['package', 'game', root, 'XR-UI-Render'], {
        encoding: 'utf8', timeout: 30000,
    });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    result = spawnSync(exe, ['open', path.join(root, 'Packages', 'XR-UI-Render.zgame')], {
        encoding: 'utf8', timeout: 30000,
        env: {
            ...process.env,
            ZSHARP_XR_MODE: 'desktop',
            ZSHARP_GAME_AUTOCLOSE_MS: '600',
            ZSHARP_GAME_WORLD_UI_TEST: '1',
            ZSHARP_SKIP_UPDATE_CHECK: '1',
        },
    });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout + result.stderr, /PASS: XR world UI buttons\/text/);
    assert.match(result.stdout, /XR UI RENDER TEST START/);
    console.log('PASS: real XR UI world export: buttons/text, logical pixel scaling, XYZ/legacy rotation, clipped labels, transparent contrast, unlit colors, opacity, Unicode fallback and immutable glyph atlas');
} finally {
    spawnSync(exe, ['hub', 'remove', projectId], {encoding: 'utf8', timeout: 15000});
    fs.rmSync(root, {recursive: true, force: true});
}
