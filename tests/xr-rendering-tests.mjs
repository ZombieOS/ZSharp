import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
const root=fs.mkdtempSync(path.join(os.tmpdir(),'zsharp-xr-render-'));
const exe=path.resolve('build/Release/zsharp.exe');
try {
 for(const file of ['Cube.zobject','Prism.zmodel','BlueButton.zobject','CoralButton.zobject','Lab.zscene'])fs.copyFileSync('examples/pcvr-test/'+file,path.join(root,file));
 fs.cpSync('examples/pcvr-test/Assets',path.join(root,'Assets'),{recursive:true});
 const settings=fs.readFileSync('examples/pcvr-test/project.zsettings','utf8')
  .replace('zsharp_pcvr_lab','zsharp_xr_render_test').replace('Z# PCVR Lab','Z# XR Render Test');
 fs.writeFileSync(path.join(root,'project.zsettings'),settings);
 fs.writeFileSync(path.join(root,'Main.zsharp'),'zsharp=type.script noticed room Main[] (noticed brain Start[] (Print("XR WORLD TEST START"):))');
 fs.copyFileSync('build/_deps/ufbx-src/data/max_transformed_skin_7500_ascii.fbx',path.join(root,'model.fbx'));
 // Authored 2x2 PNG fixture, not a downloaded/distributed third-party image.
 const {deflateSync}=await import('node:zlib');
 const crc=bytes=>{let c=0xffffffff;for(const b of bytes){c^=b;for(let bit=0;bit<8;bit++)c=(c>>>1)^((c&1)?0xedb88320:0);}return (c^0xffffffff)>>>0;};
 const chunk=(name,data)=>{const type=Buffer.from(name),body=Buffer.concat([type,data]),size=Buffer.alloc(4),sum=Buffer.alloc(4);size.writeUInt32BE(data.length);sum.writeUInt32BE(crc(body));return Buffer.concat([size,body,sum]);};
 const header=Buffer.alloc(13);header.writeUInt32BE(2);header.writeUInt32BE(2,4);header[8]=8;header[9]=6;
 const rgba=Buffer.from([0,255,255,255,255,40,90,220,255,0,40,90,220,255,255,255,255,255]);
 const png=Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',header),chunk('IDAT',deflateSync(rgba)),chunk('IEND',Buffer.alloc(0))]);
 fs.writeFileSync(path.join(root,'checker.png'),png);
 let result=spawnSync(exe,['package','game',root,'XR-Render'],{encoding:'utf8',timeout:30000});
 assert.equal(result.status,0,result.stdout+result.stderr);
 result=spawnSync(exe,['open',path.join(root,'Packages','XR-Render.zgame')],{
  encoding:'utf8',timeout:30000,
  env:{...process.env,ZSHARP_XR_MODE:'desktop',ZSHARP_GAME_AUTOCLOSE_MS:'600',ZSHARP_GAME_WORLD_TEST:root,ZSHARP_SKIP_UPDATE_CHECK:'1'},
 });
 assert.equal(result.status,0,result.stdout+result.stderr);
 assert.match(result.stdout+result.stderr,/PASS: XR shared world/);
 assert.match(result.stdout,/XR WORLD TEST START/);
 console.log('PASS: real shared XR world export with PNG textures, skinned FBX poses, point/spot/directional lighting, moved shadows and transforms');
} finally {
 spawnSync(exe,['hub','remove','zsharp_xr_render_test'],{encoding:'utf8',timeout:15000});
 fs.rmSync(root,{recursive:true,force:true});
}
