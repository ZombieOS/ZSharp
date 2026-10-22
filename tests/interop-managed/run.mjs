import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const build = path.resolve(process.argv[2] || path.join(root, 'build-interop-managed'));
const javaClasses = path.join(build, 'java-sdk');
const javaSample = path.join(build, 'java-sample');
const csharp = path.join(build, 'csharp-sdk');
const csharpSample = path.join(build, 'csharp-sample');
const javaSettings = spawnSync('java', ['-XshowSettings:properties', '-version'], { encoding: 'utf8', windowsHide: true });
const javaHome = /java\.home\s*=\s*([^\r\n]+)/.exec(javaSettings.stderr || '')?.[1]?.trim();
const jarTool = javaHome ? path.join(javaHome, 'bin', process.platform === 'win32' ? 'jar.exe' : 'jar') : 'jar';
for (const folder of [javaClasses, javaSample, csharp, csharpSample]) fs.mkdirSync(folder, { recursive: true });

function command(program, args) {
  const result = spawnSync(program, args, { cwd: root, encoding: 'utf8', windowsHide: true });
  if (result.error || result.status !== 0) {
    throw new Error(`${program} failed: ${result.error || result.stdout + result.stderr}`);
  }
}

const sdkSource = path.join(root, 'native/interop/java/src/ZSharp');
command('javac', ['--release', '17', '-encoding', 'UTF-8', '-d', javaClasses,
  ...fs.readdirSync(sdkSource).filter(name => name.endsWith('.java')).map(name => path.join(sdkSource, name))]);
const workerJar = path.join(build, 'zsharp-java-worker.jar');
command(jarTool, ['--create', '--file', workerJar, '-C', javaClasses, '.']);
command('javac', ['--release', '17', '-encoding', 'UTF-8', '-cp', javaClasses, '-d', javaSample,
  path.join(root, 'tests/interop-managed/Sample.java')]);
const sampleJar = path.join(build, 'Sample.zjava.jar');
command(jarTool, ['--create', '--file', sampleJar, '-C', javaSample, '.']);
command('dotnet', ['build', path.join(root, 'native/interop/csharp/ZSharp.Interop.Worker.csproj'), '-c', 'Release', '-o', csharp, '--nologo']);
command('dotnet', ['build', path.join(root, 'tests/interop-managed/Sample.csproj'), '-c', 'Release', '-o', csharpSample, '--nologo']);

const number = text => ({ $number: String(text) });
let assertions = 0;
function equal(actual, expected) { assert.deepEqual(actual, expected); assertions++; }

