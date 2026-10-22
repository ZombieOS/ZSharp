// Isolated CLI fixtures. No physical recording/playback, application control,
// startup registration, model installation, network or user project access.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';
import {spawn, spawnSync} from 'node:child_process';

const executable = path.resolve(process.argv[2] || 'build/Release/zsharp.exe');
const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const temporaryParent = fs.realpathSync(os.tmpdir());
const root = fs.mkdtempSync(path.join(temporaryParent, 'zsharp-system-api-'));
const startup = path.join(root, 'startup-fixtures');
const helpers = path.join(root, 'python-fixtures');
fs.mkdirSync(startup);
fs.mkdirSync(helpers);
fs.writeFileSync(path.join(helpers, 'zsharp_bridge.py'), '# Unexecuted fixture path anchor.\n');
fs.copyFileSync(path.join(repository, 'native/python/zsharp_speech_input.py'),
  path.join(helpers, 'zsharp_speech_input.py'));

const environment = {
  ...process.env,
  SDL_AUDIODRIVER: 'dummy',
  ZSHARP_DESKTOP_DISABLE_ACTIONS: '1',
  ZSHARP_SKIP_UPDATE_CHECK: '1',
  ZSHARP_SKIP_DESKTOP_INTEGRATION: '1',
  ZSHARP_SKIP_ASSOCIATION_INSTALL: '1',
  ZSHARP_STARTUP_DIRECTORY: startup,
  // Never discover a developer's actual installed models or managed speech home.
  ZSHARP_SPEECH_HOME: path.join(root, 'intentionally-unconfigured-speech'),
  ZSHARP_SPEECH_MODEL: path.join(root, 'missing-vosk-model'),
  ZSHARP_VOICE_MODEL_CONFIG: path.join(root, 'missing-voice-model.yaml'),
  ZSHARP_PYTHON_BRIDGE: path.join(helpers, 'zsharp_bridge.py'),
  // A nonempty override also prevents the Python archive bootstrap/download path.
  ZSHARP_PYTHON_RUNTIME: path.join(root, 'intentionally-missing-python'),
  ZSHARP_SPEECH_PYTHON: path.join(root, 'intentionally-missing-python'),
};
delete environment.ZSHARP_DISABLE_PROJECT_STARTS;

function findPython() {
  const names = process.platform === 'win32' ? ['python.exe', 'python3.exe'] : ['python3', 'python'];
  const candidates = [process.argv[3], process.env.ZSHARP_TEST_PYTHON,
    path.join(path.dirname(executable), 'python', process.platform === 'win32' ? 'python.exe' : 'bin/python3')];
  for (const directory of (process.env.PATH || '').split(path.delimiter)) {
    // Windows App Execution Aliases can open the Store; never execute them.
    if (!directory || /[\\/]WindowsApps(?:[\\/]|$)/i.test(directory)) continue;
    for (const name of names) candidates.push(path.join(directory, name));
  }
  for (const candidate of candidates.filter(Boolean)) {
    if (!path.isAbsolute(candidate) || !fs.existsSync(candidate)) continue;
    const result = spawnSync(candidate, ['-I', '-c', 'import sys;print(sys.executable)'],
      {env: environment, encoding: 'utf8', timeout: 10000, windowsHide: true});
    if (!result.error && result.status === 0 && result.stdout.trim()) return result.stdout.trim();
  }
  return null;
}
const python = findPython();
if (python) {
  environment.ZSHARP_PYTHON_RUNTIME = python;
  environment.ZSHARP_SPEECH_PYTHON = python;
}

const normalSettings = `zsharp = type.settings
Project: "System API Runtime Fixtures":
PID: "system_api_fixture":
Version: [1.4.0.0]:
Authors: ["Z# Tests"]:
Description: "Owned temporary desktop API fixtures":
ZSharp: [1.4.0.0]:
Dependencies ():
`;
const imports = ['Speech', 'Microphone', 'Speaker', 'Desktop']
  .map(name => `import ZSharp.${name}():`).join('\n');
