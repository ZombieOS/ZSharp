// Regression coverage for multiple HTML entry scripts using the real loader/WASM.
// Run after building browser/dist: node browser/multi-entry-tests.mjs
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const loaderSource = fs.readFileSync(new URL('./dist/bzvm.js', import.meta.url), 'utf8');
const wasmBytes = fs.readFileSync(new URL('./dist/bzvm.wasm', import.meta.url));
const origin = 'http://localhost';
const settingsSource = `zsharp = type.settings
Project: "Multi Entry Test":
PID: "browser_test":
Version: [1.0.0.0]:
Authors: ["Tester"]:
Description: "Multi-entry loader regression test":
ZSharp: [1.3.0.0]:
Dependencies ():
`;

const program = (room, body, declarations = '', functions = '') => `zsharp = type.script
noticed room ${room}[] (
 ${declarations}
 noticed brain Start[] (
  ${body}
 )
 ${functions}
)
`;
const external = (path, extra = {}) => ({src: new URL(path, origin).href, dataset: {}, ...extra});
const inline = (source, name) => ({src: '', textContent: source, dataset: name ? {name} : {}});
const resourcePath = url => new URL(String(url), origin).pathname;

async function page({scripts, files = {}, settings = false, viewSources = false}) {
  const fetched = [], prints = [], writes = [], reports = [], sourceViews = [];
  const timers = new Map(), listeners = new Map(), browserEvents = new Map();
  let nextTimer = 0, complete, instance;
  const finished = new Promise(resolve => complete = resolve);
  const node = id => {
    let content = '';
    const element = {id, style: {},
      addEventListener(event, callback) {
        const key = id + ':' + event;
        if (!listeners.has(key)) listeners.set(key, new Set());
        listeners.get(key).add(callback);
      },
      removeEventListener(event, callback) {
        const key = id + ':' + event;
        listeners.get(key)?.delete(callback);
        if (!listeners.get(key)?.size) listeners.delete(key);
      }
    };
    Object.defineProperty(element, 'textContent', {
      get: () => content,
      set(value) {content = value; writes.push([id, value]);}
    });
    return element;
  };
  const button = node('button'), other = node('other'), output = node('output');
  const document = {
    currentScript: {src: origin + '/bzvm/bzvm.js', dataset: viewSources ? {} : {sources: 'false'}},
    readyState: 'complete', baseURI: origin + '/index.html',
    querySelectorAll: selector => selector === '[id]' ? [button, other, output] : scripts
  };
  const window = {
    addEventListener(event, callback) {
      if (!browserEvents.has(event)) browserEvents.set(event, new Set());
      browserEvents.get(event).add(callback);
    },
    removeEventListener(event, callback) {
      browserEvents.get(event)?.delete(callback);
      if (!browserEvents.get(event)?.size) browserEvents.delete(event);
    },
    dispatchEvent(event) {
      if (event.type === 'bzvm:ready' || event.type === 'bzvm:error') complete(event);
    }
  };
  const context = {
    document, window, location: {href: document.baseURI}, URL, TextEncoder, TextDecoder,
    WebAssembly: {instantiate: async (...arguments_) => {
      const result = await WebAssembly.instantiate(...arguments_);
      instance = result.instance; return result;
    }}, Uint8Array, DataView, AbortController,
    Function: function (...args) {sourceViews.push(args.at(-1)); return () => {};},
    console: {log: (...values) => prints.push(values.join('')), error: (...values) => reports.push(values)},
    CustomEvent: class {constructor(type, options) {this.type = type; this.detail = options?.detail;}},
    setTimeout(callback, delay) {const id = ++nextTimer; timers.set(id, {callback, delay}); return id;},
    clearTimeout: id => timers.delete(id),
    fetch: async value => {
      const url = String(value), path = resourcePath(url);
      fetched.push(url);
      if (path.endsWith('.wasm')) return {ok: true, status: 200,
        arrayBuffer: async () => wasmBytes.buffer.slice(wasmBytes.byteOffset, wasmBytes.byteOffset + wasmBytes.byteLength)};
      let source;
      if (path.endsWith('.zsettings')) source = settings && path === '/project.zsettings' ? settingsSource : undefined;
      else source = files[path];
      if (source === undefined) return {ok: false, status: 404};
      if (typeof source === 'function') source = await source();
      return {ok: true, status: 200, text: async () => source};
    }
  };
  vm.runInNewContext(loaderSource, context, {filename: 'bzvm.js'});
  let timeout;
  const terminal = await Promise.race([finished,
    new Promise((_, reject) => timeout = setTimeout(() => reject(new Error('Loader never completed: ' + reports.map(String).join('\n'))), 10000))
  ]).finally(() => clearTimeout(timeout));
  const emit = (id, event = 'click') => {
    for (const callback of [...listeners.get(id + ':' + event) || []]) callback({preventDefault() {}});
  };
  const fireWindow = event => {
    for (const callback of [...browserEvents.get(event) || []]) callback({});
  };
  const resume = () => {
    assert.ok(timers.size, 'Expected a suspended Z# task');
    const [id, timer] = timers.entries().next().value;
    timers.delete(id); timer.callback();
  };
  const close = () => fireWindow('pagehide');
  return {terminal, fetched, prints, writes, reports, sourceViews, timers, listeners, browserEvents, instance,
    output, emit, fireWindow, resume, close};
}

