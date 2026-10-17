import fs from 'node:fs';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import os from 'node:os';
import path from 'node:path';
const module = new WebAssembly.Module(fs.readFileSync(new URL('./dist/bzvm.wasm',import.meta.url)));
let instance, now = 1767225600000;
const output=[];
const read = p => {const m=new Uint8Array(instance.exports.memory.buffer);let end=p;while(m[end])end++;return new TextDecoder().decode(m.subarray(p,end));};
const wasi={};for(const imp of WebAssembly.Module.imports(module))if(imp.module==='wasi_snapshot_preview1')wasi[imp.name]=()=>0;
instance=new WebAssembly.Instance(module,{wasi_snapshot_preview1:wasi,bzvm:{
 now:()=>performance.now(),wall_time:()=>now,input_session:()=>{},network:()=>0,
 local_time:(ms,i)=>[2026,1,2,15,4,5,5][i],
 convert_local_time:(y,m,d)=>y===2026 && m===1 && d===2 ? 1767366245000 : NaN,
 print:p=>output.push(read(p)),read:()=>0,write:()=>1,raw:()=>1,bind:()=>1,schedule:()=>1
}});
instance.exports._initialize?.();
function call(name,...args){const ptrs=args.map(s=>{const bytes=Buffer.from(s+'\0');const p=instance.exports.malloc(bytes.length);new Uint8Array(instance.exports.memory.buffer).set(bytes,p);return p;});try{return instance.exports[name](...ptrs);}finally{ptrs.forEach(p=>instance.exports.free(p));}}
function run(body,expected){instance.exports.bzvm_reset();output.length=0;assert.equal(call('bzvm_load',`zsharp = type.script noticed room Main[] ( noticed brain Start[] ( ${body} ) )`),1,read(instance.exports.bzvm_error()));assert.equal(instance.exports.bzvm_run(),1,read(instance.exports.bzvm_error()));assert.deepEqual(output,expected);}
run(`Print(Time.convert("1/1/26", "00:00", UTC)):
 Print(Time.convert("2026-01-01", "00:00:00", "UTC+05:30")):
 Print(Time.convert("1970-01-01", "00:00", UTC)):
 Print(Time.convert("1969-12-31", "23:59:59", UTC)):
 Print(Time.since("1767131760")):
 Print(Time.until(1767319440)):
 Print(Time.since(1767319440000, "ms")):
 Print(Time.until("1767131760")):
 Print(Time.since(Time.convert("1/1/26", "00:00", UTC))):`,
 ['1767225600','1767205800','0','-1','1 day, 2 hours, 4 minutes ago','in 1 day, 2 hours, 4 minutes','in 1 day, 2 hours, 4 minutes','1 day, 2 hours, 4 minutes ago','0 seconds ago']);
now=1767366245427;
run(`Print(Time.getYear()): Print(Time.getMonth()): Print(Time.getDay()):
 Print(Time.getWeek()): Print(Time.getHour()): Print(Time.getHour().formatted):
 Print(Time.getMinute()): Print(Time.getSecond()): Print(Time.getMS()):
 Print(Time.getMS(1)): Print(Time.getMS(2)): Print(Time.getMS(3)):
 Print(Time.convert("2026-01-02", "15:04:05", LOCAL)):`,
 ['2026','1','2','Friday','15','3 PM','4','5','4','4','42','427','1767366245']);
for(const expr of ['Time.convert("2026-02-29","00:00",UTC)','Time.getMS(4)','Time.since("abc")','Time.since(alive)','Time.convert("2026-01-02","15:04:05", "invalid")']) {
 instance.exports.bzvm_reset();
 assert.equal(call('bzvm_load',`zsharp=type.script noticed room Main[] (noticed brain Start[] (Print(${expr}):))`),1);
 assert.equal(instance.exports.bzvm_run(),0,expr);assert.match(read(instance.exports.bzvm_error()),/Time/);
}
for(const expr of ['Time.getYear(1)','Time.since()','Time.getHour().nope','Time.convert("2026-01-01","00:00")']) {
 instance.exports.bzvm_reset();
 assert.equal(call('bzvm_load',`zsharp=type.script noticed room Main[] (noticed brain Start[] (Print(${expr}):))`),0,expr);
}
const fixture=new URL('./fixtures/Time/Main.zsharp',import.meta.url);
const native=spawnSync(fileURLToPath(new URL('../build/Release/zsharp.exe',import.meta.url)),['run',fileURLToPath(fixture)],{encoding:'utf8'});
assert.equal(native.status,0,native.stderr);
assert.equal(native.stdout.replace(/\r\n/g,'\n').trim(),'0\n1767225600\n1767205800\n-1\nyear OK\nmonth OK\nhour OK\nweekday OK\nformatted OK\nms OK\nrelative OK');
const nativePath=fileURLToPath(new URL('../build/Release/zsharp.exe',import.meta.url));
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'zsharp-time-'));
const settings=fs.readFileSync(new URL('./fixtures/Time/project.zsettings',import.meta.url),'utf8');
fs.writeFileSync(path.join(temp,'project.zsettings'),settings);
fs.copyFileSync(fixture,path.join(temp,'Main.zsharp'));
const compile=spawnSync(nativePath,['compile',path.join(temp,'Main.zsharp'),'-o',path.join(temp,'Main.zbc')],{encoding:'utf8'});
assert.equal(compile.status,0,compile.stderr);
const bytecode=spawnSync(nativePath,['run-bytecode',path.join(temp,'Main.zbc')],{encoding:'utf8'});
assert.equal(bytecode.status,0,bytecode.stderr);assert.equal(bytecode.stdout,native.stdout);
for(const expr of ['Time.convert("2026-02-29","00:00",UTC)','Time.getMS(4)','Time.since("abc")','Time.since(alive)','Time.convert("2026-01-02","15:04:05", "invalid")']) {
 fs.writeFileSync(path.join(temp,'Bad.zsharp'),`zsharp=type.script noticed room Main[] (noticed brain Start[] (Print(${expr}):))`);
 const bad=spawnSync(nativePath,['run',path.join(temp,'Bad.zsharp')],{encoding:'utf8'});
 assert.notEqual(bad.status,0,expr);assert.match(bad.stderr+bad.stdout,/Time/);
}
fs.writeFileSync(path.join(temp,'project.zsettings'),settings.replace('[1.3.0.0]','[1.2.2.0]'));
const old=spawnSync(nativePath,['compile',path.join(temp,'Main.zsharp'),'-o',path.join(temp,'old.zbc')],{encoding:'utf8'});
assert.notEqual(old.status,0);assert.match(old.stderr,/Time\/networking requires ZSharp/);
console.log('PASS: deterministic browser Time conversions, durations, getters, nested expressions and errors');
console.log('PASS: desktop Time source/bytecode execution, invalid inputs and minimum project version');
