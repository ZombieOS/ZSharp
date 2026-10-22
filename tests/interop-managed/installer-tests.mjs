import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';

const binary = path.resolve(process.argv[2]);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-interop-installer-'));
const platform = `${process.platform === 'win32' ? 'windows' : process.platform === 'darwin' ? 'macos' : 'linux'}-${process.arch === 'arm64' ? 'aarch64' : 'x86_64'}`;
const runtimeName = process.platform === 'win32' ? 'zsharp.exe' : 'zsharp';
const digest = data => crypto.createHash('sha256').update(data).digest('hex');
let checks = 0;

function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit++) value = (value >>> 1) ^ (value & 1 ? 0xedb88320 : 0);
  }
  return (value ^ 0xffffffff) >>> 0;
}
function storedZip(entries) {
  const local = [], central = [];
  let offset = 0;
  for (const [name, data, mode = 0] of entries) {
    const filename = Buffer.from(name), contents = Buffer.from(data), checksum = crc32(contents);
    const header = Buffer.alloc(30);
    header.writeUInt32LE(0x04034b50); header.writeUInt16LE(20, 4);
    header.writeUInt32LE(checksum, 14); header.writeUInt32LE(contents.length, 18);
    header.writeUInt32LE(contents.length, 22); header.writeUInt16LE(filename.length, 26);
    local.push(header, filename, contents);
    const record = Buffer.alloc(46);
    record.writeUInt32LE(0x02014b50); record.writeUInt16LE(0x0314, 4); record.writeUInt16LE(20, 6);
    record.writeUInt32LE(checksum, 16); record.writeUInt32LE(contents.length, 20);
    record.writeUInt32LE(contents.length, 24); record.writeUInt16LE(filename.length, 28);
    record.writeUInt32LE(mode >>> 0, 38); record.writeUInt32LE(offset, 42);
    central.push(record, filename);
    offset += header.length + filename.length + contents.length;
  }
  const index = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(index.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...local, index, end]);
}

function install(label, entries, { success = true, corruptHash = false, legacy = false, metadataOnly = false, archiveData } = {}) {
  const directory = path.join(root, label);
  fs.mkdirSync(directory, { recursive: true });
  const runtime = Buffer.from('Owned installer test runtime; never executed.');
  const interop = archiveData || storedZip(entries);
  const outer = storedZip([
    [`runtimes/${platform}/${runtimeName}`, runtime],
    [`runtimes/${platform}/interop-runtime.zip`, interop],
  ]);
  const archive = path.join(directory, 'runtime.zip'), manifest = path.join(directory, 'manifest.json');
  fs.writeFileSync(archive, outer);
  const info = { path: `runtimes/${platform}/${runtimeName}`, sha256: digest(runtime), size: runtime.length };
  if (!legacy) {
    info.interopSize = interop.length;
    if (!metadataOnly) {
      info.interopPath = `runtimes/${platform}/interop-runtime.zip`;
      info.interopSha256 = corruptHash ? '0'.repeat(64) : digest(interop);
    }
  }
  fs.writeFileSync(manifest, JSON.stringify({ schema: 1, latestVersion: '1.4.0.0',
    download: { url: 'https://example.invalid/test.zip', sha256: digest(outer), size: outer.length },
    platforms: { [platform]: info } }));
  const result = spawnSync(binary, ['--yes', '--manifest-file', manifest, '--install-dir', path.join(directory, 'install'), '--current-version', '0.0.0.0'], {
    encoding: 'utf8', windowsHide: true, timeout: 15000,
    env: { ...process.env, ZSHARP_INSTALLER_ARTIFACT_FILE: archive,
      ZSHARP_INSTALLER_SKIP_INTEGRATION: '1', ZSHARP_INSTALLER_SKIP_HUB_SHORTCUT: '1' },
  });
  assert.ifError(result.error);
  const output = (result.stdout || '') + (result.stderr || '');
  if (success) assert.equal(result.status, 0, `${label}: ${output}`);
  else assert.notEqual(result.status, 0, `${label} unexpectedly accepted: ${output}`);
  checks++;
  return { directory, output };
}

try {
  const entries = [
    ['interop/java/zsharp-java-worker.jar', 'owned java helper'],
    ['interop/csharp/ZSharp.Interop.Worker.dll', 'owned managed helper'],
    ['interop/python/zsharp_worker.py', 'owned python helper'],
    ['interop/go/go.mod', 'module zsharp\n'],
  ];
  const installed = install('valid', entries);
  for (const [name, contents] of entries) {
    assert.equal(fs.readFileSync(path.join(installed.directory, 'install', name), 'utf8'), contents);
    checks++;
  }
  install('valid', entries); // Update keeps the previous helper tree recoverable.
  assert.ok(fs.readdirSync(path.join(installed.directory, 'install')).some(name => name.startsWith('.zsharp-interop-previous-')));
  checks++;
  install('legacy', entries, { legacy: true });
  if (process.argv[3]) {
    const actual = install('shipping-assets', [], { archiveData: fs.readFileSync(process.argv[3]) });
    for (const helper of ['java/zsharp-java-worker.jar', 'csharp/ZSharp.Interop.Worker.dll',
      'python/zsharp_worker.py', 'ruby/zsharp_worker.rb', 'go/zsharp.go']) {
      assert.ok(fs.statSync(path.join(actual.directory, 'install/interop', helper)).size > 0);
      checks++;
    }
  }
  for (const name of ['interop/../escape.txt', '../escape.txt', '/interop/escape.txt',
    'interop/a\\escape.txt', 'interop/C:/escape.txt', 'interop//escape.txt',
    'interop/./escape.txt', 'interop/CON.txt', 'interop/NUL', 'interop/file.', 'interop/dir/']) {
    const rejected = install(`invalid-${checks}`, [[name, 'must not be extracted']], { success: false });
    assert.match(rejected.output, /unsafe|invalid|incomplete/);
    assert.ok(!fs.existsSync(path.join(rejected.directory, 'install', runtimeName)));
    checks++;
  }
  install('symlink', [['interop/link', '../escape', (0o120777 << 16) >>> 0]], { success: false });
  install('duplicate', [['interop/a.txt', 'a'], ['interop/a.txt', 'b']], { success: false });
  install('case-collision', [['interop/A.txt', 'a'], ['interop/a.txt', 'b']], { success: false });
  assert.match(install('bad-hash', entries, { success: false, corruptHash: true }).output, /SHA-256/);
  assert.match(install('partial-metadata', entries, { success: false, metadataOnly: true }).output, /metadata/);
  assert.ok(!fs.existsSync(path.join(root, 'escape.txt')));
  checks++;
  console.log(`Interop installer validation: ${checks} checks passed`);
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