function succeeded(result) {
  assert.equal(result.terminal.type, 'bzvm:ready', result.terminal.detail);
  assert.deepEqual(result.reports, []);
}
function failed(result, pattern) {
  assert.equal(result.terminal.type, 'bzvm:error', 'Invalid project unexpectedly started');
  assert.match(result.terminal.detail, pattern);
  assert.deepEqual(result.prints, [], 'A Start[] ran before all entry scripts were validated');
  assert.equal(result.timers.size, 0, 'Failed boot must retain no suspended work');
  assert.equal(result.listeners.size, 0, 'Failed boot must retain no DOM bindings');
  for (const name of ['keydown', 'keyup', 'blur', 'pointermove']) {
    assert.ok(!result.browserEvents.has(name), 'Failed boot retained window listener: ' + name);
  }
}
const canonicalFetchCount = (result, path) => result.fetched.filter(url => resourcePath(url) === path).length;
const failures = [];
const test = async (name, callback) => {
  try {await callback(); console.log('PASS: ' + name);}
  catch (error) {failures.push({name, error}); console.error('FAIL: ' + name + '\n' + error.stack);}
};

await test('two external HTML entries auto-start in tag order without project settings', async () => {
  const result = await page({scripts: [external('/First.zsharp'), external('/Second.zsharp')], files: {
    '/First.zsharp': program('First', 'Print("first"): '),
    '/Second.zsharp': program('Second', 'Print("second"): ')
  }});
  succeeded(result);
  assert.deepEqual(result.prints, ['first', 'second']);
  assert.equal(result.terminal.detail.project, undefined);
  assert.equal(canonicalFetchCount(result, '/First.zsharp'), 1);
  assert.equal(canonicalFetchCount(result, '/Second.zsharp'), 1);
  result.close();
});

await test('tagged roots preserve order while imports-only Start functions stay inert', async () => {
  const result = await page({settings: true, scripts: [external('/First.zsharp'), external('/Second.zsharp')], files: {
    '/First.zsharp': program('First', 'Function.call(Helper:Helper:Show): Print("first"):', 'import browser_test.Helper():'),
    '/Second.zsharp': program('Second', 'Print("second"):'),
    '/Helper.zsharp': program('Helper', 'Print("must not auto-start"):', '', 'noticed brain Show[] ( Print("helper call"): )')
  }});
  succeeded(result);
  assert.deepEqual(result.prints, ['helper call', 'first', 'second']);
  assert.equal(canonicalFetchCount(result, '/Helper.zsharp'), 1);
  result.close();
});

await test('an imported module explicitly tagged as an entry starts once in HTML order', async () => {
  const result = await page({settings: true,
    scripts: [external('/First.zsharp'), external('/Last.zsharp'), external('/Helper.zsharp?v=1310#entry')], files: {
      '/First.zsharp': program('First', 'Print("first"):', 'import browser_test.Helper():'),
      '/Last.zsharp': program('Last', 'Print("last"):'),
      '/Helper.zsharp': program('Helper', 'Print("helper entry"):')
    }});
  succeeded(result);
  assert.deepEqual(result.prints, ['first', 'last', 'helper entry']);
  assert.equal(canonicalFetchCount(result, '/Helper.zsharp'), 1, 'Import and tagged cache-busted URL must share one module');
  assert.ok(result.fetched.includes(origin + '/Helper.zsharp?v=1310#entry'), 'The first tagged fetch URL keeps its cache query');
  result.close();
});

