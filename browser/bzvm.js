/* Z# 1.3.0.0 browser prototype. JavaScript is host glue, not a Z# interpreter. */
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
    // View-only authored sources: never parse or execute Z# as JavaScript.
    // An empty script anchors a source map containing the exact original text.
    const exposeSource = module => {
      if (loader?.dataset?.sources === 'false') return;
      try {
        const url = new URL(module.url, location.href);
        url.search = ''; url.hash = '';
        if (!/\.zsharp$/i.test(url.pathname)) url.pathname += '/' + module.name + '.zsharp';
        const original = url.href, generated = original + '.bzvm-view.js';
        const map = {version:3,file:generated,sources:[original],sourcesContent:[module.source],names:[],mappings:'AAAA'};
        new Function('/* bZVM source viewer only; execution remains in WebAssembly. */\n' +
          '//# sourceURL=' + generated + '\n' +
          '//# sourceMappingURL=data:application/json;charset=utf-8,' + encodeURIComponent(JSON.stringify(map)))();
      } catch (_) {
        // CSP may disallow dynamic JavaScript. Source viewing is optional;
        // never weaken CSP or prevent the actual WebAssembly runtime starting.
      }
    };
    const bindings = new Map();
    const timers = new Map();
    let stopped = false;
    let instance, hostError;
    let project;
    const jsonFiles = new Map();
    const jsonFile = path => {
      if (!path || /[\u0000-\u001f]/.test(path) || path.includes('\\') || path.includes('%') || path.includes('?') || path.includes('#') ||
          path.split('/').some(part => !part || part === '.' || part === '..' || part.includes(':')))
        throw new Error('JSON.load requires a safe project-relative path');
      const root = new URL(project?.root || './', document.baseURI || location.href);
      const url = new URL(path, root);
      if (url.origin !== new URL(location.href).origin || !url.pathname.startsWith(root.pathname))
        throw new Error('JSON file must stay inside the same-origin project');
      let entry = jsonFiles.get(url.href);
      if (!entry) {
        if(jsonFiles.size >= 128) throw new Error('Browser JSON file cache exceeds 128 files');
        entry = {source: null, error: null, controller: new AbortController()};
        entry.promise = (async () => {
          const timeout=setTimeout(()=>entry.controller.abort(),15000);
          try {
            const response = await fetch(url.href, {credentials: 'same-origin', redirect: 'error', signal:entry.controller.signal});
            entry.status=response.status;
            if(!response.ok)throw new Error(`JSON download failed: HTTP ${response.status}`);
            const reader = response.body?.getReader();
            let size=0, chunks=[];
            if(reader) {
              try {for(;;){const {value,done}=await reader.read();if(done)break;size+=value.length;
                if(size>=65536){await reader.cancel();throw new Error('JSON file exceeds 64 KiB');}chunks.push(value);}}
              finally {reader.releaseLock();}
              const bytes=new Uint8Array(size);let offset=0;for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.length;}
              entry.source=new TextDecoder('utf-8',{fatal:true}).decode(bytes);
            } else {entry.source=await response.text();if(encoder.encode(entry.source).length>=65536)throw new Error('JSON file exceeds 64 KiB');}
            if(entry.source.includes('\0'))throw new Error('JSON file contains an unsupported NUL');
          } catch(error) {entry.error=error;}
          finally {clearTimeout(timeout);}
        })();
        jsonFiles.set(url.href, entry);
      }
      return entry;
    };
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
    const keys = new Map(), inputSessions = new Map();
    const pointer = {xpos: 0, ypos: 0};
    const keyboard = (event, held) => {
      const name = String(event.key || '').toLowerCase();
      const key = keys.get(name) || {held: false, pressed: 0, released: 0};
      if (key.held !== held) { key.held = held; key[held ? 'pressed' : 'released']++; }
      keys.set(name, key);
    };
    const keydown = event => keyboard(event, true), keyup = event => keyboard(event, false);
    const blur = () => { for (const key of keys.values()) if (key.held) {key.held=false;key.released++;} };
    const move = event => {pointer.xpos=event.clientX;pointer.ypos=event.clientY;};
    window.addEventListener('keydown', keydown); window.addEventListener('keyup', keyup);
    window.addEventListener('blur', blur); window.addEventListener('pointermove', move);
    let platform = null;
    const nav = globalThis.navigator;
    const ua = nav?.userAgent || '';
    let os = /ZombieOS|\bZOS\b/i.test(ua) ? 'zos' : /Android/i.test(ua) ? 'android' :
      /iPhone|iPad|iPod/i.test(ua) || (/Macintosh/i.test(ua) && nav?.maxTouchPoints > 1) ? 'ios' :
      /Windows/i.test(ua) ? 'windows' : /Macintosh|Mac OS/i.test(ua) ? 'mac' : /Linux|CrOS/i.test(ua) ? 'linux' : null;
    let architecture = /aarch64|arm64/i.test(ua) ? 'arm64' : /x86_64|x64|Win64|WOW64/i.test(ua) ? 'x8664' : '';
    let version = /Windows NT 6\.1/.test(ua) ? '7' : /Windows NT 6\.2/.test(ua) ? '8' : /Windows NT 6\.3/.test(ua) ? '81' : '';
    if (nav?.userAgentData?.getHighEntropyValues) {
      try {
        const hints = await Promise.race([nav.userAgentData.getHighEntropyValues(['architecture','bitness','platformVersion']),
          new Promise(resolve => setTimeout(() => resolve(null), 150))]);
        if (hints) {
          if (hints.bitness === '64') architecture = hints.architecture === 'arm' ? 'arm64' : hints.architecture === 'x86' ? 'x8664' : architecture;
          const major = Number.parseInt(hints.platformVersion, 10);
          if (os === 'windows' && major >= 1) version = major >= 13 ? '11' : '10';
        }
      } catch (_) { /* Restricted hints leave a less specific platform. */ }
    }
    platform = os ? (os === 'windows' && version ? 'win' + version : os) + architecture : null;
    const elements = id => {
      const selector = /^([^\[]+)(?:\[(\*|\d+)\])?$/.exec(id);
      if (!selector) throw new Error('Invalid Browser element selector');
      const matches = [...document.querySelectorAll('[id]')].filter(node => node.id === selector[1]);
      if (!selector[2] && !matches.length) throw new Error(`HTML ID '${selector[1]}' was not found`);
      return selector[2] === '*' ? matches : matches.slice(0, Number(selector[2] || 1));
    };
    const resolve = path => {
      const parts = path.split(".");
      if (parts[0] !== "Browser" || parts.length < 3) throw new Error("Invalid Browser path");
      if (parts[1] === 'page' && parts[2] === 'title') return {nodes:[document],property:'title'};
      if (/^readelement\[title\]$/.test(parts[2]) &&
          (parts[1] === 'page' || parts[1] === new URL(location.href).pathname.split('/').at(-1)?.replace(/\.html$/, '')))
        return {nodes:[document],property:'title'};
      const nodes = elements(parts[1]), multiple = parts[1].includes('[');
      const attribute = /^(?:getattr|readattr|setattr)\[([A-Za-z_][A-Za-z_0-9]*)\]$/.exec(parts[2]);
      if (attribute && parts.length === 3) return {nodes,multiple,attribute:attribute[1]};
      if (parts.length === 4 && parts[2] === 'style') return {nodes,multiple,style:parts[3]};
      const property = {content:'textContent',html:'innerHTML',value:'value',getInput:'value',getOption:'value'}[parts[2]];
      if (parts.length !== 3 || !property) throw new Error(`Unsupported Browser property: ${path}`);
      if(property === 'value' && nodes.some(node => !('value' in node))) throw new Error('Element has no input value');
      return {nodes,multiple,property};
    };
    const requests = new Map(), sockets = new Map();
    let requestSerial=0;
    const dispatch = (target,args=[]) => {
      if(!target || stopped)return;
      let t=0,a=0;
      try {
        hostError=undefined;t=allocate(target);a=allocate(JSON.stringify(args));
        if(!instance.exports.bzvm_event(t,a))throw hostError||new Error(read(instance.exports.bzvm_error()));
      }catch(error){report(error);}
      finally{if(t)instance.exports.free(t);if(a)instance.exports.free(a);}
    };
    const textArgument=(value,label)=>{if(typeof value!=='string')throw new Error(label+' must be text');return value;};
    const network = (operation,args) => {
      if(stopped)throw new Error('Page has stopped');
      if(operation==='HTTP.request') {
        const options=Object.fromEntries(Array.from({length:args.length/2},(_,i)=>[args[i*2],args[i*2+1]]));
        const name=options.name??'Request'+(++requestSerial),url=new URL(textArgument(options.url,'URL'),location.href);
        textArgument(name,'Request name');
        if(!['http:','https:'].includes(url.protocol))throw new Error('HTTP URL must use http:// or https://');
        if(url.username||url.password)throw new Error('Credentials in URLs are not supported');
        if(requests.get(name)?.active)throw new Error('HTTP request name is already active: '+name);
        if([...requests.values()].filter(r=>r.active).length>=32)throw new Error('At most 32 active HTTP requests');
        const method=(options.method||'GET').toUpperCase(),timeout=options.timeout??30000;
        if(!['GET','POST','PUT','PATCH','DELETE','HEAD','OPTIONS'].includes(method))throw new Error('Unsupported HTTP method');
        if(!Number.isFinite(timeout)||timeout<1||timeout>300000)throw new Error('HTTP timeout must be 1..300000 ms');
        const headers=typeof options.headers==='string'?JSON.parse(options.headers):options.headers||{};
        if(!headers || Array.isArray(headers)||typeof headers!=='object'||Object.values(headers).some(v=>typeof v!=='string'))
          throw new Error('HTTP headers must be a JSON object of text values');
        const body=options.body;
        if(body!==undefined && typeof body!=='string')throw new Error('HTTP body must be text');
        if(body && encoder.encode(body).length>1048576)throw new Error('HTTP body exceeds 1 MiB');
        if(body && (method==='GET'||method==='HEAD'))throw new Error('GET/HEAD requests cannot have a body');
        const controller=new AbortController(),request={active:true,controller,headers:'{}',reason:null};
        requests.set(name,request);
        const timer=setTimeout(()=>{request.reason='Request timed out';controller.abort();},timeout);
        fetch(url.href,{method,headers,body:body||undefined,signal:controller.signal,credentials:'omit'}).then(async response=>{
          const size=Number(response.headers.get('content-length'));
          if(size>1048576)throw new Error('HTTP response exceeds 1 MiB');
          let result='';
          if(response.body?.getReader) {
            const reader=response.body.getReader(),chunks=[];let total=0;
            try {for(;;){const {done,value}=await reader.read();if(done)break;total+=value.byteLength;
              if(total>1048576){await reader.cancel();throw new Error('HTTP response exceeds 1 MiB');}chunks.push(value);}}
            finally{reader.releaseLock();}
            const all=new Uint8Array(total);let offset=0;for(const chunk of chunks){all.set(chunk,offset);offset+=chunk.byteLength;}
            result=decoder.decode(all);
          } else {result=await response.text();if(encoder.encode(result).length>1048576)throw new Error('HTTP response exceeds 1 MiB');}
          if(!request.active)return;
          request.headers=JSON.stringify(Object.fromEntries(response.headers.entries()));
          request.active=false;clearTimeout(timer);dispatch(options.onResponse,[response.status,result]);
        }).catch(error=>{
          if(!request.active)return;request.active=false;clearTimeout(timer);
          dispatch(options.onError,[request.reason||String(error.message||error)]);
        });
        request.timer=timer;request.error=options.onError;
        while(requests.size>128){const old=[...requests].find(([,r])=>!r.active);if(!old)break;requests.delete(old[0]);}
        return null;
      }
      if(operation==='HTTP.requestCancel') {
        const request=requests.get(textArgument(args[0],'Request name'));
        if(!request)throw new Error('Unknown HTTP request');
        if(request.active){request.reason='Request cancelled';request.controller.abort();}
        return null;
      }
      if(operation==='HTTP.responseHeaders') {
        const request=requests.get(textArgument(args[0],'Request name'));
        if(!request)throw new Error('Unknown HTTP request');return request.headers;
      }
      const method=operation.split('.')[1];
      if(method==='connect') {
        const url=new URL(textArgument(args[0],'Socket URL')),name=textArgument(args[1],'Socket name');
        if(!['ws:','wss:'].includes(url.protocol))throw new Error('Websocket requires ws:// or wss://');
        if(url.username||url.password)throw new Error('Credentials in socket URLs are not supported');
        if(sockets.has(name)&&sockets.get(name).socket.readyState!==3)throw new Error('Socket name is already in use');
        if([...sockets.values()].filter(s=>s.socket.readyState!==3).length>=32)throw new Error('At most 32 sockets');
        const socket=new WebSocket(url.href),entry={socket,callbacks:{},state:'connecting'};
        sockets.set(name,entry);
        socket.addEventListener('open',()=>{entry.state='open';dispatch(entry.callbacks.connected);});
        socket.addEventListener('message',event=>{
          if(typeof event.data!=='string'){dispatch(entry.callbacks.error,['Binary WebSocket messages are not supported']);return;}
          if(encoder.encode(event.data).length>1048576){dispatch(entry.callbacks.error,['Socket message exceeds 1 MiB']);socket.close(1009);return;}
          dispatch(entry.callbacks.callback,[event.data]);
        });
        socket.addEventListener('error',()=>dispatch(entry.callbacks.error,['WebSocket connection failed']));
        socket.addEventListener('close',event=>{entry.state='closed';dispatch(entry.callbacks.closed,[event.code,event.reason]);});
        return null;
      }
      const entry=sockets.get(textArgument(args[0],'Socket name'));
      if(!entry)throw new Error('Unknown socket name');
      if(['callback','connected','error','closed'].includes(method)){
        entry.callbacks[method]=textArgument(args[1],'Callback target');return null;
      }
      if(method==='state')return entry.state;
      if(method==='send'){
        const message=textArgument(args[1],'Socket message');
        if(entry.socket.readyState!==1)throw new Error('Socket must be open before sending');
        if(encoder.encode(message).length>1048576||entry.socket.bufferedAmount>1048576)throw new Error('Socket send buffer/message exceeds 1 MiB');
        entry.socket.send(message);return null;
      }
      if(method==='close'){entry.state='closing';entry.socket.close(1000);return null;}
      throw new Error('Unknown network operation');
    };
    const imports = {bzvm: {
      network: guard((operation,argumentsPointer,output,capacity) => {
        const result=JSON.stringify(network(read(operation),JSON.parse(read(argumentsPointer))));
        const bytes=encoder.encode(result);
        if(bytes.length>=capacity)throw new Error("Network result exceeds buffer");
        memory().set(bytes,output);memory()[output+bytes.length]=0;return 1;
      },0),
      now: () => globalThis.performance?.now?.() ?? Date.now(),
      input_session: (task, begin) => {
        if(begin) inputSessions.set(task, {base:new Map([...keys].map(([name,key]) => [name,{pressed:key.pressed,released:key.released}])),sites:new Map()});
        else inputSessions.delete(task);
      },
      wall_time: () => Date.now(),
      local_time: (timestamp, field) => {
        const date = new Date(timestamp);
        return [date.getFullYear(), date.getMonth() + 1, date.getDate(),
          date.getHours(), date.getMinutes(), date.getSeconds(), date.getDay()][field] ?? NaN;
      },
      convert_local_time: (year, month, day, hour, minute, second) => {
        const utc = new Date(0);
        utc.setUTCFullYear(year, month - 1, day);
        utc.setUTCHours(hour, minute, second, 0);
        const base = utc.getTime(), offsets = new Set(), matches = new Set();
        // Gather both sides of nearby DST/offset transitions, including
        // half-hour transitions. Never normalize a missing local time.
        for (let h = -48; h <= 48; h++) offsets.add(new Date(base + h * 3600000).getTimezoneOffset());
        for (const offset of offsets) {
          const candidate = base + offset * 60000, date = new Date(candidate);
          if (date.getFullYear() === year && date.getMonth() === month - 1 &&
              date.getDate() === day && date.getHours() === hour &&
              date.getMinutes() === minute && date.getSeconds() === second) matches.add(candidate);
        }
        return matches.size === 1 ? [...matches][0] : NaN;
      },
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
      read: guard((pathPointer, output, capacity, task, sitePointer) => {
        const path = read(pathPointer); let value;
        if(path.startsWith('JSON.file:') || path.startsWith('File.exists:')) {
          const exists=path.startsWith('File.exists:');
          const entry=jsonFile(path.slice(exists ? 12 : 10));
          if(exists && entry.status===404){const bytes=encoder.encode('dead');memory().set(bytes,output);memory()[output+bytes.length]=0;return bytes.length;}
          if(entry.error)throw entry.error;
          if(entry.source===null)return -2;
          const bytes=encoder.encode(exists ? 'alive' : entry.source);if(bytes.length>=capacity)throw new Error('JSON file exceeds buffer');
          memory().set(bytes,output);memory()[output+bytes.length]=0;return bytes.length;
        }
        if (path === 'Browser.platform') value = platform;
        else if (/^Browser\.pointer\.(xpos|ypos)$/.test(path)) value = pointer[path.split('.')[2]];
        else if (/^Browser\.input\.[A-Za-z_0-9]+\.(held|pressed|released)$/.test(path)) {
          const [, , name, state] = path.split('.'), key=keys.get(name.toLowerCase());
          value=false;
          if(key && state === 'held') value=key.held;
          else if(key) {
            const session=inputSessions.get(task), site=(sitePointer ? read(sitePointer) : path)+':'+path;
            if(session) {
              const seen=session.sites.get(site) ?? session.base.get(name.toLowerCase())?.[state] ?? 0;
              value=key[state]>seen;session.sites.set(site,value ? seen+1 : seen);
            }
          }
        } else {
          const {nodes, property, style, attribute, multiple}=resolve(path);
          const values=nodes.map(node => attribute ? node.getAttribute(attribute) : style ? node.style[style] : node[property]);
          value=multiple ? values.map(v=>String(v??'')) : values[0]??null;
        }
        const bytes = encoder.encode('\x1e'+JSON.stringify(value));
        if (bytes.length >= capacity) throw new Error("Browser value exceeds 64 KiB prototype limit");
        memory().set(bytes, output); memory()[output + bytes.length] = 0; return bytes.length;
      }, -1),
      write: guard((pathPointer, valuePointer) => {
        const {nodes, property, style, attribute} = resolve(read(pathPointer));
        const value = read(valuePointer);
        for(const node of nodes) if (style) {
          if (!(style in node.style)) throw new Error(`Unknown CSS property '${style}'`);
          node.style[style] = value;
        } else if(attribute) node.setAttribute(attribute,value); else node[property] = value;
        return 1;
      }, 0),
      bind: guard((idPointer, targetPointer) => {
        const id = read(idPointer), target = read(targetPointer), [selector, kind='clicked'] = id.split('|');
        const nodes = elements(selector), eventName={clicked:'click',changed:'change',inputted:'input',submitted:'submit'}[kind];
        if(!eventName)throw new Error('Unknown browser event');
        if (bindings.has(id)) bindings.get(id)();
        const callback = event => {
          if(kind==='submitted')event.preventDefault();
          let pointer;
          try {
            hostError = undefined; pointer = allocate(target);
            if (!instance.exports.bzvm_call(pointer)) throw hostError || new Error(read(instance.exports.bzvm_error()));
          } catch (error) { report(error); }
          finally { if (pointer) instance.exports.free(pointer); }
        };
        for(const node of nodes)node.addEventListener(eventName, callback);
        bindings.set(id, () => nodes.forEach(node=>node.removeEventListener(eventName, callback))); return 1;
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
    // Revalidate the paired binary so an updated loader cannot silently run an
    // older cached WASM module with a different export/API surface.
    const response = await fetch(wasmURL, {cache:'no-cache'});
    if (!response.ok) throw new Error(`bZVM download failed: HTTP ${response.status}`);
    const result = await WebAssembly.instantiate(await response.arrayBuffer(), imports);
    instance = result.instance;
    if (instance.exports._initialize) instance.exports._initialize();
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
      exposeSource(module);
      for (let i = 0; i < instance.exports.bzvm_import_count(moduleIndex); i++) {
        const path = read(instance.exports.bzvm_import(moduleIndex,i));
        if (['ZSharp.Browser','ZSharp.HTTP','ZSharp.Websocket','ZSharp.JSON'].includes(path)) continue;
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
    for(let module=0;module<pending.length;module++) {
      for(let index=0;;index++) {
        const path=read(instance.exports.bzvm_json_path(module,index));if(!path)break;
        const entry=jsonFile(path);await entry.promise;if(entry.error)throw entry.error;
      }
    }
    if (!instance.exports.bzvm_run()) throw hostError || new Error(read(instance.exports.bzvm_error()));
    window.addEventListener("pagehide",()=>{
      stopped = true;
      for(const entry of jsonFiles.values())entry.controller.abort();
      jsonFiles.clear();
      for(const request of requests.values()){clearTimeout(request.timer);request.active=false;request.controller.abort();}
      for(const entry of sockets.values())try{entry.socket.close(1000);}catch(_){}
      requests.clear();sockets.clear();
      for(const timer of timers.values())clearTimeout(timer);
      timers.clear(); instance.exports.bzvm_cancel();
      for(const dispose of bindings.values())dispose(); bindings.clear();
      window.removeEventListener?.('keydown',keydown);window.removeEventListener?.('keyup',keyup);
      window.removeEventListener?.('blur',blur);window.removeEventListener?.('pointermove',move);
      inputSessions.clear();
    },{once:true});
    window.dispatchEvent(new CustomEvent("bzvm:ready", {detail: {project}}));
  }
  if(document.readyState==="loading")document.addEventListener("DOMContentLoaded",()=>boot().catch(report),{once:true});
  else boot().catch(report);
})();
