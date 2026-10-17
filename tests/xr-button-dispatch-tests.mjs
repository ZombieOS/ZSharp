import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';

const root=fs.mkdtempSync(path.join(os.tmpdir(),'zsharp-xr-buttons-'));
const source=path.resolve('examples/pcvr-test');
const exe=path.resolve('build/Release/zsharp.exe');
const sample=fs.readFileSync(path.join(source,'Main.zsharp'),'utf8');
try {
 assert.doesNotMatch(sample,/\)\.update:/,'demo must not continuously rewrite wrapping telemetry');
 for(const name of ['Blue feedback','Coral feedback'])
  assert.ok(sample.includes(`[UI CLICK] ${name}`));
 assert.doesNotMatch(sample,/passthrough|TogglePassthrough/i,'deferred MR must not remain in the Lab script');
 assert.doesNotMatch(fs.readFileSync(path.join(source,'Lab.zscene'),'utf8'),/PassthroughButton|PassthroughToggle/);
 assert.equal(fs.existsSync(path.join(source,'PassthroughButton.zobject')),false);
 fs.cpSync(source,root,{recursive:true,filter:file=>!path.relative(source,file).split(path.sep).includes('Packages')});
 const settings=fs.readFileSync(path.join(root,'project.zsettings'),'utf8')
  .replace('zsharp_pcvr_lab','zsharp_xr_button_dispatch_test');
 fs.writeFileSync(path.join(root,'project.zsettings'),settings);
 // Keep the real endless Start loop: callbacks in the SAME room must run while
 // it waits, and its next iteration must observe their shared state updates.
 // No physical headset is initialized; only the dispatcher input is synthetic.
 const program=sample
  .replace(' noticed number UIActivations = 0:', ' noticed number UIActivations = 0:\n noticed text CallbackState = "initial":')
  .replace(' noticed brain Start[] (', ' noticed brain Start[] (\n  Print("[FIXTURE] Active polling regression"):\n  text LocalState = CallbackState:\n  number LocalCount = UIActivations:')
  .replaceAll('  number.set:UIActivations = UIActivations + 1:', '  number.set:UIActivations = UIActivations + 1:\n  text.set.CallbackState = "updated":')
  .replace('   wait(8ms):',`   if[UIActivations == 6 and CallbackState == "updated"] (
    Print("[ROOM STATE] Poll loop observed six callbacks"):
    if[LocalState == "initial" and LocalCount == 0] (
     Print("[ROOM STATE] Task-local snapshots survived shared updates"):
    ) else ()
    Application.Quit:
   ) else ()
   wait(8ms):`);
 for(const nested of [false,true]) {
 const nestedProgram=program
  .replace(' import ZSharp.XR():', ' import ZSharp.XR():\n import zsharp_xr_button_dispatch_test.Main():')
  .replace('  number LocalCount = UIActivations:', '  number LocalCount = UIActivations:\n  Function.call(Main:Main:AwaitCallbacks [CallbackState, UIActivations]):')
  .replace(' noticed brain Shutdown[] (', ` noticed brain AwaitCallbacks[text OriginalText, number OriginalNumber] (
  loop (
   if[UIActivations == 6] (
    if[OriginalText == "initial" and OriginalNumber == 0] (
     Print("[ROOM STATE] Nested parameters survived shared updates"):
    ) else ()
    loop.end:
   ) else ()
   wait(8ms):
  )
 )
 noticed brain Shutdown[] (`);
 fs.writeFileSync(path.join(root,'Main.zsharp'),nested?nestedProgram:program);
 let result=spawnSync(exe,['package','game',root,'XR-Buttons'],{encoding:'utf8',timeout:30000});
 assert.equal(result.status,0,result.stdout+result.stderr);
 result=spawnSync(exe,['open',path.join(root,'Packages','XR-Buttons.zgame')],{
  encoding:'utf8',timeout:20000,
  env:{...process.env,ZSHARP_XR_MODE:'desktop',ZSHARP_XR_UI_TEST:'1',ZSHARP_XR_UI_TRACE:'1',ZSHARP_GAME_AUTOCLOSE_MS:'5000',ZSHARP_SKIP_DESKTOP_INTEGRATION:'1',ZSHARP_SKIP_UPDATE_CHECK:'1'},
 });
 assert.equal(result.status,0,result.stdout+result.stderr);
 assert.equal((result.stdout.match(/\[UI CLICK\] Blue feedback/g)||[]).length,3,result.stdout);
 assert.equal((result.stdout.match(/\[UI CLICK\] Coral feedback/g)||[]).length,3,result.stdout);
 assert.equal((result.stdout.match(/\[UI\] Total button clicks:/g)||[]).length,6,result.stdout);
 assert.match(result.stdout,/\[ROOM STATE\] Poll loop observed six callbacks/);
 assert.match(result.stdout,/\[ROOM STATE\] Task-local snapshots survived shared updates/);
 if(nested)assert.match(result.stdout,/\[ROOM STATE\] Nested parameters survived shared updates/);
 const counts=[...result.stdout.matchAll(/\[UI\] Total button clicks: (\d+)/g)].map(match=>Number(match[1])).sort((a,b)=>a-b);
 assert.deepEqual(counts,[1,2,3,4,5,6],'room updates must not be lost or overwritten by a waiting Start task');
 assert.doesNotMatch(result.stdout,/\[UI CLICK\] Toggle passthrough/);
 assert.doesNotMatch(result.stdout,/Head:|\[Status\]/,'desktop Start loop must not flood status');
 assert.match(result.stdout,/\[XR UI\] Left trigger: .*fire=1/);
 assert.match(result.stdout,/\[XR UI\] Right trigger: .*fire=1/);
 }
 console.log('PASS: real XR dispatch -> VM -> demo console callbacks, both hands, held/focus/scene guards and no status flood');
}finally {
 spawnSync(exe,['hub','remove','zsharp_xr_button_dispatch_test'],{encoding:'utf8',timeout:15000});
 fs.rmSync(root,{recursive:true,force:true});
}
