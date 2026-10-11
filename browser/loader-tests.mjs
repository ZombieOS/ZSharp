// End-to-end loader/DOM bridge test using actual WASM and deterministic browser timers.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const listeners = new Map(), timers = new Map(), events = [], errors = [];
const standalone = process.argv.includes('--standalone');
const multi = process.argv.includes('--multi');
const nested = process.argv.includes('--nested');
const fetched = [];
let projectMetadata;
let nextTimer = 0, ready;
const loaded = new Promise(resolve => ready = resolve);
const node = (id, values = {}) => ({id, style: {}, ...values,
  addEventListener: (event, fn) => listeners.set(id + ':' + event, fn),
  removeEventListener: event => listeners.delete(id + ':' + event)});
const input = node('input', {value: 'Tester'}), output = node('output', {textContent: ''}), button = node('button');
const nodes = [input, output, button];
let source = `zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (
  Browser.button.clicked: (
   if[Browser.input.value != ""] (
    Browser.output.content.set: "Hello " + Browser.input.value + " #" + 2:
    wait(5s):
    Browser.output.content.set: "finished":
   ) else ( Browser.output.content.set: "empty": )
  ):
 )
)`;
if(multi)source=source.replace('noticed room Main[] (','noticed room Main[] ( import browser_test.Scripts.Messages():')
  .replace('noticed brain Start[] (','noticed brain Start[] ( Function.call(Messages:Messages:Show):');
const document = {
  currentScript: {src: 'http://localhost/bzvm/bzvm.js', dataset: {}}, readyState: 'complete',
  querySelectorAll: selector => selector === '[id]' ? nodes : [{src: 'http://localhost/Main.zsharp', dataset: {}}]
};
const window = {
  addEventListener: (event, fn) => events.push([event, fn]),
  dispatchEvent: event => {if(event.type === 'bzvm:ready'){projectMetadata=event.detail?.project;ready();}}
};
const context = {document, window, URL, TextEncoder, TextDecoder, WebAssembly, Uint8Array, DataView,
  location: {href: nested ? 'http://localhost/pages/test/index.html' : 'http://localhost/'},
  console: {log() {}, error: (...args) => errors.push(args)},
  CustomEvent: class {constructor(type, options){this.type=type;this.detail=options?.detail;}},
  setTimeout: (fn, delay) => {const id=++nextTimer;timers.set(id,{fn,delay});return id;},
  clearTimeout: id => timers.delete(id),
  fetch: async url => {fetched.push(String(url));return String(url).endsWith('.zsettings') &&
    (standalone || String(url)!=='http://localhost/project.zsettings') ? {ok:false,status:404} : String(url).endsWith('.wasm')
    ? {ok:true,arrayBuffer:async()=>{const b=fs.readFileSync(new URL('./dist/bzvm.wasm',import.meta.url));return b.buffer.slice(b.byteOffset,b.byteOffset+b.byteLength);}}
    : {ok:true,text:async()=>String(url).endsWith('.zsettings') ? `zsharp = type.settings
Project: "Browser Test":
PID: "browser_test":
Version: [1.0.0.0]:
Authors: ["Tester"]:
Description: "Test":
ZSharp: [1.2.2.0]:
Dependencies ():
` : String(url).endsWith('/Scripts/Messages.zsharp') ? `zsharp = type.script
noticed room Messages[] (
 import browser_test.Main():
 noticed brain Show[] ( Browser.output.content.set: "imported": )
)` : source};}
};
vm.runInNewContext(fs.readFileSync(new URL('./dist/bzvm.js',import.meta.url),'utf8'),context);
let startupTimeout;
await Promise.race([loaded, new Promise((_,reject)=>startupTimeout=setTimeout(()=>reject(Error('loader did not start: '+JSON.stringify(errors))),3000))]);
clearTimeout(startupTimeout);
if(standalone)assert.equal(projectMetadata,undefined);
else {assert.equal(projectMetadata.id,'browser_test');assert.equal(projectMetadata.root,'http://localhost/');}
if(multi){assert.equal(output.textContent,'imported');assert.equal(fetched.filter(url=>url.endsWith('/Scripts/Messages.zsharp')).length,1);}
if(nested)assert.ok(fetched.includes('http://localhost/pages/test/project.zsettings'));
listeners.get('button:click')();
assert.equal(output.textContent,'Hello Tester #2');
assert.equal(timers.size,1);assert.equal([...timers.values()][0].delay,5000);
// A second event runs immediately while the first event is suspended.
input.value='';listeners.get('button:click')();assert.equal(output.textContent,'empty');
const [id,timer]=[...timers][0];timers.delete(id);timer.fn();assert.equal(output.textContent,'finished');
input.value='Again';listeners.get('button:click')();assert.equal(timers.size,1);
events.find(([event])=>event==='pagehide')[1]();
assert.equal(timers.size,0);assert.equal(listeners.size,0);assert.deepEqual(errors,[]);
console.log('PASS: real JS loader and WASM, DOM fields, click dispatch, non-blocking timers and pagehide cleanup');
