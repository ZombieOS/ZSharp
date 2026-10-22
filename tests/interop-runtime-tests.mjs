import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const executable = path.resolve(process.argv[2] || path.join(root, 'build/Release/zsharp.exe'));
const managed = path.resolve(process.argv[3] || path.join(root, 'build-interop-managed'));
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'zsharp-interop-runtime-'));
const env = { ...process.env };
const localRuby = path.join(root, 'build-tools/ruby/rubyinstaller-3.4.11-1-x64/bin/ruby.exe');
if (!env.ZSHARP_RUBY && process.platform === 'win32' && fs.existsSync(localRuby)) env.ZSHARP_RUBY = localRuby;
let checks = 0;
const write = (name, text) => fs.writeFileSync(path.join(temporary, name), text);
function command(program, args, options = {}) {
  const result = spawnSync(program, args, { cwd: temporary, encoding: 'utf8', windowsHide: true,
    timeout: 90000, env, ...options });
  assert.equal(result.error, undefined, String(result.error));
  assert.equal(result.status, 0, result.stdout + result.stderr);
  return result;
}
function run(source, expected, failure) {
  write('Main.zsharp', source);
  const result = spawnSync(executable, ['run', path.join(temporary, 'Main.zsharp')], {
    cwd: temporary, encoding: 'utf8', windowsHide: true, timeout: 20000, env });
  assert.equal(result.error, undefined, String(result.error));
  if (failure) {
    assert.notEqual(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout + result.stderr, failure);
  } else {
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.deepEqual(result.stdout.trim().split(/\r?\n/), expected);
  }
  checks++;
}
function runBytecode(source, expected) {
  write('Main.zsharp', source);
  command(executable, ['compile', path.join(temporary, 'Main.zsharp'), '-o', path.join(temporary, 'Main.zbc')]);
  const result = command(executable, ['run-bytecode', path.join(temporary, 'Main.zbc')]);
  assert.deepEqual(result.stdout.trim().split(/\r?\n/), expected);
  checks++;
}
write('project.zsettings', `zsharp = type.settings
Project: "Interop runtime tests":
PID: "interop":
Version: [1.4.0.0]:
Authors: ["ZSharp tests"]:
Description: "Owned temporary test project":
ZSharp: [1.4.0.0]:
Dependencies ():
`);
write('Tests.zsharp', `zsharp = type.script
noticed room Bridge[] (
 noticed number Double[number Value] ( feed(Value * 2): )
 silent number Hidden[number Value] ( feed(Value * 2): )
)
`);
const objectRoom = `noticed room Counter[] (
 noticed number value = 7:
 silent number secret = 99:
 noticed number increase[number Amount] (
  number.set:value = value + Amount:
  feed(value):
 )
)
`;
function script(language, body, imports = true) {
  return `zsharp = type.script
noticed room Main[] (
 ${imports ? `import ${language}:interop.Sample():` : ''}
 import interop.Tests():
 noticed Counter Local = new Counter[]:
 noticed brain Start[] (
  ${body}
 )
)
${objectRoom}`;
}
const ordinary = language => script(language, `
 Print(Function.call(${language}:Sample:add [20, 22])):
 Print(Function.call(${language}:Sample:counted [])):
 Print(Function.call(${language}:Sample:counted [])):
 object First = Foreign.new(${language}:Sample [10]):
 object Second = Foreign.new(${language}:Sample [70]):
 Print(First.value):
 number.set:First.value = 20:
 First.increase[2]:
 Print(First.value):
 Print(Second.value):
 Print(Function.call(${language}:Sample:callback [9])):
 Print(Function.call(${language}:Sample:proxy [Local])):
 Print(Local.value):
 Foreign.release(First):
 Foreign.release(Second):
`);
const expected = ['42', '1', '2', '10', '22', '70', '18', '28', '28'];
const multipleStarts = language => `zsharp = type.script
noticed room First[] (
 import ${language}:interop.Sample():
 noticed brain Start[] ( Print(Function.call(${language}:Sample:counted [])): )
)
noticed room Second[] (
 import ${language}:interop.Sample():
 noticed brain Start[] ( Print(Function.call(${language}:Sample:counted [])): )
)
`;