await test('repeated external URLs differing only by query/hash load and start once', async () => {
  const result = await page({scripts: [external('/Repeat.zsharp?v=1'), external('/Repeat.zsharp?v=2#again')], files: {
    '/Repeat.zsharp': program('Repeat', 'Print("once"):')
  }});
  succeeded(result);
  assert.deepEqual(result.prints, ['once']);
  assert.equal(canonicalFetchCount(result, '/Repeat.zsharp'), 1);
  result.close();
});

await test('repeated HTML tags do not consume the unique-module file bound', async () => {
  const result = await page({scripts: Array.from({length: 70}, (_, index) => external('/Repeat.zsharp?v=' + index)), files: {
    '/Repeat.zsharp': program('Repeat', 'Print("once"):')
  }});
  succeeded(result);
  assert.deepEqual(result.prints, ['once']);
  assert.equal(canonicalFetchCount(result, '/Repeat.zsharp'), 1);
  result.close();
});

await test('different source paths cannot silently share a filename', async () => {
  const result = await page({scripts: [external('/first/Main.zsharp'), external('/second/Main.zsharp')], files: {
    '/first/Main.zsharp': program('Main', 'Print("must not run"): '),
    '/second/Main.zsharp': program('Main', 'Print("must not run either"): ')
  }});
  failed(result, /duplicate.*(?:filename|script)|(?:filename|script).*duplicate/i);
});

await test('single inline entry preserves the Main filename', async () => {
  const result = await page({viewSources: true, scripts: [inline(program('Main', 'Print("single inline"):'))]});
  succeeded(result);
  assert.deepEqual(result.prints, ['single inline']);
  assert.ok(result.sourceViews.some(source => source.includes('/Main.zsharp.bzvm-view.js')));
  result.close();
});

await test('unnamed inline entries receive distinct deterministic filenames and start in order', async () => {
  const result = await page({viewSources: true,
    scripts: [inline(program('Main', 'Print("inline one"):')), inline(program('Main', 'Print("inline two"):'))]});
  succeeded(result);
  assert.deepEqual(result.prints, ['inline one', 'inline two']);
  assert.ok(result.sourceViews.some(source => source.includes('/Inline1.zsharp.bzvm-view.js')));
  assert.ok(result.sourceViews.some(source => source.includes('/Inline2.zsharp.bzvm-view.js')));
  result.close();
});

await test('mixed external/inline default names use one-based HTML tag position', async () => {
  const result = await page({viewSources: true,
    scripts: [external('/External.zsharp'), inline(program('Main', 'Print("inline"):'))],
    files: {'/External.zsharp': program('External', 'Print("external"):')}});
  succeeded(result);
  assert.deepEqual(result.prints, ['external', 'inline']);
  assert.ok(result.sourceViews.some(source => source.includes('/Inline2.zsharp.bzvm-view.js')));
  result.close();
});

await test('explicit inline data-name values preserve callback/module identity', async () => {
  const result = await page({scripts: [
    inline(program('Main', 'Print("alpha"): Browser.button.clicked: ( Print("alpha click"): ): '), 'Alpha'),
    inline(program('Main', 'Print("beta"): Browser.other.clicked: ( Print("beta click"): ): '), 'Beta')
  ]});
  succeeded(result);
  result.emit('other'); result.emit('button');
  assert.deepEqual(result.prints, ['alpha', 'beta', 'beta click', 'alpha click']);
  result.close();
});

await test('a named inline module resolves an explicitly authorized root-level import without a source download', async () => {
  const result = await page({settings: true, scripts: [external('/Main.zsharp'),
    inline(program('Utilities', 'Print("utilities entry"):', 'import browser_test.Main():',
      'noticed brain Show[] ( Print("inline function"): )'), 'Utilities')], files: {
      '/Main.zsharp': program('Main', 'Function.call(Utilities:Utilities:Show): Print("main"): ', 'import browser_test.Utilities():')
    }});
  succeeded(result);
  assert.deepEqual(result.prints, ['inline function', 'main', 'utilities entry']);
  assert.equal(canonicalFetchCount(result, '/Utilities.zsharp'), 0, 'Named inline source is already loaded');
  result.close();
});

