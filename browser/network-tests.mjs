import http from 'node:http';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawn} from 'node:child_process';
import assert from 'node:assert/strict';
import vm from 'node:vm';

// Loopback-only integration fixture. Never contacts external services.
const server = http.createServer((req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  if (req.url === '/slow') return setTimeout(() => res.end('late'), 200);
  res.writeHead(req.url === '/missing' ? 404 : 200, {'Content-Type':'text/plain'});
  res.end('network-ok');
});
server.on('upgrade', (req, socket) => {
  const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  socket.on('data', buffer => {
    if ((buffer[0] & 15) === 8) return socket.end(Buffer.from([0x88,2,3,232]));
    const size = buffer[1] & 127;
    if (size >= 126 || buffer.length < size + 6) return;
    const data = Buffer.from(buffer.subarray(6, 6 + size));
    for (let i=0; i<size; i++) data[i] ^= buffer[2 + i%4];
    socket.write(Buffer.concat([Buffer.from([0x81,size]),data]));
  });
  socket.on('error',()=>{});
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const port = server.address().port;
const root = fs.mkdtempSync(path.join(os.tmpdir(),'zsharp-network-'));
fs.writeFileSync(path.join(root,'project.zsettings'),`zsharp = type.settings
Project: "Network Test":
PID: "network_test":
Version: [1.0.0.0]:
Authors: ["Test"]:
Description: "Local network integration":
ZSharp: [1.3.0.0]:
Dependencies ():
`);
const source = `zsharp = type.script
noticed room Main[] (
 import ZSharp.HTTP():
 import ZSharp.Websocket():
 noticed brain Start[] (
  HTTP.request(
   name: "Get":
   timeout: 5s:
   headers[JSON] ( {"X-ZSharp-Test":"yes"} )
   url: "http://127.0.0.1:${port}/missing":
   onResponse: [Main:Main:Response]:
   onError: [Main:Main:Error]:
  ):
  HTTP.request(
   name: "Cancel":
   url: "http://127.0.0.1:${port}/slow":
   onResponse: [Main:Main:Response]:
   onError: [Main:Main:Cancelled]:
  ):
  HTTP.requestCancel("Cancel"):
  Websocket.connect("ws://127.0.0.1:${port}/", "Game"):
  Websocket.callback("Game", [Main:Main:Message]):
  Websocket.connected("Game", [Main:Main:Opened]):
  Websocket.error("Game", [Main:Main:Error]):
 )
 noticed brain Response[number Status, text Body] (
  Print("HTTP " + Status + " " + Body):
  HTTP.request(
   name: "Followup":
   url: "http://127.0.0.1:${port}/":
   method: "POST":
   body: "followup":
   onResponse: [Main:Main:Followup]:
   onError: [Main:Main:Error]:
  ):
 )
 noticed brain Followup[number Status, text Body] ( Print("FOLLOWUP " + Status): )
 noticed brain Cancelled[text Message] ( Print("CANCEL " + Message): )
 noticed brain Error[text Message] ( Print("ERROR " + Message): )
 noticed brain Opened[] ( Websocket.send("Game", "echo-ok"): )
 noticed brain Message[text Body] ( Print("WS " + Body): Websocket.close("Game"): )
)
`;
fs.writeFileSync(path.join(root,'Main.zsharp'),source);
try {
  const result = await new Promise((resolve,reject)=>{
    const child=spawn(path.resolve('build/Release/zsharp.exe'),['run',path.join(root,'Main.zsharp')],{env:process.env});
    let output='';child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>output+=b);
    const timer=setTimeout(()=>{child.kill();reject(Error('network timeout: '+output));},15000);
    child.on('exit',code=>{clearTimeout(timer);resolve({code,output});});child.on('error',reject);
  });
  assert.equal(result.code,0,result.output);
  assert.match(result.output,/HTTP 404 network-ok/);
  assert.match(result.output,/CANCEL Request cancelled/);
  assert.match(result.output,/WS echo-ok/);
  assert.match(result.output,/FOLLOWUP 200/);
  assert.doesNotMatch(result.output,/ERROR /);
  console.log('PASS: desktop HTTP status/body, cancellation, WebSocket connect/send/receive/close and callback lifetime');
  const logs=[], errors=[], cleanup=[];
  const origin=`http://127.0.0.1:${port}/`;
  const context={URL,TextEncoder,TextDecoder,WebAssembly,Uint8Array,DataView,
    AbortController,WebSocket,Headers,setTimeout,clearTimeout,
    location:{href:origin},navigator:{userAgent:'test'},
    document:{currentScript:{src:origin+'bzvm.js',dataset:{}},readyState:'complete',
      querySelectorAll:selector=>selector==='[id]'?[]:[{src:origin+'Main.zsharp',dataset:{}}]},
    window:{addEventListener:(kind,fn)=>{if(kind==='pagehide')cleanup.push(fn);},dispatchEvent:()=>{}},
    CustomEvent:class {constructor(type,options){this.type=type;this.detail=options?.detail;}},
    console:{log:value=>logs.push(String(value)),error:(...values)=>errors.push(values.join(' '))},
    fetch:async (url,options)=>String(url).endsWith('bzvm.wasm')?new Response(fs.readFileSync('browser/dist/bzvm.wasm')):
      String(url).endsWith('project.zsettings')?new Response(fs.readFileSync(path.join(root,'project.zsettings'))):
      String(url).endsWith('Main.zsharp')?new Response(source):fetch(url,options)
  };
  vm.runInNewContext(fs.readFileSync('browser/dist/bzvm.js','utf8'),context);
  const deadline=Date.now()+10000;
  while(Date.now()<deadline&&(!logs.includes('WS echo-ok')||!logs.includes('FOLLOWUP 200'))&&!errors.length)
    await new Promise(resolve=>setTimeout(resolve,20));
  for(const fn of cleanup)fn();
  assert.deepEqual(errors,[]);
  assert.ok(logs.includes('HTTP 404 network-ok'),logs.join('\n'));
  assert.ok(logs.includes('CANCEL Request cancelled'),logs.join('\n'));
  assert.ok(logs.includes('WS echo-ok'),logs.join('\n'));
  assert.ok(logs.includes('FOLLOWUP 200'),logs.join('\n'));
  console.log('PASS: real browser loader/WASM HTTP, cancellation and WebSocket callbacks against loopback server');
} finally {server.closeAllConnections();server.close();fs.rmSync(root,{recursive:true,force:true});}