try {
  for (const language of ['java', 'cs']) {
    const compiled = language === 'java' ? path.join(managed, 'Sample.zjava.jar') : path.join(managed, 'csharp-sample/Sample.zcs.dll');
    if (!fs.existsSync(compiled)) throw new Error(`Run tests/interop-managed/run.mjs first: missing ${compiled}`);
    fs.copyFileSync(compiled, path.join(temporary, path.basename(compiled)));
    write(`Sample.${language === 'java' ? 'java' : 'cs'}`, '// Compiled test module; source endpoint marker.\n');
    run(ordinary(language), expected);
    if (language === 'java') {
      runBytecode(ordinary(language), expected);
      run(multipleStarts(language), ['1', '2']);
      runBytecode(multipleStarts(language), ['1', '2']);
    }
    run(script(language, `object Value = Foreign.new(${language}:Sample [1]):
      Foreign.release(Value): Print(Value.value):`), null, /released/);
    run(script(language, `object Value = Foreign.new(${language}:Sample [1]): Print(Value.secret):`), null, /export|member|field|visible/i);
    run(script(language, `Print(Function.call(${language}:Sample:callback [1])):`, false), null, /import/i);
  }
  const python = `from zsharp import export, call
_calls = 0
@export
class Sample:
    def __init__(self, value): self.value = value
    def increase(self, amount):
        self.value += amount
        return self.value
@export
def add(a, b): return a + b
@export
def counted():
    global _calls
    _calls += 1
    return _calls
@export
def callback(value): return call("Tests:Bridge:Double", value)
@export
def private_callback(value): return call("Tests:Bridge:Hidden", value)
@export
def proxy(value):
    old = value.get("value")
    value.set("value", 21)
    return value.invoke("increase", old)
@export
def make(value): return Sample(value)
@export
def callbackVoid():
    call("Main:Main:Observe")
    return "ok"
@export
def reenter(): return call("Main:Main:Reenter")
@export
def proxyHidden(value): return value.get("secret")
@export
def record(): return {"name":"Ada", "count":12}
@export
def echo(value): return value
`;
  write('Sample.py', python);
  run(ordinary('py'), expected);
  run(script('py', `object Value = Function.call(py:Sample:make [12]): Print(Value.value):
      number.set:Value.value = 30: Print(Value.value):`), ['12', '30']);
  run(script('py', 'Print(Function.call(py:Sample:private_callback [1])):'), null, /silent|not visible/i);
  write('Other.py', `from zsharp import export
@export
def identity(value): return value
`);
  run(script('py', 'object Value = Foreign.new(py:Sample [1]): Print(Function.call(py:Other:identity [Value])):')
    .replace('import py:interop.Sample():', 'import py:interop.Sample():\n import py:interop.Other():'), null, /another module|another language|belongs/i);
  run(script('py', `Print(Function.call(py:Sample:callbackVoid [])):`).replace(
    'noticed brain Start[] (', 'noticed brain Observe[] ( Print("observed"): )\n noticed brain Start[] ('), ['observed', 'ok']);
  run(script('py', `object Value = Foreign.new(py:Sample [1]): object Alias = Value:
      Foreign.release(Value): Print(Alias.value):`), null, /released/);
  run(script('py', `Print(Function.call(py:Sample:proxyHidden [Local])):`), null, /visible/);
  run(script('py', `object Data = Function.call(py:Sample:record []):
      object Copy = Function.call(py:Sample:echo [Data]): Print(Copy.name): Print(Copy.count):
      Print(Function.call(py:Sample:echo [12345678901234567890.123456789])):`), ['Ada', '12', '12345678901234567890.123456789']);
  run(script('py', `Print(Function.call(py:Sample:reenter [])):`).replace(
    'noticed brain Start[] (', 'noticed brain Reenter[] ( Function.call(py:Sample:counted []): )\n noticed brain Start[] ('), null, /re-enter/);
  write('Sample.js', `let calls = 0;
export class Sample { constructor(value) { this.value = value; } increase(amount) { return this.value += amount; } }
export function add(a,b) { return a+b; }
export function counted() { return ++calls; }
export function callback(value) { return ZSharp.call("Tests:Bridge:Double", value); }
export function proxy(value) { const old = value.get("value"); value.set("value",21); return value.invoke("increase", old); }
export function tiny() { return 1e-7; }
`);
  run(ordinary('js'), expected);
  runBytecode(ordinary('js'), expected);
  run(multipleStarts('js'), ['1', '2']);
  runBytecode(multipleStarts('js'), ['1', '2']);
  run(script('js', 'Print(Function.call(js:Sample:tiny [])):'), ['0.0000001']);
  write('Sample.lua', `local calls=0
local M={}
function M.Sample(value) return {value=value,increase=function(self,amount) self.value=self.value+amount return self.value end} end
function M.add(a,b) return a+b end
function M.counted() calls=calls+1 return calls end
function M.callback(value) return ZSharp.call("Tests:Bridge:Double",value) end
function M.proxy(value) local old=value:get("value") value:set("value",21) return value:invoke("increase",old) end
return M
`);
  run(ordinary('lua'), expected);
  write('Sample.rb', `require "zsharp"
class Sample
  attr_accessor :value
  def initialize(value)
    @value = value
  end
  def increase(amount)
    @value += amount
  end
  private
  def secret
    "not public"
  end
end
calls = 0
ZSharp.export(Sample)
ZSharp.export("add") { |a,b| a+b }
ZSharp.export("counted") { calls += 1 }
ZSharp.export("callback") { |value| ZSharp.call("Tests:Bridge:Double", value) }
ZSharp.export("proxy") do |value|
  old = value.get("value")
  value.set("value", 21)
  value.invoke("increase", old)
end
ZSharp.export("make") { |value| Sample.new(value) }
`);
  command(env.ZSHARP_RUBY || 'ruby', ['-v']);
  run(ordinary('rb'), expected);
  run(script('rb', 'object Value = Foreign.new(rb:Sample [1]): Print(Value.secret):'), null, /not public|private|reader/i);
  run(script('rb', 'object Value = Function.call(rb:Sample:make [12]): Print(Value.value):'), ['12']);
  run(script('rb', 'object Value = Foreign.new(rb:Sample [1]): Foreign.release(Value): Print(Value.value):'), null, /released/);
  run(script('rb', 'Print(Function.call(rb:Sample:callback [1])):').replace('import interop.Tests():', ''), null, /import/i);
  write('go.mod', `module sample\n\ngo 1.22\nrequire zsharp v0.0.0\nreplace zsharp => ${JSON.stringify(path.join(root, 'native/interop/go').replaceAll('\\', '/'))}\n`);
  write('Sample.go', `package main
import "zsharp"
type Sample struct { Value int }
func (s *Sample) Increase(amount int) int { s.Value += amount; return s.Value }
func main() {
  calls := 0
  zsharp.Export("add",func(a,b int) int { return a+b })
  zsharp.Export("counted",func() int { calls++; return calls })
  zsharp.Export("callback",func(value int)(any,error) { return zsharp.Call("Tests:Bridge:Double",value) })
  zsharp.Export("proxy",func(value *zsharp.Object)(any,error) {
    old,err := value.Get("value"); if err != nil { return nil,err }
    if err=value.Set("value",21); err != nil { return nil,err }
    return value.Invoke("increase",old)
  })
  zsharp.ExportType("Sample",func(value int)*Sample { return &Sample{Value:value} })
  if err:=zsharp.Serve();err!=nil { panic(err) }
}
`);
  command('go', ['build', '-o', path.join(temporary, process.platform === 'win32' ? 'Sample.zgo.exe' : 'Sample.zgo'), 'Sample.go']);
  run(ordinary('go').replaceAll('First.value', 'First.Value').replaceAll('Second.value', 'Second.Value').replaceAll('First.increase', 'First.Increase'), expected);
  run(script('py', `Print(Function.call(py:Sample:callback [1])):`).replace('import interop.Tests():', ''), null, /import/i);
  console.log(`Interop runtime tests passed: ${checks} complete Z# executions.`);
} finally {
  fs.rmSync(temporary, { recursive: true, force: true });
}
