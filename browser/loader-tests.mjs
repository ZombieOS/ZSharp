// End-to-end loader/DOM bridge test using actual WASM and deterministic browser timers.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const listeners = new Map(), timers = new Map(), events = [], errors = [];
const standalone = process.argv.includes('--standalone');
const multi = process.argv.includes('--multi');
const nested = process.argv.includes('--nested');
const time = process.argv.includes('--time');
const fold = process.argv.includes('--fold');
const features = process.argv.includes('--features');
const noSources = process.argv.includes('--no-sources');
const csp = process.argv.includes('--csp');
const json = process.argv.includes('--json');
const fetched = [];
const sourceViews = [];
let projectMetadata;
let nextTimer = 0, ready;
const loaded = new Promise(resolve => ready = resolve);
const node = (id, values = {}) => ({id, style: {}, ...values,
  addEventListener: (event, fn) => listeners.set(id + ':' + event, fn),
  removeEventListener: event => listeners.delete(id + ':' + event)});
const input = node('input', {value: 'Tester'}), output = node('output', {textContent: ''}), button = node('button');
const nodes = [input, output, button];
if(features) {
 for(let i=0;i<3;i++)nodes.push(node('duplicate',{textContent:'original',getAttribute:name=>name==='class'?'sample':null,setAttribute(){}}));
 input.getAttribute=name=>name==='class'?'entry':null;
 input.setAttribute=(name,value)=>input[name]=value;
}
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
if(time) source=`zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (
  Browser.output.content.set: "" + Time.convert("2026-01-02", "15:04:05", LOCAL):
  Browser.button.clicked: (
   Browser.output.content.set: "" + Time.convert("2026-03-08", "02:30:00", LOCAL):
  ):
 )
)`;
if(time && fold) source=source.replace('2026-03-08','2026-11-01').replace('02:30:00','01:30:00');
if(features)source=`zsharp = type.script
noticed room Main[] (
 noticed brain Start[] (
  Browser.duplicate[2].content.set: "updated":
  text() Classes = Browser.duplicate[*].getattr[class]:
  Browser.output.content.set: "" + Classes.Length + Browser.input.getInput:
  Browser.input.setattr[class]: "changed":
  Browser.button.submitted: ( Browser.output.content.set: "submitted": ):
  loop (
   if[Browser.input.e.pressed] ( Browser.output.content.set: "pressed": ) else ()
   if[Browser.input.e.released] ( Browser.output.content.set: Browser.output.content + " released": ) else ()
   wait(100ms):
  )
 )
)
`;
if(json)source=`zsharp = type.script
noticed room Main[] (
 import ZSharp.JSON():
 noticed JSON PersonSchema[] (name: text: enabled: status:)
 noticed JSON Person=JSON.load(PersonSchema,"person.json"):
 noticed brain Start[] (
  Browser.output.content.set: Person.name:
  Browser.button.clicked: (
   text Path="other.json":
   JSON Other=JSON.load(PersonSchema,Path):
   Browser.output.content.set: Other.name + " " + Other.enabled:
  ):
 )
)`;
const document = {
  currentScript: {src: 'http://localhost/bzvm/bzvm.js', dataset: noSources ? {sources:'false'} : {}}, readyState: 'complete',
  querySelectorAll: selector => selector === '[id]' ? nodes : [{src: 'http://localhost/Main.zsharp', dataset: {}}]
};
const window = {
  addEventListener: (event, fn) => events.push([event, fn]),
  dispatchEvent: event => {if(event.type === 'bzvm:ready'){projectMetadata=event.detail?.project;ready();}}
};
const context = {document, window, URL, TextEncoder, TextDecoder, WebAssembly, Uint8Array, DataView, AbortController,
  Function: function(...args) {if(csp)throw new EvalError('CSP blocks dynamic code');sourceViews.push(args.at(-1));return Function(...args);},
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
ZSharp: [1.3.0.0]:
Dependencies ():
` : String(url).endsWith('.json') ? JSON.stringify({name:String(url).endsWith('other.json')?'Grace':'Ada',enabled:true}) : String(url).endsWith('/Scripts/Messages.zsharp') ? `zsharp = type.script
noticed room Messages[] (
 import browser_test.Main():
 noticed brain Show[] ( Browser.output.content.set: "imported": )
)` : source};}
};
vm.runInNewContext(fs.readFileSync(new URL('./dist/bzvm.js',import.meta.url),'utf8'),context);
let startupTimeout;
await Promise.race([loaded, new Promise((_,reject)=>startupTimeout=setTimeout(()=>reject(Error('loader did not start: '+JSON.stringify(errors))),3000))]);
clearTimeout(startupTimeout);
if(noSources || csp)assert.equal(sourceViews.length,0);
else {
assert.ok(sourceViews.length >= 1);
const maps=sourceViews.map(view=>JSON.parse(decodeURIComponent(view.split('sourceMappingURL=data:application/json;charset=utf-8,')[1])));
assert.equal(maps[0].sourcesContent[0],source);
assert.equal(maps[0].sources[0],'http://localhost/Main.zsharp');
assert.ok(sourceViews[0].includes('sourceURL=http://localhost/Main.zsharp.bzvm-view.js'));
if(multi)assert.ok(maps.some(map=>map.sources[0].endsWith('/Scripts/Messages.zsharp')));
}
if(standalone)assert.equal(projectMetadata,undefined);
else {assert.equal(projectMetadata.id,'browser_test');assert.equal(projectMetadata.root,'http://localhost/');}
if(multi){assert.equal(output.textContent,'imported');assert.equal(fetched.filter(url=>url.endsWith('/Scripts/Messages.zsharp')).length,1);}
if(nested)assert.ok(fetched.includes('http://localhost/pages/test/project.zsettings'));
if(json) {
 assert.equal(output.textContent,'Ada');
 listeners.get('button:click')();assert.ok([...timers.values()].some(timer=>timer.delay===10));
 await new Promise(resolve=>setImmediate(resolve));
 const [id,timer]=[...timers].find(([,timer])=>timer.delay===10);timers.delete(id);timer.fn();
 assert.equal(output.textContent,'Grace alive');assert.deepEqual(errors,[]);
 assert.equal(fetched.filter(url=>url.endsWith('/person.json')).length,1);
 assert.equal(fetched.filter(url=>url.endsWith('/other.json')).length,1);
 events.find(([event])=>event==='pagehide')[1]();
 console.log('PASS: loader preloads room JSON, fetches variable paths asynchronously and reuses bounded same-origin cache');
 process.exit(0);
}
if(features) {
 assert.equal(output.textContent,'3Tester');
 assert.equal(nodes[3].textContent,'updated');assert.equal(nodes[4].textContent,'updated');assert.equal(nodes[5].textContent,'original');
 assert.equal(input.class,'changed');
 let prevented=false;listeners.get('button:submit')({preventDefault(){prevented=true;}});
 assert.equal(prevented,true);assert.equal(output.textContent,'submitted');
 events.find(([kind])=>kind==='keydown')[1]({key:'e'});
 events.find(([kind])=>kind==='keyup')[1]({key:'e'});
 assert.deepEqual(errors,[]);
 const resume=()=>{const [id,timer]=[...timers][0];timers.delete(id);timer.fn();};
 resume();assert.equal(output.textContent,'pressed released');
 output.textContent='consumed';resume();assert.equal(output.textContent,'consumed');
 events.find(([event])=>event==='pagehide')[1]();
 assert.deepEqual(errors,[]);
 console.log('PASS: mixed-tag duplicate selectors, attribute arrays/writes, getInput, submit prevention and latched short key edges');
 process.exit(0);
}
if(time) {
 assert.equal(output.textContent,String(new Date(2026,0,2,15,4,5).getTime()/1000));
 if((process.env.TZ || Intl.DateTimeFormat().resolvedOptions().timeZone)==='America/New_York') {
  listeners.get('button:click')();
  assert.ok(errors.some(e=>String(e[1]).includes('nonexistent or ambiguous')),'DST gap must be rejected');
 } else {
  listeners.get('button:click')();
  assert.equal(output.textContent,String((fold?new Date(2026,10,1,1,30,0):new Date(2026,2,8,2,30,0)).getTime()/1000));
 }
 console.log('PASS: real loader Time timezone bridge ('+(process.env.TZ||'system')+')');
 process.exit(0);
}
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
