import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import assert from 'node:assert/strict';
const root=fs.mkdtempSync(path.join(os.tmpdir(),'zsharp-xr-'));
const exe=path.resolve('build/Release/zsharp.exe');
const settings=fs.readFileSync('examples/pcvr-test/project.zsettings','utf8')
 .replace('PID: "zsharp_pcvr_lab"','PID: "zsharp_xr_routing_test"')
 .replace('Project: "Z# PCVR Lab"','Project: "Z# XR Routing Test"');
const main=fs.readFileSync('examples/pcvr-test/Main.zsharp','utf8');
for(const name of ['Cube.zobject','Prism.zmodel','BlueButton.zobject','CoralButton.zobject','Lab.zscene'])fs.copyFileSync('examples/pcvr-test/'+name,path.join(root,name));
fs.cpSync('examples/pcvr-test/Assets',path.join(root,'Assets'),{recursive:true});
const compile=(source,config=settings)=>{
 fs.writeFileSync(path.join(root,'Main.zsharp'),source);
 fs.writeFileSync(path.join(root,'project.zsettings'),config);
 return spawnSync(exe,['compile',path.join(root,'Main.zsharp'),'-o',path.join(root,'Main.zbc')],{encoding:'utf8',timeout:15000});
};
try {
 let result=compile(main);assert.equal(result.status,0,result.stderr);
 // Execute nested runtime paths even in desktop mode: compilation alone cannot
 // catch the VM mistaking XR.right.trigger.pressed for an external script.
 result=compile(`zsharp = type.script
 noticed room Main[] (
  import ZSharp.XR():
  noticed brain Start[] (
   number.set:XR.rig.positionX = 2:
   number.set:XR.rig.positionY = 1:
   number.set:XR.rig.positionZ = -3:
   number.set:XR.rig.rotationY = 90:
   XR.left.vibrate(0, 0ms):
   Print("XR optional hands: " + XR.hands.available + "," + XR.left.hand.tracked + "," + XR.left.hand.indexTip.positionX + "," + XR.right.hand.pinch.pressed):
   Print("XR runtime paths: " + XR.rig.positionX + "," + XR.left.positionX + "," + XR.right.stick.angle + "," + XR.right.trigger.pressed + "," + XR.left.x.released):
  )
  noticed brain Shutdown[] ( Print("XR shutdown completed"): )
 )`);
 assert.equal(result.status,0,result.stderr);
 result=spawnSync(exe,['package','game',root,'XR-Routing'],{encoding:'utf8',timeout:30000});
 assert.equal(result.status,0,result.stderr+result.stdout);
 result=spawnSync(exe,['open',path.join(root,'Packages','XR-Routing.zgame')],{
  encoding:'utf8',timeout:20000,
  env:{...process.env,ZSHARP_XR_MODE:'desktop',ZSHARP_GAME_AUTOCLOSE_MS:'600'},
 });
 assert.equal(result.status,0,result.stderr+result.stdout);
 assert.match(result.stdout,/XR runtime paths: 2(?:\.0+)?,0(?:\.0+)?,0(?:\.0+)?,dead,dead/);
 assert.match(result.stdout,/XR shutdown completed/);
 assert.match(result.stdout,/XR optional hands: dead,dead,0(?:\.0+)?,dead/);
 // Removed MR properties must fail validation rather than silently returning
 // a default or accidentally resolving them as an imported script.
 for(const statement of [
  'Print(XR.passthrough.available):',
  'Print(XR.passthrough.enabled):',
  'Print(XR.passthrough.active):',
  'XR.passthrough.enabled.set: alive:',
  'XR.passthrough.enabled.set: dead:',
 ]) {
  result=compile(`zsharp = type.script
   noticed room Main[] (
    import ZSharp.XR():
    noticed brain Start[] ( ${statement} )
   )`);
  assert.notEqual(result.status,0);
  assert.match(result.stderr+result.stdout,/unknown or read-only XR property 'XR\.passthrough\./);
 }
 for(const [source,config,expected] of [
  [main.replace('import ZSharp.XR():',''),settings,/requires import ZSharp.XR/],
  [main.replace('XR.active == alive','XR.typo == alive'),settings,/unknown or read-only XR/],
  [main.replace('number.set:Lab.LeftHand.positionX = XR.left.positionX:','number.set:XR.head.positionX = 1:'),settings,/unknown or read-only XR/],
  [main,settings.replace('required: false','required: true').replace('XR: true','XR: false'),/requires AllowedDevices XR/],
  [main,settings.replace('Windows: true:', 'Windows: true:\n Windows: false:'),/duplicate AllowedDevices/],
  [main,settings.replace('Windows: true:', 'Windows: "yes":'),/true or false/],
 ]) {
  result=compile(source,config);assert.notEqual(result.status,0);assert.match(result.stderr+result.stdout,expected);
 }
 result=compile('zsharp=type.script noticed room Main[] ( noticed brain Start[] ( Print("not allowed"): ) )',
  settings.replace('Windows: true:','Windows: false:'));
 assert.equal(result.status,0,result.stderr);
 const run=spawnSync(exe,['run-bytecode',path.join(root,'Main.zbc')],{encoding:'utf8',timeout:15000});
 assert.notEqual(run.status,0);assert.match(run.stdout+run.stderr,/does not allow/);
 console.log('PASS: XR game syntax/bytecode, import gate, read-only tracking, invalid settings and host OS restrictions');
}finally{
 spawnSync(exe,['hub','remove','zsharp_xr_routing_test'],{encoding:'utf8',timeout:15000});
 fs.rmSync(root,{recursive:true,force:true});
}
