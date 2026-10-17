import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

// Exercise the real application scripts with the development VM, but replace
// only device/scene boundaries with deterministic variables. No window,
// controller, package rebuild, network connection, or project write is needed.
const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const project = path.resolve(process.argv[2] ?? 'E:/The Pallet');
const scripts = fs.existsSync(path.join(project, 'Scripts')) ? path.join(project, 'Scripts') : project;
const executable = path.join(repository, 'build', 'Release', 'zsharp.exe');
const movement = fs.readFileSync(path.join(scripts, 'Movement.zsharp'), 'utf8');
const interaction = fs.readFileSync(path.join(scripts, 'InteractionSystem.zsharp'), 'utf8');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-pallet-input-'));
let fixtureIndex = 0;

function replaceOnce(source, pattern, replacement, description) {
  let count = 0;
  const result = source.replace(pattern, (...args) => {
    count++;
    return typeof replacement === 'function' ? replacement(...args) : replacement;
  });
  assert.equal(count, 1, `Cannot identify the real ${description}; update this fixture adapter.`);
  return result;
}

function withoutImports(source) {
  return source.replace(/^\s*import\s+[^\r\n]+:\s*$/gm, '');
}

function run(source, filename = 'Movement.zsharp') {
  const directory = path.join(temporary, String(++fixtureIndex));
  fs.mkdirSync(directory);
  const filenameOnDisk = path.join(directory, filename);
  fs.writeFileSync(filenameOnDisk, source);
  fs.writeFileSync(path.join(directory, 'project.zsettings'), `zsharp = type.settings
Project: "Pallet Input Regression":
PID: "pallet_input_regression":
Version: [1.0.0.0]:
Authors: ["ZombieOS"]:
Description: "Deterministic input regression fixture.":
ZSharp: [1.3.0.0]:
Dependencies (
):
`);
  const result = spawnSync(executable, ['run', filenameOnDisk], {
    cwd: directory, encoding: 'utf8', timeout: 15000,
  });
  assert.equal(result.status, 0,
    `Fixture ${fixtureIndex} must execute successfully, not merely compile.\n${result.error ?? ''}\n${result.stderr}${result.stdout}`);
  return result.stdout;
}

// Match parentheses while ignoring strings/comments so injected statements
// follow the real drain loop rather than a nearby nested if block.
function closingParenthesis(source, opening) {
  assert.equal(source[opening], '(');
  let depth = 0, quote = false, comment = false;
  for (let i = opening; i < source.length; i++) {
    const character = source[i];
    if (comment) { if (character === '\n') comment = false; continue; }
    if (quote) {
      if (character === '\\') i++;
      else if (character === '"') quote = false;
      continue;
    }
    if (character === '/' && source[i + 1] === '/') { comment = true; i++; continue; }
    if (character === '"') { quote = true; continue; }
    if (character === '(') depth++;
    if (character === ')' && --depth === 0) return i;
  }
  assert.fail('Unterminated source block in fixture adapter.');
}

function removeEmptyElse(source, condition) {
  const start = source.indexOf(`if[${condition}]`);
  assert.notEqual(start, -1, `Expected real guard ${condition}`);
  const opening = source.indexOf('(', start + `if[${condition}]`.length);
  const ending = closingParenthesis(source, opening) + 1;
  const suffix = source.slice(ending);
  const emptyElse = /^\s*else\s*\(\s*\)/.exec(suffix);
  assert(emptyElse, `The guard ${condition} needs its explicit empty else().`);
  return source.slice(0, ending) + suffix.slice(emptyElse[0].length);
}

const fieldPattern = /\b(?:Gamepad|Game|input|Player|Chapter1Floor[12]|flashlightclick)\.[A-Za-z0-9_.]+/g;
function fieldKind(field) {
  if (field === 'Game.scene') return 'text';
  if (field.startsWith('input.key.') || field === 'input.mouse.captured' ||
      field === 'Player.grounded' || field === 'flashlightclick.audioPlay' ||
      /(?:connected|pressed|held|released)$/.test(field)) return 'status';
  return 'number';
}

const alias = field => `Mock_${field.replaceAll('.', '_')}`;
function assignment(field, value) {
  const kind = fieldKind(field);
  const literal = kind === 'text' ? JSON.stringify(value) :
    kind === 'status' ? (value ? 'alive' : 'dead') : String(value);
  return `${kind === 'number' ? 'number.set:' : ''}${alias(field)} = ${literal}:`;
}

