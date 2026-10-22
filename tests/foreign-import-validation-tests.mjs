import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const binary = path.resolve(process.argv[2]);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-foreign-imports-'));
let checks = 0;
const settings = (pid = 'bridge_fixture', version = '1.4.0.0', dependencies = '') => `zsharp = type.settings
Project: "Foreign import tests": PID: "${pid}": Version: [1.4.0.0]:
Authors: ["Z# Tests"]: Description: "Owned validation fixtures":
ZSharp: [${version}]: Dependencies (${dependencies}):
`;
function write(name, value) {
  const file = path.join(root, name);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, value);
}
function compile(success, expected) {
  const result = spawnSync(binary, ['compile', 'Entry.zsharp', '-o', 'Result.zbc'], {
    cwd: root, encoding: 'utf8', windowsHide: true, timeout: 20000,
  });
  const output = (result.stdout || '') + (result.stderr || '');
  assert.ifError(result.error);
  if (success) assert.equal(result.status, 0, output);
  else {
    assert.notEqual(result.status, 0, output);
    assert.match(output, expected);
  }
  fs.rmSync(path.join(root, 'Result.zbc'), { force: true });
  checks++;
}
try {
  for (const [prefix, extension, companion] of [
    ['cs', 'cs', 'zcs.dll'], ['java', 'java', 'zjava.jar'],
    ['go', 'go', process.platform === 'win32' ? 'zgo.exe' : 'zgo'], ['rb', 'rb', null],
  ]) {
    write('project.zsettings', settings());
    write('Entry.zsharp', `zsharp = type.script noticed room Main[] (
 import ${prefix}:bridge_fixture.Module():
 noticed brain Start[] (Function.call(${prefix}:Module:echo ["hello"]):)
)`);
    write(`Module.${extension}`, 'Source validation fixture; not executed.');
    compile(true);
    write('project.zsettings', settings('bridge_fixture', '1.3.0.0'));
    compile(false, /requires? ZSharp: \[1\.4\.0\.0\]/);
    write('project.zsettings', settings());
    fs.unlinkSync(path.join(root, `Module.${extension}`));
    compile(false, /not found/);
    if (companion) {
      write(`Module.${companion}`, 'Compiled artifact validation fixture; not executed.');
      compile(true);
      fs.unlinkSync(path.join(root, `Module.${companion}`));
    }
    write('Entry.zsharp', `zsharp = type.script noticed room Main[] (
 noticed brain Start[] (Function.call(${prefix}:Module:echo):)
)`);
    compile(false, /requires import/);
    write('Entry.zsharp', `zsharp = type.script noticed room Main[] (import ${prefix}:bridge_fixture.*():)`);
    write('project.zsettings', settings('bridge_fixture', '1.3.0.0'));
    compile(false, /requires? ZSharp: \[1\.4\.0\.0\]/);
  }
  write('project.zsettings', settings('bridge_fixture', '1.4.0.0', 'library:1.0.0.0:'));
  write('Dependencies/library/1.0.0.0/project.zsettings', settings('library').replace('Version: [1.4.0.0]', 'Version: [1.0.0.0]'));
  write('Dependencies/library/1.0.0.0/Module.java', 'Source dependency fixture; not executed.');
  write('Entry.zsharp', `zsharp = type.script noticed room Main[] (
 import java:library.Module():
 noticed brain Start[] (Function.call(java:library.Module:echo):)
)`);
  compile(true);
  console.log(`Foreign import project validation: ${checks} checks passed`);
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