async function exercise(language, program, arguments_, module) {
  const child = spawn(program, arguments_, { cwd: build, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  const pending = new Map();
  const lines = readline.createInterface({ input: child.stdout });
  let stderr = '';
  let serial = 1;
  let hostValue = 7;
  let callbackError = false;
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', value => { stderr += value; });
  const exited = new Promise(resolve => child.on('exit', (code, signal) => {
    for (const { reject, timeout } of pending.values()) {
      clearTimeout(timeout);
      reject(new Error(`${language} worker exited (${code}/${signal}): ${stderr}`));
    }
    pending.clear();
    resolve(code);
  }));
  lines.on('line', line => {
    let message;
    try { message = JSON.parse(line); } catch (error) { throw new Error(`${language} protocol corruption: ${line}`); }
    if (message.op.startsWith('host.')) {
      let value = null;
      if (callbackError) {
        child.stdin.write(JSON.stringify({ id: message.id, op: 'result', ok: false, error: 'intentional callback failure' }) + '\n');
        return;
      }
      if (message.op === 'host.call') {
        equal(message.target, 'Tests:Bridge:Double');
        value = number(Number(message.args[0].$number) * 2);
      } else {
        equal(message.ref, 'z-object-1');
        if (message.op === 'host.get') value = number(hostValue);
        else if (message.op === 'host.set') hostValue = Number(message.args[0].$number);
        else if (message.op === 'host.invoke') value = number(hostValue + Number(message.args[0].$number));
        else throw new Error('Unexpected callback operation');
      }
      child.stdin.write(JSON.stringify({ id: message.id, op: 'result', ok: true, value }) + '\n');
      return;
    }
    const request = pending.get(String(message.id));
    if (!request) throw new Error(`Unexpected ${language} result id: ${message.id}`);
    pending.delete(String(message.id));
    clearTimeout(request.timeout);
    request.resolve(message);
  });
  function request(op, extra = {}) {
    const id = serial++;
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => { pending.delete(String(id)); reject(new Error(`${language} timeout: ${op}`)); child.kill(); }, 10000);
      pending.set(String(id), { resolve, reject, timeout });
      child.stdin.write(JSON.stringify({ id, op, module, ...extra }) + '\n');
    });
  }
  async function ok(op, extra) {
    const result = await request(op, extra);
    if (!result.ok) throw new Error(`${language} ${op} ${JSON.stringify(extra)}: ${result.error}`);
    equal(result.ok, true);
    return result.value;
  }
  async function failure(op, extra, pattern) {
    const result = await request(op, extra);
    equal(result.ok, false);
    assert.match(result.error, pattern);
    assertions++;
  }
  try {
    equal(await ok('call', { target: 'Sample.add', args: [number(20), number(22)] }), number(42));
    equal(await ok('call', { target: 'counted', args: [] }), number(1));
    equal(await ok('call', { target: 'counted', args: [] }), number(2));
    const exact = number('123456789012345678901234567890.12345678901234567890');
    equal(await ok('call', { target: 'exact', args: [exact] }), exact);
    equal(await ok('call', { target: 'array', args: [[number(1), number(2), number(3)]] }), [number(1), number(2), number(3)]);
    equal(await ok('call', { target: 'record', args: [] }), { $record: { name: 'bridge', value: number(42) } });
    equal(await ok('call', { target: 'overload', args: [number(1)] }), 'int');
    equal(await ok('call', { target: 'overload', args: ['hi'] }), 'text');
    await failure('call', { target: 'ambiguous', args: [null] }, /Ambiguous/);
    await failure('call', { target: 'Sample.add', args: [number('1.25'), number(2)] }, /No exported/);
    await failure('call', { target: 'Sample.add', args: [number('999999999999999999999999999'), number(2)] }, /No exported/);
    await failure('call', { target: 'NotPublic.leak', args: [] }, /not public|not part/);
    const reference = await ok('new', { target: 'Sample', args: [number(4)] });
    equal(typeof reference.$ref, 'string');
    equal(reference.$type, 'Sample');
    equal(await ok('get', { ref: reference.$ref, member: 'value' }), number(4));
    await ok('set', { ref: reference.$ref, member: 'value', args: [number(10)] });
    equal(await ok('invoke', { ref: reference.$ref, member: 'increase', args: [number(5)] }), number(15));
    equal(await ok('invoke', { ref: reference.$ref, member: 'self', args: [] }), reference);
    await failure('get', { ref: reference.$ref, member: 'secret' }, /not exported|NoSuchField/);
    await failure('set', { ref: reference.$ref, member: 'readOnly', args: ['changed'] }, /writable|read-only/);
    await failure('invoke', { ref: reference.$ref, member: 'hidden' }, /No exported/);
    await failure('invoke', { ref: reference.$ref, member: language === 'Java' ? 'getClass' : 'GetType' }, /No exported/);
    if (language === 'C#') {
      equal(await ok('get', { ref: reference.$ref, member: 'publicGetPrivateSet' }), 'readable');
      await failure('set', { ref: reference.$ref, member: 'publicGetPrivateSet', args: ['no'] }, /public set accessor/);
      equal(await ok('get', { ref: reference.$ref, member: 'initOnly' }), 'initial');
      await failure('set', { ref: reference.$ref, member: 'initOnly', args: ['no'] }, /init-only/);
    }
    equal(await ok('call', { target: 'callback', args: [number(21)] }), number(42));
    callbackError = true;
    await failure('call', { target: 'callback', args: [number(21)] }, /intentional callback failure/);
    callbackError = false;
    equal(await ok('call', { target: 'proxy', args: [{ $zref: 'z-object-1', $type: 'Tests.Bridge' }] }), number(28));
    equal(hostValue, 21);
    equal(await ok('call', { target: 'printed' }), 'printed');
    await ok('release', { ref: reference.$ref });
    await failure('get', { ref: reference.$ref, member: 'value' }, /released/);
    await failure('release', { ref: reference.$ref }, /released/);
    await failure('call', { target: 'add', args: [{ $number: 'not-a-number' }, number(2)] }, /NumberFormat|valid decimal/);
    equal(await ok('call', { target: 'counted', args: [] }), number(3));
  } finally {
    child.stdin.end();
    equal(await exited, 0);
  }
  assert.match(stderr, /user-output-stays-on-stderr/);
  assertions++;
  console.log(`${language} persistent worker: passed`);
}

await exercise('Java', 'java', ['-cp', workerJar, 'ZSharp.Worker'], sampleJar);
await exercise('C#', 'dotnet', [path.join(csharp, 'ZSharp.Interop.Worker.dll')], path.join(csharpSample, 'Sample.zcs.dll'));
console.log(`Managed interop: ${assertions} assertions passed`);