function movementFixture(source, floor, mode) {
  let adapted = withoutImports(source);
  const fields = [...new Set([...adapted.matchAll(fieldPattern)]
    .map(match => match[0].replace(/\.set$/, '')))];
  assert(fields.includes('input.key.w') && fields.includes('Gamepad.left.stick.y'));
  const declarations = fields.map(field => {
    const kind = fieldKind(field);
    const initial = kind === 'text' ? '""' : kind === 'status' ? 'dead' : '0';
    return `noticed ${kind} ${alias(field)} = ${initial}:`;
  }).join('\n');

  // Convert runtime property setters to assignments of the mock field while
  // leaving arithmetic, input composition, guard structure and Math calls intact.
  for (const field of [...fields].sort((a, b) => b.length - a.length)) {
    adapted = adapted.replaceAll(`${field}.set:`,
      `${fieldKind(field) === 'number' ? 'number.set:' : ''}${alias(field)} =`);
  }
  adapted = adapted.replace(fieldPattern, field => {
    assert(fields.includes(field), `Unadapted runtime field ${field}`);
    return alias(field);
  });

  const keyboard = mode !== 'controller';
  const controller = mode !== 'keyboard';
  const elapsed = [0.008, 0.004, 0.2]; // Normal, negative and excessive polling deltas.
  const frames = elapsed.map((time, frame) => {
    const values = {
      'Game.elapsed': time, 'Game.fps': 120, 'Game.scene': `Chapter1Floor${floor}`,
      'Player.positionX': 2, 'Player.positionY': 3, 'Player.positionZ': 4,
      'Player.velocityY': 0, 'Player.grounded': true,
      'input.mouse.deltaX': keyboard ? 2 : 0,
      'input.mouse.deltaY': keyboard ? -1 : 0,
      'input.key.w': keyboard && frame === 0,
      'input.key.d': keyboard && frame === 1,
      'input.key.s': keyboard && frame === 2,
      'input.key.a': keyboard && frame === 2,
      'input.key.lshift': keyboard && frame === 1,
      'input.key.rshift': keyboard && frame === 2,
      'input.key.space': frame === 1 || (controller && frame === 2),
      'input.key.f': keyboard && frame >= 1,
      'Gamepad.connected': controller,
      'Gamepad.left.stick.pressed': controller && frame === 1,
      'Gamepad.left.stick.x': controller && frame === 1 ? 0.6 : 0,
      'Gamepad.left.stick.y': controller ? [0.5, 0.8, 0][frame] : 0,
      'Gamepad.left.stick.magnitude': controller ? [0.5, 1, 0][frame] : 0,
      'Gamepad.right.stick.x': controller ? [0.25, -0.4, 0][frame] : 0,
      'Gamepad.right.stick.y': controller ? [-0.5, 0.2, 0][frame] : 0,
      'Gamepad.south.pressed': controller && frame === 1,
      'Gamepad.south.held': controller && frame >= 1,
      'Gamepad.north.pressed': controller && frame === 1,
      'flashlightclick.audioPlay': false,
    };
    return Object.entries(values).filter(([field]) => fields.includes(field))
      .map(([field, value]) => assignment(field, value)).join('\n');
  });
  const configureFrame = frames.map((frame, index) =>
    index === 0 ? `if[TestIteration == 0] (\n${frame}\n)` :
      index === 1 ? ` else if[TestIteration == 1] (\n${frame}\n)` :
        ` else (\n${frame}\n)`).join('');
  adapted = replaceOnce(adapted, /noticed\s+brain\s+Start\s*\[\]\s*\(/g,
    `${declarations}\nnoticed number TestIteration = 0:\nnoticed brain Start[] (`, 'Movement.Start declaration');
  adapted = replaceOnce(adapted, /\bloop\s*\(/g,
    `loop (\n${configureFrame}\n`, 'Movement polling loop');
  const camera = suffix => alias(`Chapter1Floor${floor}.${suffix}`);
  const report = [
    'TestIteration', 'ControllerDelta', alias('Player.velocityX'), alias('Player.velocityY'),
    alias('Player.velocityZ'), camera('cameraRotationX'), camera('cameraRotationY'),
    'Speed', 'FlashlightOn', 'FlashlightKeyDown', alias('input.mouse.captured'),
    'ControllerSprint', alias('flashlightclick.audioPlay'),
    camera('cameraX'), camera('cameraY'), camera('cameraZ'),
    alias(`Chapter1Floor${floor}.Flashlight.lightIntensity`),
    'Length',
  ];
  const tail = `Print("INPUT_FRAME|" + ${report.join(' + "|" + ')}):
   number.set:TestIteration = TestIteration + 1:
   if[TestIteration >= 3] ( loop.end: ) else ()`;
  adapted = replaceOnce(adapted, /\bwait\s*\(\s*8ms\s*\)\s*:/g, tail, 'Movement wait boundary');
  return adapted;
}

function framesFrom(output) {
  return [...output.matchAll(/INPUT_FRAME\|([^\r\n]+)/g)]
    .map(match => match[1].trim().split('|'));
}

function near(actual, expected, description) {
  assert(Number.isFinite(Number(actual)), `${description} was not numeric: ${actual}`);
  assert(Math.abs(Number(actual) - expected) < 0.0002,
    `${description}: expected ${expected}, received ${actual}`);
}

function verifyMovement(source, floor, mode) {
  const frames = framesFrom(run(movementFixture(source, floor, mode)));
  assert.equal(frames.length, 3, `Floor ${floor}, ${mode}: the polling brain stopped at a false guard.`);
  const keyboard = mode !== 'controller', controller = mode !== 'keyboard';
  let pitch = 0, yaw = 0;
  for (let frame = 0; frame < 3; frame++) {
    const values = frames[frame], delta = [0.008, 0, 0.05][frame];
    near(values[0], frame, 'poll iteration');
    near(values[1], delta, 'clamped task delta');
    pitch += (keyboard ? 0.08 : 0) + (controller ? [-0.5, 0.2, 0][frame] * 150 * delta : 0);
    yaw -= (keyboard ? 0.16 : 0) + (controller ? [0.25, -0.4, 0][frame] * 150 * delta : 0);
    yaw = (yaw + 360) % 360;
    near(values[5], pitch, 'keyboard/controller camera pitch');
    near(values[6], yaw, 'keyboard/controller camera yaw');
    let forward = (keyboard ? [1, 0, -1][frame] : 0) + (controller ? [0.5, 0.8, 0][frame] : 0);
    let strafe = (keyboard ? [0, 1, -1][frame] : 0) + (controller && frame === 1 ? 0.6 : 0);
    const speed = (keyboard && frame >= 1) || (controller && frame === 1) ? 30 : 15;
    const radians = -yaw * Math.PI / 180;
    let moveX = Math.sin(radians) * forward + Math.cos(radians) * strafe;
    let moveZ = -Math.cos(radians) * forward + Math.sin(radians) * strafe;
    near(values[17], Math.hypot(moveX, moveZ), 'combined input length');
    // The current Z# number rules intentionally retain one fractional digit
    // for repeating division. The real script normalizes after yaw rotation.
    // Use the independently checked VM length for its >1 boundary, including
    // tiny Math rounding differences around an exactly full-length stick.
    const length = Number(values[17]);
    if (length > 1) {
      moveX = Math.trunc(moveX / length * 10) / 10;
      moveZ = Math.trunc(moveZ / length * 10) / 10;
    }
    near(values[2], moveX * speed, 'movement X');
    near(values[4], moveZ * speed, 'movement Z');
    // Frame two keeps the controller's legacy Space held, but must not autojump.
    near(values[3], frame === 1 ? 10 : 0, 'keyboard/controller jump');
    near(values[7], speed, 'keyboard Shift/controller sprint');
    assert.equal(values[8], frame === 0 ? 'dead' : 'alive', 'flashlight must toggle once');
    assert.equal(values[9], keyboard && frame >= 1 ? 'alive' : 'dead', 'keyboard F latch');
    assert.equal(values[10], 'alive', 'desktop mouse capture');
    assert.equal(values[11], controller && frame === 1 ? 'alive' : 'dead', 'controller sprint toggle/reset');
    assert.equal(values[12], frame === 1 ? 'alive' : 'dead', 'flashlight audio edge');
    near(values[13], 2, 'camera follows player X');
    near(values[14], 4.6, 'camera follows player eye height');
    near(values[15], 4, 'camera follows player Z');
    near(values[16], frame === 0 ? 0 : 300, 'flashlight intensity');
  }
}

function interactionFixture(source) {
  let adapted = withoutImports(source);
  adapted = adapted.replaceAll('input.key.e', 'MockE')
    .replaceAll('Gamepad.right.trigger.pressed',
      '(Function.call(InteractionSystem:InteractionSystem:NextTrigger []) > 0)');
  adapted = replaceOnce(adapted, /\bwait\s*\(\s*2s\s*\)\s*:/g, '// Mock cooldown elapsed.\n', 'interaction cooldown');
  const loop = /\bloop\s*\(/.exec(adapted);
  assert(loop, 'Cannot identify the real interaction trigger-drain loop.');
  const opening = adapted.indexOf('(', loop.index);
  const ending = closingParenthesis(adapted, opening) + 1;
  adapted = adapted.slice(0, ending) + '\nnumber.set:DrainCompleted = DrainCompleted + 1:\n' + adapted.slice(ending);
  const setup = `
   noticed status MockE = dead:
   noticed number QueuedPresses = 0:
   noticed number Reads = 0:
   noticed number ExecutedCount = 0:
   noticed number DrainCompleted = 0:
   noticed number NextTrigger[] (
    number.set:Reads = Reads + 1:
    if[QueuedPresses > 0] (
     number.set:QueuedPresses = QueuedPresses - 1:
     feed(1):
    ) else ( feed(0): )
   )
   noticed brain Executed[] ( number.set:ExecutedCount = ExecutedCount + 1: )
   noticed brain Start[] (
    MockE = alive:
    number.set:QueuedPresses = 3:
    Function.call(InteractionSystem:InteractionSystem:Interactable ["Test", "InteractionSystem:InteractionSystem:Executed", "Test", 2, 1, 1, dead]):
    Print("INTERACTION|keyboard|" + ExecutedCount + "|" + DrainCompleted + "|" + Reads + "|" + QueuedPresses):
    MockE = dead:
    number.set:QueuedPresses = 3:
    number.set:Reads = 0:
    number.set:ExecutedCount = 0:
    number.set:DrainCompleted = 0:
    Function.call(InteractionSystem:InteractionSystem:Interactable ["Test", "InteractionSystem:InteractionSystem:Executed", "Test", 2, 1, 1, alive]):
    Print("INTERACTION|controller|" + ExecutedCount + "|" + DrainCompleted + "|" + Reads + "|" + QueuedPresses):
   )
  `;
  return replaceOnce(adapted, /noticed\s+room\s+InteractionSystem\s*\[\]\s*\(/g,
    `noticed room InteractionSystem[] (\n${setup}`, 'InteractionSystem room');
}

try {
  for (const floor of [1, 2]) for (const mode of ['keyboard', 'controller', 'mixed'])
    verifyMovement(movement, floor, mode);
  let output = run(interactionFixture(interaction), 'InteractionSystem.zsharp');
  for (const mode of ['keyboard', 'controller'])
    assert.match(output, new RegExp(`INTERACTION\\|${mode}\\|1(?:\\.0+)?\\|1(?:\\.0+)?\\|4(?:\\.0+)?\\|0(?:\\.0+)?`),
      `${mode}: cooldown drain returned early instead of consuming queued edges.`);

  // Negative controls prove that this regression catches the actual historical
  // guard behavior, rather than merely checking that source text contains else.
  for (const [condition, floor] of [
    ['ControllerDelta > 0.05', 1],
    ['ControllerSprintPressed == alive', 1],
    ['Gamepad.left.stick.magnitude == 0', 1],
    ['(input.key.f == alive and FlashlightKeyDown == dead) or ControllerFlashlightPressed == alive', 1],
    ['(input.key.f == alive and FlashlightKeyDown == dead) or ControllerFlashlightPressed == alive', 2],
  ]) {
    // The two flashlight guards have the same condition; target the matching
    // floor's scene branch when selectively removing the fixed fallback.
    let broken;
    if (floor === 2) {
      const scene = movement.indexOf('else if[Game.scene == "Chapter1Floor2"]');
      assert(scene >= 0);
      broken = movement.slice(0, scene) + removeEmptyElse(movement.slice(scene), condition);
    } else broken = removeEmptyElse(movement, condition);
    const frames = framesFrom(run(movementFixture(broken, floor, 'controller')));
    assert.equal(frames.length, 0, `Negative control did not detect ${condition} on floor ${floor}.`);
  }
  const brokenInteraction = removeEmptyElse(interaction, 'Gamepad.right.trigger.pressed == dead');
  output = run(interactionFixture(brokenInteraction), 'InteractionSystem.zsharp');
  assert.match(output, /INTERACTION\|controller\|1(?:\.0+)?\|0(?:\.0+)?\|1(?:\.0+)?\|2(?:\.0+)?/);
  console.log('PASS: real Pallet polling loops, both floors, keyboard/controller/mixed movement, camera, sprint, jump, flashlight, interaction drain, and six historical negative controls');
} finally {
  fs.rmSync(temporary, {recursive: true, force: true});
}
