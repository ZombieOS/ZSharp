// Local-only static preview with HTTP fixtures and a text WebSocket echo endpoint.
import http from 'node:http';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
const root=path.resolve(process.argv[2]||'.'), port=Number(process.argv[3]||8781);
const mime={'.html':'text/html','.css':'text/css','.js':'text/javascript','.wasm':'application/wasm','.zsharp':'text/plain','.zsettings':'text/plain','.png':'image/png'};
const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://localhost');
  res.setHeader('Cache-Control','no-store');
  if(url.pathname==='/api/slow')return setTimeout(()=>res.end('Slow request completed.'),3000);
  if(url.pathname==='/api/hello'){res.setHeader('Content-Type','application/json');return res.end(JSON.stringify({message:'Hello from the local test server',ok:true}));}
  let filename;
  try{filename=path.resolve(root,'.'+decodeURIComponent(url.pathname==='/'?'/index.html':url.pathname));}catch{res.writeHead(400);return res.end();}
  if(filename!==root&&!filename.startsWith(root+path.sep)){res.writeHead(403);return res.end();}
  fs.readFile(filename,(error,content)=>{if(error){res.writeHead(404);return res.end('Not found');}
    res.setHeader('Content-Type',mime[path.extname(filename)]||'application/octet-stream');res.end(content);});
});
server.on('upgrade',(req,socket)=>{
  if(req.url!=='/echo'||!req.headers['sec-websocket-key'])return socket.destroy();
  const accept=crypto.createHash('sha1').update(req.headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n');
  let pending=Buffer.alloc(0);
  socket.on('data',chunk=>{
    pending=Buffer.concat([pending,chunk]);
    while(pending.length>=2){
      const opcode=pending[0]&15;let length=pending[1]&127,offset=2;
      if(!(pending[1]&128)||length===127)return socket.destroy();
      if(length===126){if(pending.length<4)return;length=pending.readUInt16BE(2);offset=4;}
      if(pending.length<offset+4+length)return;
      const mask=pending.subarray(offset,offset+4),body=Buffer.from(pending.subarray(offset+4,offset+4+length));
      pending=pending.subarray(offset+4+length);
      for(let i=0;i<body.length;i++)body[i]^=mask[i%4];
      if(opcode===8)return socket.end(Buffer.from([0x88,2,3,232]));
      if(opcode!==1&&opcode!==9)return socket.destroy();
      const header=body.length<126?Buffer.from([opcode===9?0x8a:0x81,body.length]):Buffer.from([0x81,126,body.length>>8,body.length&255]);
      socket.write(Buffer.concat([header,body]));
    }
  });
  socket.on('error',()=>{});
});
server.listen(port,'127.0.0.1',()=>console.log(`Local preview: http://127.0.0.1:${port}/`));