const source = (body, featureImports = imports) => `zsharp = type.script
noticed room Main[] (
 ${featureImports}
 brain Start[] (${body})
)
`;
const write = (name, value) => fs.writeFileSync(path.join(root, name), value);
const sourcePath = path.join(root, 'Main.zsharp');
let checks = 0;
function run(arguments_, env = environment) {
  assert.equal(env.SDL_AUDIODRIVER, 'dummy', 'physical audio drivers must remain disabled');
  assert.equal(env.ZSHARP_DESKTOP_DISABLE_ACTIONS, '1', 'real application actions must remain disabled');
  const result = spawnSync(executable, arguments_,
    {cwd: root, env, encoding: 'utf8', timeout: 20000, windowsHide: true});
  assert.equal(result.error, undefined, String(result.error));
  return {...result, combined: (result.stdout || '') + (result.stderr || '')};
}
function success(label, result) {
  assert.equal(result.status, 0, `${label}\n${result.combined}`);
  checks++; return result;
}
function failure(label, result, pattern) {
  assert.notEqual(result.status, 0, `${label} unexpectedly succeeded\n${result.combined}`);
  assert.match(result.combined, pattern, `${label}\n${result.combined}`);
  checks++; return result;
}
function compile(label, name = 'Main.zbc', env = environment) {
  return success(label, run(['compile', sourcePath, '-o', path.join(root, name)], env));
}
function rejectSyntax(label, body, pattern, featureImports = imports) {
  write('Main.zsharp', source(body, featureImports));
  for (const command of [['check', sourcePath], ['run', sourcePath],
    ['compile', sourcePath, '-o', path.join(root, 'Rejected.zbc')]])
    failure(`${label}: ${command[0]}`, run(command), pattern);
}
function rejectRuntime(label, body, pattern, env = environment) {
  write('Main.zsharp', source(body));
  failure(`${label}: source`, run(['run', sourcePath], env), pattern);
  compile(`${label}: compile`, 'Main.zbc', env);
  failure(`${label}: bytecode`, run(['run-bytecode', path.join(root, 'Main.zbc')], env), pattern);
}
function mutateBytecode(name, edit) {
  const data = Buffer.from(fs.readFileSync(path.join(root, 'Main.zbc')));
  edit(data);
  assert.equal(data.subarray(0, 4).toString(), 'ZSBC');
  const hashOffset = 12 + data.readUInt32LE(8) + 32;
  assert.ok(hashOffset + 32 < data.length);
  crypto.createHash('sha256').update(data.subarray(0, hashOffset))
    .update(data.subarray(hashOffset + 32)).digest().copy(data, hashOffset);
  const filename = path.join(root, name);
  fs.writeFileSync(filename, data);
  return filename;
}
function replaceEqual(data, from, to) {
  const before = Buffer.from(from), after = Buffer.from(to);
  assert.equal(before.length, after.length);
  const offset = data.indexOf(before);
  assert.ok(offset >= 0 && data.indexOf(before, offset + 1) < 0, `unique token ${from}`);
  after.copy(data, offset);
}
function verifyRecording(data, sampleCount = 1600) {
  assert.equal(data.length, 44 + sampleCount * 2, 'recording contains the full requested sample count');
  assert.equal(data.subarray(0, 4).toString(), 'RIFF');
  assert.equal(data.readUInt32LE(4), data.length - 8);
  assert.equal(data.subarray(8, 16).toString(), 'WAVEfmt ');
  assert.equal(data.readUInt32LE(16), 16);
  assert.equal(data.readUInt16LE(20), 1, 'uncompressed PCM');
  assert.equal(data.readUInt16LE(22), 1, 'mono');
  assert.equal(data.readUInt32LE(24), 16000);
  assert.equal(data.readUInt32LE(28), 32000);
  assert.equal(data.readUInt16LE(32), 2);
  assert.equal(data.readUInt16LE(34), 16);
  assert.equal(data.subarray(36, 40).toString(), 'data');
  assert.equal(data.readUInt32LE(40), sampleCount * 2);
}
async function observeAtomicRecording(arguments_, filename, previous) {
  assert.equal(environment.SDL_AUDIODRIVER, 'dummy');
  const child = spawn(executable, arguments_,
    {cwd: root, env: environment, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']});
  let stdout = '', stderr = '', observationError, observations = 0;
  child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
  child.stdout.on('data', value => {stdout += value;});
  child.stderr.on('data', value => {stderr += value;});
  const sample = () => {
    try {
      const current = fs.readFileSync(filename);
      if (!current.equals(previous)) verifyRecording(current);
      observations++;
    } catch (error) { observationError ||= error; }
  };
  sample();
  const timer = setInterval(sample, 2);
  const deadline = setTimeout(() => child.kill(), 20000);
  let status;
  try {
    status = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    });
  } finally {
    clearInterval(timer); clearTimeout(deadline);
  }
  sample();
  if (observationError) throw observationError;
  assert.ok(observations > 0, 'observed only the old complete recording or the new complete recording');
  return {status, stdout, stderr, combined: stdout + stderr};
}