await test('inline names cannot hide different-path imports with the same basename', async () => {
  const result = await page({settings: true, scripts: [external('/Main.zsharp'),
    inline(program('Utilities', 'Print("must not run"): '), 'Utilities')], files: {
      '/Main.zsharp': program('Main', 'Print("must not run either"): ', 'import browser_test.Scripts.Utilities():'),
      '/Scripts/Utilities.zsharp': program('Utilities', '')
    }});
  failed(result, /duplicate.*(?:filename|script)|(?:filename|script).*duplicate/i);
  assert.equal(canonicalFetchCount(result, '/Scripts/Utilities.zsharp'), 1, 'A nested path must not resolve by basename to inline source');
});

await test('duplicate inline filenames fail clearly before any startup', async () => {
  const result = await page({scripts: [inline(program('Main', 'Print("no"): '), 'Same'), inline(program('Other', ''), 'Same')]});
  failed(result, /duplicate.*(?:filename|script)|(?:filename|script).*duplicate/i);
});

await test('invalid inline module identifiers fail clearly before any startup', async () => {
  const result = await page({scripts: [inline(program('Main', 'Print("no"): '), 'Good'), inline(program('Other', ''), 'not-valid')]});
  failed(result, /(?:invalid|valid|identifier|filename|data-name)/i);
});

await test('waiting in one entry does not block another or confuse their variables/continuations', async () => {
  const result = await page({scripts: [external('/First.zsharp'), external('/Second.zsharp')], files: {
    '/First.zsharp': program('SharedRoom', 'Print("first before"): wait(2ms): Print(Message):', 'noticed text Message = "first after":'),
    '/Second.zsharp': program('SharedRoom', 'Print(Message):', 'noticed text Message = "second start":')
  }});
  succeeded(result);
  assert.deepEqual(result.prints, ['first before', 'second start']);
  assert.equal(result.timers.size, 1);
  assert.equal([...result.timers.values()][0].delay, 2);
  result.resume();
  assert.deepEqual(result.prints, ['first before', 'second start', 'first after']);
  assert.deepEqual(result.reports, []);
  result.close();
});

await test('loading another entry does not implicitly authorize cross-file calls', async () => {
  const result = await page({settings: true, scripts: [external('/First.zsharp'), external('/Second.zsharp')], files: {
    '/First.zsharp': program('First', 'Function.call(Second:Second:Show):'),
    '/Second.zsharp': program('Second', '', '', 'noticed brain Show[] ( Print("unauthorized"): )')
  }});
  failed(result, /requires an import/i);
});

await test('malformed later entry prevents all Start functions from running', async () => {
  const result = await page({scripts: [external('/First.zsharp'), external('/Broken.zsharp')], files: {
    '/First.zsharp': program('First', 'Print("must not run"): '),
    '/Broken.zsharp': 'zsharp = type.script\nnoticed room Broken[] ( noticed brain Start[] ('
  }});
  failed(result, /Broken|expected|unterminated/i);
});

await test('empty/whitespace entry is an error rather than a silently skipped program', async () => {
  const result = await page({scripts: [inline(program('First', 'Print("must not run"): ')), inline(' \r\n\t ')]});
  failed(result, /empty|blank|source|expected|zsharp/i);
});

await test('missing later source prevents startup of already downloaded entries', async () => {
  const result = await page({scripts: [external('/First.zsharp'), external('/Missing.zsharp')], files: {
    '/First.zsharp': program('First', 'Print("must not run"): ')
  }});
  failed(result, /404|download|Missing/i);
});

await test('room JSON preloading in a later entry completes before any Start executes', async () => {
  const result = await page({scripts: [external('/First.zsharp'), external('/Second.zsharp')], files: {
    '/First.zsharp': program('First', 'Print("must not run"): '),
    '/Second.zsharp': program('Second', 'Print("must not run either"): ', `import ZSharp.JSON():
     noticed JSON Person[] ( name: text: )
     noticed JSON Data = JSON.load(Person, "missing.json"):`)
  }});
  failed(result, /JSON|404/i);
  assert.equal(canonicalFetchCount(result, '/missing.json'), 1);
});

