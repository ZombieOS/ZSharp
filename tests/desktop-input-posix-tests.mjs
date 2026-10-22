import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Exercise the exact constant JXA shipped by the macOS backend, not a second
// implementation. Every OS/UI bridge below is an in-memory mock: this test
// cannot focus, click, type, request permission, or launch any application.
const source = readFileSync(new URL('../native/src/desktop_api.c', import.meta.url), 'utf8');
const begin = source.indexOf('static int desktop_macos_input(');
assert(begin >= 0);
const declaration = source.indexOf('static const char script[] =', begin);
const end = source.indexOf('\n        "}\\n";', declaration);
assert(end > declaration);
const literals = source.slice(declaration, end + '\n        "}\\n";'.length)
  .split(/\r?\n/)
  .map(line => line.trim().match(/^("(?:[^"\\]|\\.)*")/))
  .filter(Boolean)
  .map(match => JSON.parse(match[1]));
const script = literals.join('');
assert(script.includes("function run(a)"));
new vm.Script(script);

let assertions = 0;
function checked(callback) { callback(); assertions++; }

function backend(options = {}) {
  const state = {
    events: [], releases: 0, eventReleases: 0, frontmost: true, pid: 1234, windowId: 1,
    minimized: true, raised: false, imports: [], mediaAllocations: [],
  };
  const window = {
    position: () => [100, 200], size: () => [600, 300],
    attributes: { byName: () => new Proxy({}, { set(_object, key, value) {
      if (key === 'value') state.minimized = value;
      return true;
    } }) },
    actions: { byName: () => ({ perform: () => { state.raised = true; } }) },
  };
  const process = new Proxy({
    windows: () => options.noWindow ? [] : [window],
    unixId: () => 1234,
    attributes: { byName: name => {
      assert.equal(name, 'AXFocusedWindow');
      return { value: () => options.noWindow ? null : window };
    } },
  }, {
    get(object, key) { return key === 'frontmost' ? () => state.frontmost : object[key]; },
    set(object, key, value) {
      if (key === 'frontmost') state.frontmost = value;
      else object[key] = value;
      return true;
    },
  });
  const quartz = text => ({ dataUsingEncoding: () => ({ bytes: text }) });
  Object.assign(quartz, {
    AXIsProcessTrusted: () => !options.permissionDenied,
    CGEventSourceFlagsState: () => options.heldFlags ?? 0,
    CGEventSourceButtonState: (_source, button) => options.heldButton === button,
    AXUIElementCreateApplication: pid => ({ kind: 'application', pid }),
    AXUIElementCopyAttributeValue: (_application, _name, ref) => {
      if (options.noWindow) return -1;
      ref[0] = { kind: 'window', id: state.windowId };
      return 0;
    },
    CFEqual: (left, right) => left.kind === right.kind && left.id === right.id,
    CGPointMake: (x, y) => ({ x, y }),
    NSMakePoint: (x, y) => ({ x, y }),
    NSEvent: {
      otherEventWithTypeLocationModifierFlagsTimestampWindowNumberContextSubtypeData1Data2:
        (type, point, flags, timestamp, windowNumber, context, subtype, data1, data2) => {
          state.mediaAllocations.push({ type, point, flags, timestamp, windowNumber, context, subtype, data1, data2 });
          assert.equal(state.events.length, 0, 'both media events must be allocated before either is posted');
          if (options.focusChangesBeforeMedia) state.pid = 9999;
          if (options.failMediaFlags === flags) return null;
          return { CGEvent: options.failMediaConversion === flags ? null : {
            type: 'systemDefined', subtype, flags, data1, data2,
          } };
        },
    },
    CGEventCreateMouseEvent: (_source, type, point) => options.failMouseEvent === type ? null : ({ type, point }),
    CGEventCreateKeyboardEvent: (_source, code, down) => {
      if (options.failKeyboardUp && !down) return null;
      return { type: down ? 'keyDown' : 'keyUp', code };
    },
    CGEventSetFlags: (event, flags) => { event.flags = flags; },
    CGEventKeyboardSetUnicodeString: (event, length, bytes) => {
      event.text = bytes; event.length = length;
    },
    CGEventPost: (_tap, event) => {
      state.events.push({ ...event });
      if (options.focusChangesAfterClick && event.type === 2) state.pid = 9999;
      if (options.windowChangesAfterClick && event.type === 2) state.windowId = 9999;
    },
    CFRelease: object => {
      state.releases++;
      if ('type' in object) state.eventReleases++;
    },
    NSUTF16LittleEndianStringEncoding: 0x94000100,
    NSWorkspace: { sharedWorkspace: { frontmostApplication: {
      get processIdentifier() { return state.pid; },
    } } },
  });
  const context = vm.createContext({
    ObjC: { import: name => state.imports.push(name) },
    Ref: () => [],
    $: quartz,
    Application(name) {
      assert.equal(name, 'System Events');
      return { applicationProcesses: { whose(criteria) {
        if ('frontmost' in criteria) return () => [process];
        return () => options.ambiguous ? [process, process] : [process];
      } } };
    },
  });
  new vm.Script(script).runInContext(context);
  return { state, run: args => context.run(args) };
}

for (let row = 0; row < 3; row++) {
  for (let column = 0; column < 3; column++) {
    const test = backend();
    test.run(['typeText', 'Hello', String(row), String(column), '0', '0']);
    checked(() => assert.deepEqual(test.state.events.slice(0, 3).map(event => event.type), [5, 1, 2]));
    checked(() => assert.deepEqual(JSON.parse(JSON.stringify(test.state.events[0].point)), {
      x: Math.floor(100 + 599 * (2 * column + 1) / 6),
      y: Math.floor(200 + 299 * (2 * row + 1) / 6),
    }));
    checked(() => assert.equal(test.state.events.filter(event => event.type === 'keyDown').map(event => event.text).join(''), 'Hello'));
  }
}

{
  const test = backend();
  const text = '\";throw Error(\"must stay literal\");// 🧪\r\n\t';
  test.run(['typeText', text, '1', '1', '0', '0']);
  const downs = test.state.events.filter(event => event.type === 'keyDown');
  checked(() => assert.equal(downs.filter(event => event.text !== undefined).map(event => event.text).join(''), text.slice(0, -3)));
  checked(() => assert.equal(downs.find(event => event.text === '🧪').length, 2));
  checked(() => assert.deepEqual(downs.slice(-2).map(event => event.code), [36, 48]));
  checked(() => assert.equal(test.state.eventReleases, test.state.events.length));
}

{
  const test = backend();
  test.run(['sendKeys', 'CTRL+S', '0', '0', '1', '1']);
  checked(() => assert.deepEqual(test.state.events.map(event => [event.code, event.flags]), [[1, 262144], [1, 262144]]));
}

for (const code of [16, 17, 18]) {
  const test = backend();
  test.run(['sendKeys', 'literal media name', '0', '0', String(1000 + code), '0']);
  checked(() => assert.deepEqual(test.state.events.map(event => ({ ...event })), [
    { type: 'systemDefined', subtype: 8, flags: 2560, data1: (code << 16) | 2560, data2: -1 },
    { type: 'systemDefined', subtype: 8, flags: 2816, data1: (code << 16) | 2816, data2: -1 },
  ]));
  checked(() => assert.equal(test.state.mediaAllocations.length, 2));
  checked(() => assert.equal(test.state.eventReleases, 0, 'borrowed CGEvents are owned by their NSEvent wrappers'));
}

for (const modifiers of ['1', '2', '4', '8']) {
  const test = backend();
  checked(() => assert.throws(() => test.run(['sendKeys', 'MEDIA_NEXT', '0', '0', '1017', modifiers]), /cannot be combined/));
  checked(() => assert.equal(test.state.mediaAllocations.length, 0));
  checked(() => assert.equal(test.state.events.length, 0));
}

for (const failMediaFlags of [2560, 2816]) {
  const test = backend({ failMediaFlags });
  checked(() => assert.throws(() => test.run(['sendKeys', 'MEDIA_NEXT', '0', '0', '1017', '0']), /allocate.*media key event/));
  checked(() => assert.equal(test.state.events.length, 0));
}

for (const failMediaConversion of [2560, 2816]) {
  const test = backend({ failMediaConversion });
  checked(() => assert.throws(() => test.run(['sendKeys', 'MEDIA_PREVIOUS', '0', '0', '1018', '0']), /convert.*media key event/));
  checked(() => assert.equal(test.state.events.length, 0));
}

{
  const test = backend({ focusChangesBeforeMedia: true });
  checked(() => assert.throws(() => test.run(['sendKeys', 'MEDIA_PLAY_PAUSE', '0', '0', '1016', '0']), /focused application changed/));
  checked(() => assert.equal(test.state.events.length, 0));
}

// The Linux dispatcher must use consumer keysyms, never type their names as
// text or reinterpret caller text as shell syntax. No X11 connection is made.
for (const [symbol, keysym] of [
  ['DESKTOP_KEY_MEDIA_PLAY_PAUSE', 'XF86AudioPlay'],
  ['DESKTOP_KEY_MEDIA_NEXT', 'XF86AudioNext'],
  ['DESKTOP_KEY_MEDIA_PREVIOUS', 'XF86AudioPrev'],
]) {
  checked(() => assert(source.includes(`case ${symbol}: return "${keysym}";`)));
}

for (const heldFlags of [131072, 262144, 524288, 1048576]) {
  const test = backend({ heldFlags });
  checked(() => assert.throws(() => test.run(['sendKeys', 'CTRL+S', '0', '0', '1', '1']), /Release held/));
  checked(() => assert.equal(test.state.events.length, 0));
}

for (let heldButton = 0; heldButton < 5; heldButton++) {
  const test = backend({ heldButton });
  checked(() => assert.throws(() => test.run(['typeText', 'not typed', '1', '1', '0', '0']), /Release held mouse/));
  checked(() => assert.equal(test.state.events.length, 0));
}

for (const failMouseEvent of [5, 1, 2]) {
  const test = backend({ failMouseEvent });
  checked(() => assert.throws(() => test.run(['typeText', 'not typed', '1', '1', '0', '0']), /allocate.*mouse event/));
  checked(() => assert.equal(test.state.events.length, 0));
  checked(() => assert.equal(test.state.eventReleases, 2));
}

{
  const test = backend({ permissionDenied: true });
  checked(() => assert.throws(() => test.run(['focusWindow', '1234', '0', '0', '0', '0']), /Accessibility permission/));
  checked(() => assert.equal(test.state.events.length, 0));
  checked(() => assert.equal(test.state.raised, false));
}

{
  const test = backend({ focusChangesAfterClick: true });
  checked(() => assert.throws(() => test.run(['typeText', 'never typed', '1', '1', '0', '0']), /focused application changed/));
  checked(() => assert.equal(test.state.events.filter(event => event.type === 'keyDown').length, 0));
}

{
  const test = backend({ windowChangesAfterClick: true });
  checked(() => assert.throws(() => test.run(['typeText', 'never typed', '1', '1', '0', '0']), /focused window changed/));
  checked(() => assert.equal(test.state.events.filter(event => event.type === 'keyDown').length, 0));
}

{
  const test = backend({ noWindow: true });
  checked(() => assert.throws(() => test.run(['typeText', 'never typed', '1', '1', '0', '0']), /no accessible focused window/));
  checked(() => assert.equal(test.state.events.length, 0));
}

{
  const test = backend({ failKeyboardUp: true });
  checked(() => assert.throws(() => test.run(['sendKeys', 'CTRL+S', '0', '0', '1', '1']), /allocate.*keyboard event/));
  checked(() => assert.equal(test.state.events.length, 0));
  checked(() => assert.equal(test.state.eventReleases, 1));
}

{
  const test = backend();
  test.run(['focusWindow', '1234', '0', '0', '0', '0']);
  checked(() => assert.equal(test.state.minimized, false));
  checked(() => assert.equal(test.state.raised, true));
  checked(() => assert.equal(test.state.events.length, 0));
}

{
  const test = backend({ ambiguous: true });
  checked(() => assert.throws(() => test.run(['focusWindow', 'Same Name', '0', '0', '0', '0']), /missing or ambiguous/));
  checked(() => assert.equal(test.state.raised, false));
}

console.log(`desktop POSIX fixed-JXA mock fixtures: ${assertions} assertions passed; no real app actions`);