try {
  write('project.zsettings', normalSettings);
  for (const [family, body] of [
    ['Speech', 'Speech.speak("must not speak"):'],
    ['Microphone', 'Print(Microphone.level):'],
    ['Speaker', 'Print(Speaker.getOutputDevice):'],
    ['Desktop', 'Desktop.openApp("must-not-open"):'],
  ]) rejectSyntax(`missing ${family} import`, body,
    new RegExp(`requires import ZSharp\\.${family}`), '');
  for (const [label, body, pattern] of [
    ['Speech arity', 'Speech.speak():', /too few arguments/i],
    ['Speech excessive arity', 'Speech.speak("a", "b", "c"):', /too many arguments/i],
    ['Speech prepare arity', 'Speech.prepare():', /too few arguments/i],
    ['Speech prepare excessive arity', 'Speech.prepare("a", "b"):', /too many arguments/i],
    ['Microphone setter arity', 'Microphone.setInputDevice("a", "b"):', /too many arguments/i],
    ['Desktop arity', 'Desktop.closeApp():', /too few arguments/i],
    ['focus arity', 'Desktop.focusWindow():', /too few arguments/i],
    ['type region required', 'Desktop.typeText("Hello"):', /too few arguments/i],
    ['shortcut excessive arity', 'Desktop.sendKeys("CTRL+S", "extra"):', /too many arguments/i],
    ['unknown operation', 'Speech.notAnOperation("x"):', /unknown desktop API operation/i],
    ['record duration unit', 'Microphone.record("none.wav", 5):', /duration requires s or ms/i],
  ]) rejectSyntax(label, body, pattern);

  write('Main.zsharp', source('Print(Microphone.level):'));
  const initial = success('initial level source', run(['run', sourcePath]));
  assert.match(initial.stdout.trim(), /^0(?:\.0+)?$/);
  assert.equal(Number(initial.stdout.trim()), 0);
  compile('level property serializes');
  const decoded = success('initial level bytecode', run(['run-bytecode', path.join(root, 'Main.zbc')]));
  assert.match(decoded.stdout.trim(), /^0(?:\.0+)?$/);
  assert.equal(Number(decoded.stdout.trim()), 0);
  const missingImport = mutateBytecode('MissingImport.zbc', data =>
    replaceEqual(data, 'ZSharp.Microphone', 'ZSharp.MissingMic'));
  failure('bytecode missing import', run(['run-bytecode', missingImport]), /requires import ZSharp\.Microphone/i);
  const unknownOperation = mutateBytecode('UnknownOperation.zbc', data =>
    replaceEqual(data, 'Microphone.level', 'Microphone.nope_'));
  failure('bytecode unknown operation', run(['run-bytecode', unknownOperation]), /desktop API|operation|invalid/i);
  const wrongArity = mutateBytecode('WrongArity.zbc', data => {
    const at = data.indexOf(Buffer.from('Microphone.level'));
    assert.ok(at >= 0); data.writeUInt32LE(1, at + Buffer.byteLength('Microphone.level'));
  });
  failure('bytecode arity validation', run(['run-bytecode', wrongArity]), /desktop API|argument|invalid/i);

  write('project.zsettings', normalSettings.replace('ZSharp: [1.4.0.0]:', 'ZSharp: [1.3.0.0]:'));
  for (const command of [['check', sourcePath], ['run', sourcePath],
    ['compile', sourcePath, '-o', path.join(root, 'Old.zbc')],
    ['run-bytecode', path.join(root, 'Main.zbc')]])
    failure(`desktop API minimum version: ${command[0]}`, run(command), /require.*1\.4\.0\.0/i);
  write('project.zsettings', normalSettings);

  write('Main.zsharp', source(`Microphone.setInputDevice(): Speaker.setOutputDevice():
    Print("INPUT"): Print(Microphone.getInputDevice):
    Print("OUTPUT"): Print(Speaker.getOutputDevice): Print(Microphone.level):`));
  for (const command of [['run', sourcePath], ['compile', sourcePath, '-o', path.join(root, 'Devices.zbc')],
    ['run-bytecode', path.join(root, 'Devices.zbc')]]) {
    const result = success(`dummy device/reset read: ${command[0]}`, run(command));
    if (command[0] !== 'compile') {
      const lines = result.stdout.trim().split(/\r?\n/);
      assert.equal(lines[0], 'INPUT'); assert.ok(lines[1].length > 0);
      assert.equal(lines[2], 'OUTPUT'); assert.ok(lines[3].length > 0);
      assert.equal(Number(lines[4]), 0);
    }
  }
  // A present device returns text, not null: this exact J.A.R.V.I.S guard
  // previously failed before entering the branch. Enumerate dummy devices only.
  write('Main.zsharp', source(`
    if[Microphone.getInputDevice != null] ( Print("MICROPHONE_PRESENT"): )
    if[null != Microphone.getInputDevice] ( Print("MICROPHONE_REVERSED"): )
    if[Speaker.getOutputDevice != null] ( Print("SPEAKER_PRESENT"): )
    Print(null == null): Print(null != null):
    Print("device" == null): Print("device" != null):
    Print(null == "device"): Print(null != "device"):
    Print("" == null): Print("" != null):
    Print(0 == null): Print(0 != null): Print(null == 0): Print(null != 0):
    Print(alive == null): Print(alive != null): Print(null == dead): Print(null != dead):
  `));
  for (const command of [['run', sourcePath], ['compile', sourcePath, '-o', path.join(root, 'NullableDevices.zbc')],
    ['run-bytecode', path.join(root, 'NullableDevices.zbc')]]) {
    const result = success(`nullable device/comparison read: ${command[0]}`, run(command));
    if (command[0] !== 'compile') assert.deepEqual(result.stdout.trim().split(/\r?\n/), [
      'MICROPHONE_PRESENT', 'MICROPHONE_REVERSED', 'SPEAKER_PRESENT',
      'alive', 'dead', 'dead', 'alive', 'dead', 'alive', 'dead', 'alive',
      'dead', 'alive', 'dead', 'alive', 'dead', 'alive', 'dead', 'alive',
    ]);
  }
  rejectRuntime('unrelated non-null types still cannot compare', 'Print("1" == 1):', /comparison requires values of the same type/i);
  rejectRuntime('null ordering remains invalid', 'Print(null < 1):', /number comparison requires two numbers/i);
  write('Main.zsharp', source('Microphone.setInputDevice(): Microphone.record("recorded.wav", 100ms):'));
  const recording = path.join(root, 'recorded.wav');
  success('valid dummy recording from source', run(['run', sourcePath]));
  const wave = fs.readFileSync(recording);
  verifyRecording(wave);
  compile('valid dummy recording serializes', 'Record.zbc');
  // An owned short WAV makes replacement distinguishable from the new complete
  // 100ms WAV. Observe while capture runs: never accept truncation or a prefix.
  const previous = Buffer.from(wave.subarray(0, 60));
  previous.writeUInt32LE(previous.length - 8, 4);
  previous.writeUInt32LE(previous.length - 44, 40);
  previous.fill(0x7f, 44);
  fs.writeFileSync(recording, previous);
  const filesBeforeRecording = fs.readdirSync(root).sort();
  success('valid dummy bytecode recording atomically replaces owned output',
    await observeAtomicRecording(['run-bytecode', path.join(root, 'Record.zbc')], recording, previous));
  verifyRecording(fs.readFileSync(recording));
  assert.deepEqual(fs.readdirSync(root).sort(), filesBeforeRecording, 'recording leaves no temporary output files');
  rejectRuntime('missing input device', 'Microphone.setInputDevice("fixture-missing-microphone-923734"):', /device.*not found|no.*device|missing.*device|device.*unavailable|device.*disconnected/i);
  rejectRuntime('missing output device', 'Speaker.setOutputDevice("fixture-missing-speaker-923734"):', /device.*not found|no.*device|missing.*device|device.*unavailable|device.*disconnected/i);
  for (const duration of ['0ms', '49ms', '31s'])
    rejectRuntime(`recording invalid duration ${duration}`, `Microphone.record("none.wav", ${duration}):`, /duration.*0\.05\.\.30|duration.*50|0\.05.*30/i);
  for (const filename of ['../outside.wav', path.join(root, 'absolute.wav').replaceAll('\\', '/')])
    rejectRuntime('recording path boundary', `Microphone.record(${JSON.stringify(filename)}, 50ms):`, /project-relative|travers|outside|invalid.*path|path.*invalid/i);
  assert.ok(!fs.existsSync(path.join(root, 'none.wav')) && !fs.existsSync(path.join(root, 'absolute.wav')));
  const junctionTarget = path.join(root, 'owned-junction-target');
  const junction = path.join(root, 'owned-junction');
  fs.mkdirSync(junctionTarget);
  fs.symlinkSync(junctionTarget, junction, process.platform === 'win32' ? 'junction' : 'dir');
  try {
    rejectRuntime('recording rejects linked parent before capture',
      'Microphone.record("owned-junction/escaped.wav", 50ms):', /symlink|symbolic|reparse|linked.*path/i);
    assert.ok(!fs.existsSync(path.join(junctionTarget, 'escaped.wav')));
  } finally {
    fs.unlinkSync(junction);
  }

  rejectRuntime('getInput missing model before capture', 'text Words = Microphone.getInput: Print(Words):',
    python ? /speech.*model|Vosk.*model|model.*directory|model.*accessible/i : /python|speech.*helper|runtime/i);
  if (python) {
    const standaloneHelpers = path.join(root, 'standalone-speech-helper');
    fs.mkdirSync(standaloneHelpers);
    fs.copyFileSync(path.join(repository, 'native/python/zsharp_speech_input.py'),
      path.join(standaloneHelpers, 'zsharp_speech_input.py'));
    const absentBridge = path.join(standaloneHelpers, 'zsharp_bridge.py');
    assert.ok(!fs.existsSync(absentBridge));
    rejectRuntime('speech helper does not require the language bridge',
      'text Words = Microphone.getInput: Print(Words):',
      /speech.*model|Vosk.*model|model.*directory|model.*accessible/i,
      {...environment, ZSHARP_PYTHON_BRIDGE: absentBridge, ZSHARP_SPEECH_PYTHON: python});
    assert.ok(!fs.existsSync(absentBridge));
  }
  rejectRuntime('empty speech home fails before capture', 'text Words = Microphone.getInput:',
    /ZSHARP_SPEECH_HOME must.*absolute/i, {...environment, ZSHARP_SPEECH_HOME: ''});
  rejectRuntime('empty speech Python fails before capture', 'text Words = Microphone.getInput:',
    /ZSHARP_SPEECH_PYTHON must.*Python executable/i, {...environment, ZSHARP_SPEECH_PYTHON: ''});
  rejectRuntime('Speech strict text argument', 'Speech.speak(123):', /argument 1 requires text/i);
  rejectRuntime('Speech missing preset', 'Speech.speak("must not speak", "Missing.zvoice"):', /voice|file|read|path/i);
  rejectRuntime('Speech prepare strict text', 'Speech.prepare(123):', /argument 1 requires text/i);
  rejectRuntime('Speech prepare missing voice', 'Speech.prepare("Missing.zvoice"):', /voice|file|read|path/i);
  if (process.platform === 'win32') {
    // SAPI synthesizes into a private WAV; all playback is forced through the
    // SDL dummy driver, never a physical speaker. No Python/model is involved.
    write('Fixture.zvoice', JSON.stringify({format: 'ZVOICE', version: 1,
      type: 'preset', backend: 'system', voice: '', pitch: 105, speed: 150, volume: 5}));
    write('Main.zsharp', source(`Speaker.setOutputDevice():
      Speech.prepare("Fixture.zvoice"):
      Speech.speak("Ready."):
      Speech.speak("A < B & C > D.", "Fixture.zvoice"):
      Print("native speech queued"):`));
    for (const command of [['run', sourcePath],
      ['compile', sourcePath, '-o', path.join(root, 'Speak.zbc')],
      ['run-bytecode', path.join(root, 'Speak.zbc')]]) {
      const result = success(`Windows native default/preset speech with dummy output: ${command[0]}`, run(command));
      if (command[0] !== 'compile') assert.equal(result.stdout.trim(), 'native speech queued');
    }
    const library = path.join(root, 'speech-library');
    const consumer = path.join(root, 'speech-consumer');
    fs.mkdirSync(library);
    fs.mkdirSync(path.join(consumer, 'Dependencies'), {recursive: true});
    fs.writeFileSync(path.join(library, 'project.zsettings'),
      normalSettings.replace('system_api_fixture', 'speech_library_fixture'));
    fs.writeFileSync(path.join(library, 'LibraryOnly.zvoice'), JSON.stringify({
      format: 'ZVOICE', version: 1, type: 'preset', backend: 'system', speed: 150}));
    fs.writeFileSync(path.join(library, 'Main.zsharp'), `zsharp = type.script
noticed room Library[] (
 import ZSharp.Speech():
 noticed brain Speak[] (
  Speech.speak("Library.", "LibraryOnly.zvoice"):
  Print("library speech queued"):
 )
)
`);
    success('package owned local speech dependency', run(['package', 'pack', library, 'SpeechLibrary']));
    fs.copyFileSync(path.join(library, 'Packages', 'SpeechLibrary.zpackage'),
      path.join(consumer, 'Dependencies', 'SpeechLibrary.zpackage'));
    fs.writeFileSync(path.join(consumer, 'project.zsettings'), normalSettings
      .replace('system_api_fixture', 'speech_consumer_fixture')
      .replace('Dependencies ():', 'Dependencies ( speech_library_fixture:1.4.0.0: ):'));
    const consumerSource = path.join(consumer, 'Main.zsharp');
    const consumerBytecode = path.join(consumer, 'Main.zbc');
    fs.writeFileSync(consumerSource, source(`Speech.speak("Consumer."):
      Function.call(speech_library_fixture:Main:Library:Speak):
      Print("consumer speech queued"):`).replace(' import ZSharp.Speech():',
        ' import ZSharp.Speech():\n import speech_library_fixture.Main():'));
    assert.ok(!fs.existsSync(path.join(consumer, 'LibraryOnly.zvoice')));
    for (const command of [['run', consumerSource],
      ['compile', consumerSource, '-o', consumerBytecode], ['run-bytecode', consumerBytecode]]) {
      const result = success(`speech services retain the owning dependency root: ${command[0]}`, run(command));
      assert.doesNotMatch(result.combined, /downloading dependency/i);
      if (command[0] !== 'compile') {
        assert.match(result.stdout, /library speech queued/);
        assert.match(result.stdout, /consumer speech queued/);
      }
    }
    assert.ok(fs.existsSync(path.join(consumer, 'Dependencies', 'speech_library_fixture',
      '1.4.0.0', 'LibraryOnly.zvoice')));
    assert.ok(!fs.existsSync(path.join(consumer, 'LibraryOnly.zvoice')));
  }
  rejectRuntime('createVoice missing reference', 'Speech.createVoice("Missing.mp3", "Created.zvoice"):', /voice|reference|file|read|path/i);
  assert.ok(!fs.existsSync(path.join(root, 'Created.zvoice')));
  for (const body of ['Desktop.openApp("must-not-open"):', 'Desktop.closeApp(999999):',
    'Desktop.minimizeApp(999999):', 'Desktop.maximizeApp(999999):',
    'Desktop.scaleApp(999999, 640, 480):', 'Desktop.moveApp(999999, 0):',
    'Desktop.focusWindow(999999):', 'Desktop.typeText("Hello\\nWorld", "center"):',
    'Desktop.sendKeys("CTRL+SHIFT+S"):', 'Desktop.sendKeys("MEDIA_PLAY_PAUSE"):',
    'Desktop.sendKeys("MEDIA_NEXT"):', 'Desktop.sendKeys("MEDIA_PREVIOUS"):'])
    rejectRuntime('Desktop side-effect guard', body, /desktop actions are disabled/i);
  rejectRuntime('invalid typing region', 'Desktop.typeText("Hello", "unknown"):', /unknown typeText region/i);
  rejectRuntime('invalid shortcut', 'Desktop.sendKeys("CTRL++S"):', /invalid keyboard shortcut/i);
  rejectRuntime('media modifiers rejected', 'Desktop.sendKeys("CTRL+MEDIA_NEXT"):', /invalid keyboard shortcut/i);
  rejectRuntime('text requires text', 'Desktop.typeText(123, "center"):', /argument 1 requires text/i);
  rejectRuntime('shortcut requires text', 'Desktop.sendKeys(123):', /argument 1 requires text/i);

  // Registration lifecycle is tested exclusively in an owned directory; remove
  // the skip guard only for these explicit isolated commands, never globally.
  const registrationEnv = {...environment};
  delete registrationEnv.ZSHARP_SKIP_DESKTOP_INTEGRATION;
  const alive = `${normalSettings}\nLaunchAtStart: alive:\n`;
  const dead = `${normalSettings}\nLaunchAtStart: dead:\n`;
  write('project.zsettings', alive);
  write('Main.zsharp', source('Print("startup fixture ran"):'));
  for (const command of [['check', path.join(root, 'project.zsettings')], ['check', sourcePath],
    ['compile', sourcePath, '-o', path.join(root, 'Startup.zbc')]]) {
    success(`startup alive no compile write: ${command[0]}`, run(command, registrationEnv));
    assert.deepEqual(fs.readdirSync(startup), []);
  }
  success('actual source launch creates isolated startup', run(['run', sourcePath], registrationEnv));
  const entries = fs.readdirSync(startup);
  assert.equal(entries.length, 1); const registration = path.join(startup, entries[0]);
  assert.match(fs.readFileSync(registration, 'utf8'), /Main\.zsharp/);
  success('actual bytecode launch updates isolated startup', run(['run-bytecode', path.join(root, 'Startup.zbc')], registrationEnv));
  assert.deepEqual(fs.readdirSync(startup), entries);
  assert.match(fs.readFileSync(registration, 'utf8'), /run-bytecode/);
  assert.match(fs.readFileSync(registration, 'utf8'), /Startup\.zbc/);
  write('Second.zsharp', source('Print("second startup entry"):'));
  success('moved source updates same startup identity', run(['run', path.join(root, 'Second.zsharp')], registrationEnv));
  assert.deepEqual(fs.readdirSync(startup), entries);
  assert.match(fs.readFileSync(registration, 'utf8'), /Second\.zsharp/);
  write('project.zsettings', normalSettings);
  const preserved = fs.readFileSync(registration, 'utf8');
  success('omitted startup setting preserves existing entry', run(['run', sourcePath], registrationEnv));
  assert.equal(fs.readFileSync(registration, 'utf8'), preserved);
  write('project.zsettings', dead);
  success('startup dead check makes no changes', run(['check', path.join(root, 'project.zsettings')], registrationEnv));
  compile('startup dead compile makes no changes', 'StartupDead.zbc', registrationEnv);
  assert.equal(fs.readFileSync(registration, 'utf8'), preserved);
  fs.writeFileSync(path.join(startup, 'unrelated.fixture'), 'must remain untouched');
  success('actual dead launch removes only owned entry', run(['run', sourcePath], registrationEnv));
  assert.deepEqual(fs.readdirSync(startup), ['unrelated.fixture']);
  write('project.zsettings', alive);
  success('skip guard prevents actual launch registration', run(['run', sourcePath]));
  assert.deepEqual(fs.readdirSync(startup), ['unrelated.fixture']);
  for (const [label, settingsText, pattern] of [
    ['duplicate startup setting', `${alive}LaunchAtStart: dead:\n`, /duplicate LaunchAtStart/i],
    ['invalid startup value', `${normalSettings}LaunchAtStart: true:\n`, /expects alive or dead/i],
    ['startup minimum version', alive.replace('ZSharp: [1.4.0.0]:', 'ZSharp: [1.3.0.0]:'), /LaunchAtStart requires.*1\.4\.0\.0/i],
  ]) {
    write('project.zsettings', settingsText);
    failure(label, run(['check', path.join(root, 'project.zsettings')], registrationEnv), pattern);
    assert.deepEqual(fs.readdirSync(startup), ['unrelated.fixture']);
  }
  console.log(`PASS: ${checks} system API CLI/source/bytecode/startup checks; dummy audio and owned fixtures only${python ? '' : '; model preflight used missing-runtime path (no Python found)'}`);
} finally {
  // Validate the exact newly created absolute directory before recursive cleanup.
  const resolved = fs.realpathSync(root);
  assert.equal(path.dirname(resolved), temporaryParent);
  assert.ok(path.basename(resolved).startsWith('zsharp-system-api-'));
  fs.rmSync(resolved, {recursive: true, force: true});
}
