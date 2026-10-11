/* Z# 1.2.2.0 browser prototype. JavaScript is host glue, not a Z# interpreter. */
(() => {
  "use strict";
  const loader = document.currentScript;
  const wasmURL = new URL("bzvm.wasm", loader?.src || location.href);
  const encoder = new TextEncoder(), decoder = new TextDecoder();
  const report = error => {
    console.error("[bZVM]", error);
    window.dispatchEvent(new CustomEvent("bzvm:error", {detail: String(error.message || error)}));
  };
  async function boot() {
    const scripts = [...document.querySelectorAll('script[type="text/zsharp"]')];
    if (!scripts.length) return;
    if (scripts.length !== 1) throw new Error("The bZVM prototype supports one Z# script per page.");
    const bindings = new Map();
    const timers = new Map();
    let stopped = false;
    let instance, hostError;
    const memory = () => new Uint8Array(instance.exports.memory.buffer);
    const read = pointer => {
      const bytes = memory(); let end = pointer;
      while (end < bytes.length && bytes[end]) end++;
      return decoder.decode(bytes.subarray(pointer, end));
    };
    const allocate = value => {
      const bytes = encoder.encode(value), pointer = instance.exports.malloc(bytes.length + 1);
      if (!pointer) throw new Error("bZVM allocation failed");
      memory().set(bytes, pointer); memory()[pointer + bytes.length] = 0;
      return pointer;
    };
    const guard = (callback, fallback) => (...args) => {
      try { return callback(...args); } catch (error) { hostError = error; return fallback; }
    };
    const element = id => {
      const matches = [...document.querySelectorAll("[id]")].filter(node => node.id === id);
      if (matches.length !== 1) throw new Error(`HTML ID '${id}' must identify exactly one element.`);
      return matches[0];
    };
    const resolve = path => {
      const parts = path.split(".");
      if (parts[0] !== "Browser" || parts.length < 3) throw new Error("Invalid Browser path");
      const node = element(parts[1]);
      if (parts.length === 4 && parts[2] === "style") return {node, style: parts[3]};
      if (parts.length !== 3 || !["content", "html", "value"].includes(parts[2])) throw new Error(`Unsupported Browser property: ${path}`);
      if (parts[2] === "value" && !("value" in node)) throw new Error(`Element '${parts[1]}' has no input value`);
      return {node, property: {content: "textContent", html: "innerHTML", value: "value"}[parts[2]]};
    };
    const imports = {bzvm: {
      now: () => globalThis.performance?.now?.() ?? Date.now(),
      schedule: guard((task, milliseconds) => {
        if (stopped) return 0;
        if (timers.has(task)) clearTimeout(timers.get(task));
        timers.set(task, setTimeout(() => {
          timers.delete(task);
          if (stopped) return;
          try {
            hostError = undefined;
            if (!instance.exports.bzvm_resume(task)) throw hostError || new Error(read(instance.exports.bzvm_error()));
          } catch (error) { report(error); }
        }, Math.ceil(milliseconds)));
        return 1;
      }, 0),
      read: guard((pathPointer, output, capacity) => {
        const {node, property, style} = resolve(read(pathPointer));
        const bytes = encoder.encode(style ? node.style[style] : String(node[property] ?? ""));
        if (bytes.length >= capacity) throw new Error("Browser value exceeds 64 KiB prototype limit");
        memory().set(bytes, output); memory()[output + bytes.length] = 0; return bytes.length;
      }, -1),
      write: guard((pathPointer, valuePointer) => {
        const {node, property, style} = resolve(read(pathPointer));
        const value = read(valuePointer);
        if (style) {
          if (!(style in node.style)) throw new Error(`Unknown CSS property '${style}'`);
          node.style[style] = value;
        } else node[property] = value;
        return 1;
      }, 0),
      bind: guard((idPointer, targetPointer) => {
        const id = read(idPointer), target = read(targetPointer), node = element(id);
        if (bindings.has(id)) bindings.get(id)();
        const callback = () => {
          let pointer;
          try {
            hostError = undefined; pointer = allocate(target);
            if (!instance.exports.bzvm_call(pointer)) throw hostError || new Error(read(instance.exports.bzvm_error()));
          } catch (error) { report(error); }
          finally { if (pointer) instance.exports.free(pointer); }
        };
        node.addEventListener("click", callback);
        bindings.set(id, () => node.removeEventListener("click", callback)); return 1;
      }, 0),
      raw: guard((kind, sourcePointer) => {
        const source = read(sourcePointer);
        if (kind === 1) new Function(source)();
        else { const style = document.createElement("style"); style.textContent = source; document.head.append(style); }
        return 1;
      }, 0),
      print: pointer => console.log(read(pointer))
    }, wasi_snapshot_preview1: {
      clock_time_get: (clock, precision, output) => {
        if (clock !== 0 && clock !== 1) return 28;
        const milliseconds = clock === 0 ? Date.now() : (globalThis.performance?.now?.() ?? Date.now());
        new DataView(instance.exports.memory.buffer).setBigUint64(output, BigInt(Math.floor(milliseconds * 1000000)), true);
        return 0;
      },
      args_sizes_get: (argc, size) => { const d = new DataView(instance.exports.memory.buffer); d.setUint32(argc,0,true); d.setUint32(size,0,true); return 0; },
      args_get: () => 0,
      environ_sizes_get: (count, size) => { const d = new DataView(instance.exports.memory.buffer); d.setUint32(count,0,true); d.setUint32(size,0,true); return 0; },
      environ_get: () => 0,
      fd_write: (fd, iovs, count, output) => {
        const d = new DataView(instance.exports.memory.buffer); let total=0, parts=[];
        for(let i=0;i<count;i++){const ptr=d.getUint32(iovs+i*8,true),len=d.getUint32(iovs+i*8+4,true);parts.push(decoder.decode(memory().subarray(ptr,ptr+len)));total+=len;}
        (fd===2?console.error:console.log)(parts.join("")); d.setUint32(output,total,true); return 0;
      },
      fd_close: () => 8, fd_seek: () => 8, fd_fdstat_get: () => 8,
      path_open: () => 76, fd_read: () => 76,
      proc_exit: code => {throw new Error(`bZVM exited (${code})`);},
      random_get: (ptr, len) => { crypto.getRandomValues(memory().subarray(ptr,ptr+len));return 0; }
    }};
    const response = await fetch(wasmURL);
    if (!response.ok) throw new Error(`bZVM download failed: HTTP ${response.status}`);
    const result = await WebAssembly.instantiate(await response.arrayBuffer(), imports);
    instance = result.instance;
    if (instance.exports._initialize) instance.exports._initialize();
    let project;
    let settingsURL = new URL(loader?.dataset?.project || 'project.zsettings', document.baseURI || location.href);
    let settingsResponse;
    for (let level = 0; level < 32; level++) {
      if (settingsURL.origin !== new URL(location.href).origin) throw new Error("Project settings must be served from the page's own origin.");
      const response = await fetch(settingsURL);
      if (response.ok) { settingsResponse = response; break; }
      if (response.status !== 404 || loader?.dataset?.project) throw new Error(`Project settings download failed: HTTP ${response.status}`);
      const parent = new URL('../project.zsettings', settingsURL);
      if (parent.href === settingsURL.href) break;
      settingsURL = parent;
    }
    if (settingsResponse) {
      const settingsSource = await settingsResponse.text();
      if (encoder.encode(settingsSource).length > 1048576) throw new Error("Project settings exceed the 1 MiB prototype limit");
      const pointer = allocate(settingsSource);
      try {
        if (!instance.exports.bzvm_settings(pointer)) throw new Error(read(instance.exports.bzvm_error()));
        project = {id: read(instance.exports.bzvm_project_id()), name: read(instance.exports.bzvm_project_name()), root: new URL(".", settingsURL).href};
      } finally { instance.exports.free(pointer); }
    }
    const script = scripts[0];
    let source = script.textContent;
    let name = script.dataset.name || "Main";
    if (script.src) {
      const response = await fetch(script.src);
      if (!response.ok) throw new Error(`Z# script download failed: HTTP ${response.status}`);
      source = await response.text(); name = new URL(script.src).pathname.split("/").pop().replace(/\.zsharp$/i, "");
    }
    instance.exports.bzvm_reset();
    const pending = [{source, name, url: script.src || location.href}];
    const requested = new Set([new URL(script.src || location.href).href]);
    const filenames = new Set();
    let totalBytes = 0;
    for (let index = 0; index < pending.length; index++) {
      const module = pending[index];
      if (filenames.has(module.name)) throw new Error(`Duplicate Z# filename '${module.name}'; use unique script filenames.`);
      filenames.add(module.name);
      totalBytes += encoder.encode(module.source).length;
      if (totalBytes > 8388608 || pending.length > 64) throw new Error('Browser project exceeds the 64-file / 8 MiB prototype limit');
      const sourcePointer = allocate(module.source), namePointer = allocate(module.name);
      let moduleIndex;
      try {
        moduleIndex = instance.exports.bzvm_load(sourcePointer,namePointer) - 1;
        if (moduleIndex < 0) throw new Error(read(instance.exports.bzvm_error()));
      } finally { instance.exports.free(sourcePointer);instance.exports.free(namePointer); }
      for (let i = 0; i < instance.exports.bzvm_import_count(moduleIndex); i++) {
        const path = read(instance.exports.bzvm_import(moduleIndex,i));
        if (path === 'ZSharp.Browser') continue;
        if (!project) throw new Error('File imports require project.zsettings in the project root.');
        const parts = path.split('.');
        if (parts.shift() !== project.id) throw new Error(`Import '${path}' is not part of project '${project.id}'; external dependencies are not supported yet.`);
        if (!parts.length || parts.some(part => !/^[A-Za-z_][A-Za-z0-9_]*$/.test(part))) throw new Error(`Import '${path}' requires an explicit project-relative file path; wildcard imports are not supported yet.`);
        const url = new URL(parts.join('/') + '.zsharp',project.root);
        if (requested.has(url.href)) continue;
        requested.add(url.href);
        if (requested.size > 64) throw new Error('Browser project exceeds the 64-file prototype limit');
        const response = await fetch(url);
        if (!response.ok) throw new Error(`Could not load import '${path}': HTTP ${response.status} (${url.pathname})`);
        pending.push({source: await response.text(),name:parts.at(-1),url:url.href});
      }
    }
    if (!instance.exports.bzvm_run()) throw hostError || new Error(read(instance.exports.bzvm_error()));
    window.addEventListener("pagehide",()=>{
      stopped = true;
      for(const timer of timers.values())clearTimeout(timer);
      timers.clear(); instance.exports.bzvm_cancel();
      for(const dispose of bindings.values())dispose(); bindings.clear();
    },{once:true});
    window.dispatchEvent(new CustomEvent("bzvm:ready", {detail: {project}}));
  }
  if(document.readyState==="loading")document.addEventListener("DOMContentLoaded",()=>boot().catch(report),{once:true});
  else boot().catch(report);
})();