await test('a later startup failure disposes already-started entry tasks and bindings', async () => {
  const result = await page({scripts: [external('/First.zsharp'), external('/Second.zsharp')], files: {
    '/First.zsharp': program('First', 'Browser.button.clicked: ( Print("must not click"): ): wait(5s): Print("must not resume"): '),
    '/Second.zsharp': program('Second', 'Browser.missing.content.set: "fails":')
  }});
  failed(result, /missing|HTML ID|found/i);
  assert.equal(result.timers.size, 0, 'Failed startup must cancel a previously suspended entry');
  assert.equal(result.listeners.size, 0, 'Failed startup must detach previously installed event callbacks');
  for (const name of ['keydown', 'keyup', 'blur', 'pointermove']) assert.ok(!result.browserEvents.has(name));
  result.emit('button');
  assert.deepEqual(result.prints, []);
});

await test('pagehide cancels timers and event bindings from every entry', async () => {
  const result = await page({scripts: [external('/First.zsharp'), external('/Second.zsharp')], files: {
    '/First.zsharp': program('First', 'Browser.button.clicked: ( Print("first click"): wait(5s): Print("first resumed"): ): wait(10s): Print("first startup resumed"): '),
    '/Second.zsharp': program('Second', 'Browser.other.clicked: ( Print("second click"): wait(6s): Print("second resumed"): ): wait(11s): Print("second startup resumed"): ')
  }});
  succeeded(result);
  result.emit('button'); result.emit('other');
  assert.deepEqual(result.prints, ['first click', 'second click']);
  assert.equal(result.timers.size, 4);
  assert.equal(result.listeners.size, 2);
  const staleTimers = [...result.timers.values()];
  const staleCallbacks = [...result.listeners.values()].flatMap(set => [...set]);
  result.close();
  assert.equal(result.timers.size, 0);
  assert.equal(result.listeners.size, 0);
  for (const name of ['keydown', 'keyup', 'blur', 'pointermove']) assert.ok(!result.browserEvents.has(name));
  for (const timer of staleTimers) timer.callback();
  // Dispatching DOM events after all listeners were removed is inert.
  result.emit('button'); result.emit('other');
  assert.deepEqual(result.prints, ['first click', 'second click']);
  assert.equal(result.timers.size, 0);
  assert.deepEqual(result.reports, []);
  assert.equal(staleCallbacks.length, 2);
});

await test('64 total entry modules are accepted', async () => {
  const scripts = [], files = {};
  for (let index = 0; index < 64; index++) {
    const path = '/Entry' + index + '.zsharp'; scripts.push(external(path));
    files[path] = program('Room', `Print("${index}"):`);
  }
  const result = await page({scripts, files});
  succeeded(result);
  assert.deepEqual(result.prints, Array.from({length: 64}, (_, index) => String(index)));
  result.close();
});

await test('entry modules share the existing 64-file project bound', async () => {
  const scripts = [], files = {};
  for (let index = 0; index < 65; index++) {
    const path = '/Entry' + index + '.zsharp'; scripts.push(external(path));
    files[path] = program('Room', 'Print("must not run"):');
  }
  const result = await page({scripts, files});
  failed(result, /64.*(?:file|script)|(?:file|script).*64/i);
});

await test('entry modules and imports count toward one combined file bound', async () => {
  const scripts = [], files = {};
  for (let index = 0; index < 64; index++) {
    const path = '/Entry' + index + '.zsharp'; scripts.push(external(path));
    files[path] = program('Room', 'Print("must not run"):', index === 63 ? 'import browser_test.Extra():' : '');
  }
  files['/Extra.zsharp'] = program('Extra', 'Print("must not run either"):');
  const result = await page({scripts, files, settings: true});
  failed(result, /64.*(?:file|script)|(?:file|script).*64/i);
});

await test('the 8 MiB source bound applies to combined entry sources', async () => {
  const scripts = [], files = {};
  for (let index = 0; index < 9; index++) {
    const path = '/Large' + index + '.zsharp'; scripts.push(external(path));
    files[path] = program('Room', 'Print("must not run"):') + '// ' + ' '.repeat(1048576);
  }
  const result = await page({scripts, files});
  failed(result, /8 MiB|8388608|project.*limit/i);
});

await test('desktop Application imports are explicitly rejected before browser startup', async () => {
  const result = await page({scripts: [inline(program('Main', 'Print("must not run"):', 'import ZSharp.Application():'))]});
  failed(result, /ZSharp.Application.*desktop.*not supported in bZVM/i);
});

