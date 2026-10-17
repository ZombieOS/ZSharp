import fs from 'node:fs';
import assert from 'node:assert/strict';
const module=new WebAssembly.Module(fs.readFileSync(new URL('./dist/bzvm.wasm',import.meta.url)));
let instance, pending=false;
const output=[], timers=[];
const decode=p=>{const m=new Uint8Array(instance.exports.memory.buffer);let end=p;while(m[end])end++;return new TextDecoder().decode(m.subarray(p,end));};
const files=new Map([['person.json','{"name":"Ada","age":36,"enabled":true,"phone-number":"123"}']]);
const wasi={};for(const imp of WebAssembly.Module.imports(module))if(imp.module==='wasi_snapshot_preview1')wasi[imp.name]=()=>0;
instance=new WebAssembly.Instance(module,{wasi_snapshot_preview1:wasi,bzvm:{
 now:()=>performance.now(),wall_time:()=>Date.now(),input_session:()=>{},network:()=>0,
 local_time:()=>0,convert_local_time:()=>NaN,write:()=>1,raw:()=>1,bind:()=>1,
 print:p=>output.push(decode(p)),schedule:(id,ms)=>{timers.push(id);return 1;},
 read:(p,out,capacity)=>{
  if(pending){pending=false;return -2;}
  const request=decode(p),exists=request.startsWith('File.exists:');
  const path=request.replace(/^(JSON.file:|File.exists:)/,'');
  const source=exists ? (files.has(path)?'alive':'dead') : files.get(path);if(source===undefined)return -1;
  const bytes=Buffer.from(source+'\0');if(bytes.length>capacity)return -1;
  new Uint8Array(instance.exports.memory.buffer).set(bytes,out);return bytes.length-1;
 }
}});
instance.exports._initialize?.();
function call(name,...args){const pointers=args.map(s=>{const b=Buffer.from(s+'\0'),p=instance.exports.malloc(b.length);new Uint8Array(instance.exports.memory.buffer).set(b,p);return p;});try{return instance.exports[name](...pointers);}finally{pointers.forEach(p=>instance.exports.free(p));}}
const schema='noticed JSON PersonSchema[] (name: text: age: number: enabled: status: phone-number: text:)';
function run(body,{global='',imports='import ZSharp.JSON():',error,delay=false}={}){
 instance.exports.bzvm_reset();output.length=timers.length=0;
 assert.equal(call('bzvm_load',`zsharp=type.script noticed room Main[] (${imports} ${schema} ${global} noticed brain Start[] (${body}))`,'Main'),1,decode(instance.exports.bzvm_error()));
 pending=delay;let result=instance.exports.bzvm_run();
 while(result && timers.length)result=instance.exports.bzvm_resume(timers.shift());
 if(error){assert.equal(result,0);assert.match(decode(instance.exports.bzvm_error()),error);}else assert.equal(result,1,decode(instance.exports.bzvm_error()));
}
run('text Path="person.json": JSON Person=JSON.load(PersonSchema,Path): Print(Person.name): Print(Person.age): Print(Person.enabled): Print(Person["phone-number"]):', {delay:true});
assert.deepEqual(output,['Ada','36','alive','123']);
run('Print(Person.name): Print(Main.Person.age): number.set:Person.age=40: Print(Person.age):', {global:'noticed JSON Person=JSON.load(PersonSchema,"person.json"):'});
assert.deepEqual(output,['Ada','36','40']);
files.set('precise.json','{"name":"Ada","age":900719925474099312345,"enabled":true,"phone-number":"123"}');
run('JSON Person=JSON.load(PersonSchema,"precise.json"): Print(Person.age):');
assert.deepEqual(output,['900719925474099312345']);
run('Print(File.exists("person.json")): Print(File.exists("missing.json")): text Path="notes.txt": Print(File.read(Path)):',{imports:'',delay:true,error:/could not read project file/});
assert.deepEqual(output,['alive','dead']);
files.set('notes.txt','Hello from a public project file!');
run('Print(File.read("notes.txt")):',{imports:'',delay:true});
assert.deepEqual(output,['Hello from a public project file!']);
run('JSON Person=JSON.load(PersonSchema,"person.json"): Print(Person.name):',{imports:'',error:/requires import/});
for(const source of ['{}','[]','{"name":1,"age":36,"enabled":true,"phone-number":"123"}',
 '{"name":"Ada","age":36,"enabled":"true","phone-number":"123"}',
 '{"name":"Ada","age":36,"enabled":true,"phone-number":"123","extra":0}',
 '{"name":"Ada","age":36,"enabled":true,"phone-number":"123"} trailing']) {
 files.set('bad.json',source);run('JSON Person=JSON.load(PersonSchema,"bad.json"):',{error:/does not match schema/});
}
run('JSON Person=JSON.load(PersonSchema,"missing.json"):',{error:/could not read JSON/});
for(const path of ['../private.json','/private.json','https://example.com/file','%2e%2e/private.json','Data//file.json'])
 run(`JSON Person=JSON.load(PersonSchema,"${path}"):`,{error:/safe project-relative path/});
run('JSON Person=JSON.load(PersonSchema,"person.json"): Print(Person.missing):',{error:/no field/});
run('JSON Person=JSON.load(PersonSchema,"person.json"): number.set:Person.age="bad":',{error:/declared type/});
instance.exports.bzvm_reset();
assert.equal(call('bzvm_load',`zsharp=type.script noticed room Main[] (import ZSharp.JSON(): ${schema} noticed JSON Person=JSON.load(PersonSchema,"person.json"): noticed brain Start[] (Print(Person.name):))`,'Main'),1);
assert.equal(decode(instance.exports.bzvm_json_path(0,0)),'person.json');
assert.equal(decode(instance.exports.bzvm_json_path(0,1)),'');
console.log('PASS: browser JSON schemas, asynchronous variable paths, room values, typed/bracket reads and writes, invalid files/types/imports');