await test('desktop Application.Quit instructions are explicitly rejected by browser WASM', async () => {
  const result = await page({scripts: [inline(program('Main', 'Application.Quit("tray"):', 'import ZSharp.Application():'))]});
  failed(result, /Application.Quit.*desktop.*not supported in bZVM/i);
});

await test('desktop speech, microphone, speaker and desktop imports are rejected before browser startup', async () => {
  for (const family of ['Speech', 'Microphone', 'Speaker', 'Desktop']) {
    const result = await page({scripts: [inline(program('Main', 'Print("must not run"):', `import ZSharp.${family}():`))]});
    failed(result, new RegExp(`ZSharp\\.${family}.*desktop.*not supported in bZVM`, 'i'));
    assert.deepEqual(result.prints, []);
  }
});

await test('desktop system instructions cannot execute in browser WASM', async () => {
  for (const [family, body] of [
    ['Speech', 'Speech.speak("must not speak"):'],
    ['Microphone', 'Print(Microphone.level):'],
    ['Speaker', 'Print(Speaker.getOutputDevice):'],
    ['Desktop', 'Desktop.openApp("must-not-open"):'],
  ]) {
    const result = await page({scripts: [inline(program('Main', body, `import ZSharp.${family}():`))]});
    failed(result, /desktop.*(?:not supported|not available) in bZVM/i);
    assert.deepEqual(result.prints, []);
  }
});

await test('entry-selection ABI validates indices, deduplicates ordered marks, rejects late marks, and resets compatibly', async () => {
  const result = await page({scripts: [inline(program('Legacy', ''))]});
  succeeded(result);
  const exports = result.instance.exports, encoder = new TextEncoder(), decoder = new TextDecoder();
  const read = pointer => {
    const bytes = new Uint8Array(exports.memory.buffer); let end = pointer;
    while (bytes[end]) end++;
    return decoder.decode(bytes.subarray(pointer, end));
  };
  const put = value => {
    const bytes = encoder.encode(value), pointer = exports.malloc(bytes.length + 1);
    assert.ok(pointer); const memory = new Uint8Array(exports.memory.buffer);
    memory.set(bytes, pointer); memory[pointer + bytes.length] = 0; return pointer;
  };
  const load = (source, name) => {
    const sourcePointer = put(source), namePointer = put(name);
    try {assert.ok(exports.bzvm_load(sourcePointer, namePointer), read(exports.bzvm_error()));}
    finally {exports.free(sourcePointer); exports.free(namePointer);}
  };
  exports.bzvm_reset();
  assert.equal(exports.bzvm_entry(0), 0, 'Cannot mark an unloaded module');
  assert.match(read(exports.bzvm_error()), /invalid|loaded|index/i);
  load(program('A', 'Print("A"): '), 'A');
  load(program('B', 'Print("B"): '), 'B');
  load(program('Helper', 'Print("not an entry"): '), 'Helper');
  assert.equal(exports.bzvm_entry(3), 0, 'Index is zero-based and must identify a loaded module');
  assert.equal(exports.bzvm_entry(1), 1);
  assert.equal(exports.bzvm_entry(0), 1);
  assert.equal(exports.bzvm_entry(1), 1, 'Repeated entry marks are idempotent');
  assert.equal(exports.bzvm_run(), 1, read(exports.bzvm_error()));
  assert.deepEqual(result.prints, ['B', 'A'], 'Mark order controls startup; unmarked helper remains inert');
  assert.equal(exports.bzvm_entry(2), 0, 'Selection is immutable after startup');
  assert.match(read(exports.bzvm_error()), /before|run|start/i);
  exports.bzvm_reset(); result.prints.length = 0;
  load(program('A', 'Print("legacy first only"): '), 'A');
  load(program('B', 'Print("must not auto-start"): '), 'B');
  assert.equal(exports.bzvm_run(), 1, read(exports.bzvm_error()));
  assert.deepEqual(result.prints, ['legacy first only'], 'Reset removes entry selections and preserves the old unmarked ABI');
  result.close();
});

assert.equal(failures.length, 0, failures.map(failure => failure.name).join('\n'));
console.log('PASS: all multiple-entry loader regressions');
