#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "interop_script.h"
#include "interop_script_lua.h"
#include "quickjs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION ScriptMutex;
static SRWLOCK script_cache_mutex = SRWLOCK_INIT;
#define SCRIPT_CACHE_LOCK() AcquireSRWLockExclusive(&script_cache_mutex)
#define SCRIPT_CACHE_UNLOCK() ReleaseSRWLockExclusive(&script_cache_mutex)
static void script_mutex_init(ScriptMutex *mutex) { InitializeCriticalSection(mutex); }
static void script_mutex_enter(ScriptMutex *mutex) { EnterCriticalSection(mutex); }
static void script_mutex_leave(ScriptMutex *mutex) { LeaveCriticalSection(mutex); }
static void script_mutex_destroy(ScriptMutex *mutex) { DeleteCriticalSection(mutex); }
static uint64_t script_clock(void) { return GetTickCount64(); }
#else
#include <pthread.h>
typedef pthread_mutex_t ScriptMutex;
static pthread_mutex_t script_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
#define SCRIPT_CACHE_LOCK() pthread_mutex_lock(&script_cache_mutex)
#define SCRIPT_CACHE_UNLOCK() pthread_mutex_unlock(&script_cache_mutex)
static void script_mutex_init(ScriptMutex *mutex) {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}
static void script_mutex_enter(ScriptMutex *mutex) { pthread_mutex_lock(mutex); }
static void script_mutex_leave(ScriptMutex *mutex) { pthread_mutex_unlock(mutex); }
static void script_mutex_destroy(ScriptMutex *mutex) { pthread_mutex_destroy(mutex); }
static uint64_t script_clock(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}
#endif

#define SCRIPT_MAX_FRAME (1024u * 1024u)

typedef struct ScriptModule {
    char *path;
    char *language;
    ScriptMutex mutex;
    int busy;
    int initialized;
    uint64_t deadline;
    JSRuntime *runtime;
    JSContext *context;
    JSValue bridge;
    ZSharpInteropHostDispatch host;
    void *host_context;
    struct ScriptModule *next;
} ScriptModule;

struct ZSharpScriptInterop {
    ScriptModule *modules;
    ZSharpLuaInterop *lua;
};

/* Keep transport mechanics separate from user modules. Tagged exact numbers
 * only become JS Numbers when they fit the native safe decimal range. */
static const char script_javascript_bridge[] =
"(function(nativeHost){\n"
"'use strict';\n"
"const refs=new Map(), identities=new WeakMap(), records=new WeakSet();let serial=1;\n"
"const decimal=/^-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$/;\n"
"class ExactNumber{constructor(text){if(!decimal.test(String(text)))throw Error('Invalid exact number');this.text=String(text);Object.freeze(this)}toString(){return this.text}valueOf(){throw Error('Exact number requires an explicit Number(value.toString()) conversion before native JS arithmetic')}}\n"
"class HostObject{constructor(ref,type){this.ref=ref;this.type=type;Object.freeze(this)}get(member){return host('host.get','',this.ref,member,[])}set(member,value){return host('host.set','',this.ref,member,[value])}invoke(member,...args){return host('host.invoke','',this.ref,member,args)}}\n"
"function count(text){return text.split(/[eE]/)[0].replace(/[-.]/g,'').replace(/^0+/,'').length}\n"
"function decode(value,depth=0){if(depth>128)throw Error('Bridge nesting limit exceeded');if(value===null||typeof value!=='object')return value;if(Array.isArray(value))return value.map(v=>decode(v,depth+1));if('$number'in value){let text=value.$number;if(typeof text!=='string'||!decimal.test(text))throw Error('Invalid $number decimal text');let n=Number(text);return Number.isFinite(n)&&count(text)<=15&&(n!==0||count(text)===0)&&(!Number.isInteger(n)||Number.isSafeInteger(n))?n:new ExactNumber(text)}if('$ref'in value){if(!refs.has(value.$ref))throw Error('JavaScript object reference is invalid or has been released');return refs.get(value.$ref)}if('$zref'in value)return new HostObject(value.$zref,value.$type||'Z# object');if('$record'in value){let result=Object.create(null);for(let [k,v]of Object.entries(value.$record))result[k]=decode(v,depth+1);records.add(result);return result}throw Error('Unknown JavaScript bridge value tag')}\n"
"function encode(value,depth=0){if(depth>128)throw Error('Bridge nesting limit exceeded');if(value===undefined||value===null)return null;if(typeof value==='string'||typeof value==='boolean')return value;if(typeof value==='number'){if(!Number.isFinite(value))throw Error('Non-finite JavaScript number');return {$number:String(value)}}if(typeof value==='bigint')return {$number:String(value)};if(value instanceof ExactNumber)return {$number:value.text};if(value instanceof HostObject)return {$zref:value.ref,$type:value.type};if(Array.isArray(value))return value.map(v=>encode(v,depth+1));if(records.has(value)){let result=Object.create(null);for(let [k,v]of Object.entries(value))result[k]=encode(v,depth+1);return {$record:result}}if(typeof value!=='object'&&typeof value!=='function')throw Error('Unsupported JavaScript bridge value');let id=identities.get(value);if(!id){id=String(serial++);identities.set(value,id);refs.set(id,value)}return {$ref:id,$type:typeof value==='function'?(value.name||'Function'):(value.constructor?.name||'Object')}}\n"
"function host(op,target,ref,member,args){return decode(nativeHost(op,target,ref,member,JSON.stringify(args.map(v=>encode(v)))))}\n"
"globalThis.ZSharp=Object.freeze({call:(target,...args)=>host('host.call',String(target),'','',args),get:(object,member)=>object.get(member),set:(object,member,value)=>object.set(member,value),invoke:(object,member,...args)=>object.invoke(member,...args),number:text=>new ExactNumber(text),record:value=>{if(!value||typeof value!=='object'||Array.isArray(value))throw Error('ZSharp.record expects an object');records.add(value);return value}});\n"
"let exports=null,legacy=false;\n"
"function lookup(target){if(!/^[A-Za-z_$][\\w$]*(?:\\.[A-Za-z_$][\\w$]*)*$/.test(target))throw Error('Invalid JavaScript exported member target');let parts=target.split('.'),owner=exports,value=exports;for(let i=0;i<parts.length;i++){owner=value;if(i===0&&legacy)value=(0,eval)(parts[i]);else if(i===0&&!(parts[i]in value)&&value.default?.name===parts[i])value=value.default;else value=value[parts[i]];if(value===undefined)throw Error('JavaScript exported member not found: '+target)}return{value,owner}}\n"
"function dispatch(op,target,member,ref,raw){let args=decode(raw),object;if(!Array.isArray(args))throw Error('Bridge arguments must be an array');if(ref){if(!refs.has(ref))throw Error('JavaScript object reference is invalid or has been released');object=refs.get(ref)}if(op==='release'){if(!ref)throw Error('release requires a live object reference');refs.delete(ref);identities.delete(object);return null}if(op==='new'){let ctor=lookup(target).value;if(typeof ctor!=='function')throw Error('Export is not a JavaScript constructor');return encode(Reflect.construct(ctor,args))}if(op==='get'||op==='set'){if(['__proto__','prototype','constructor'].includes(member))throw Error('JavaScript internal prototype members are not exposed');if(!ref){let selected=lookup(target);object=selected.value}if(object===null||object===undefined)throw Error('Object member requires a live object');if(op==='get'){if(args.length)throw Error('get takes no arguments');return encode(object[member])}if(args.length!==1)throw Error('set requires one value');object[member]=args[0];return null}if(op!=='call'&&op!=='invoke')throw Error('Unknown JavaScript bridge operation: '+op);let fn,owner;if(ref){owner=object;fn=object[member]}else{let selected=lookup(target);owner=selected.owner;fn=selected.value}if(typeof fn!=='function')throw Error('Exported JavaScript member is not callable');return fn.apply(owner,args)}\n"
"return{install:(value,isLegacy)=>{exports=value;legacy=isLegacy},dispatch,encode};\n"
"})(__zsharpNativeHost)";

static int script_fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return 0;
}

static char *script_copy(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (copy) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

static int script_interrupt(JSRuntime *runtime, void *opaque) {
    ScriptModule *module = (ScriptModule *)opaque;
    (void)runtime;
    return module->busy && script_clock() >= module->deadline;
}

static int script_exception(ScriptModule *module, char *error, size_t size) {
    JSValue exception = JS_GetException(module->context);
    JSValue stack = JS_IsObject(exception) ? JS_GetPropertyStr(module->context, exception, "stack") : JS_UNDEFINED;
    const char *message = JS_ToCString(module->context, exception);
    const char *trace = JS_IsString(stack) ? JS_ToCString(module->context, stack) : NULL;
    if (error && size) snprintf(error, size, "JavaScript interop failed: %s%s%s",
        message ? message : "execution exceeded 5 seconds or memory limit",
        trace ? "\n" : "", trace ? trace : "");
    if (trace) JS_FreeCString(module->context, trace);
    if (message) JS_FreeCString(module->context, message);
    JS_FreeValue(module->context, stack);
    JS_FreeValue(module->context, exception);
    return 0;
}

static JSValue script_host(JSContext *context, JSValueConst this_value,
                           int count, JSValueConst *arguments) {
    ScriptModule *module = (ScriptModule *)JS_GetContextOpaque(context);
    const char *values[5] = {NULL, NULL, NULL, NULL, NULL};
    char *response = NULL;
    char error[2048] = {0};
    JSValue result = JS_EXCEPTION;
    uint64_t started;
    int index;
    (void)this_value;
    if (!module || !module->busy || !module->host)
        return JS_ThrowInternalError(context, "Z# callbacks are only available during an active foreign call");
    if (count != 5) return JS_ThrowTypeError(context, "Invalid Z# callback arguments");
    for (index = 0; index < 5; index++) {
        size_t length;
        values[index] = JS_ToCStringLen(context, &length, arguments[index]);
        if (!values[index]) goto cleanup;
        if (length > SCRIPT_MAX_FRAME || memchr(values[index], 0, length)) {
            JS_ThrowRangeError(context, "Z# callback frame is invalid or exceeds 1 MiB");
            goto cleanup;
        }
    }
    started = script_clock();
    if (!module->host(module->host_context, values[0], values[1], values[3],
            values[2], values[4], &response, error, sizeof(error))) {
        module->deadline += script_clock() - started;
        JS_ThrowInternalError(context, "Z# callback failed: %s", error[0] ? error : "host operation failed");
        goto cleanup;
    }
    module->deadline += script_clock() - started;
    if (!response || strlen(response) > SCRIPT_MAX_FRAME) {
        JS_ThrowRangeError(context, "Z# callback returned an invalid or oversized frame");
        goto cleanup;
    }
    result = JS_ParseJSON(context, response, strlen(response), "Z# callback result");
cleanup:
    for (index = 0; index < 5; index++) if (values[index]) JS_FreeCString(context, values[index]);
    free(response);
    return result;
}

static JSValue script_console(JSContext *context, JSValueConst this_value,
                              int count, JSValueConst *arguments) {
    int index;
    (void)this_value;
    for (index = 0; index < count; index++) {
        const char *text = JS_ToCString(context, arguments[index]);
        if (!text) return JS_EXCEPTION;
        if (index) fputc(' ', stdout);
        fputs(text, stdout);
        JS_FreeCString(context, text);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return JS_UNDEFINED;
}

static int script_resolve_promise(ScriptModule *module, JSValue *value,
                                  char *error, size_t size) {
    JSContext *context = module->context;
    if (!JS_IsPromise(*value)) return 1;
    while (JS_PromiseState(context, *value) == JS_PROMISE_PENDING) {
        JSContext *job_context = NULL;
        int jobs;
        if (script_clock() >= module->deadline)
            return script_fail(error, size, "JavaScript execution exceeded the 5 second limit");
        jobs = JS_ExecutePendingJob(module->runtime, &job_context);
        if (jobs < 0) return script_exception(module, error, size);
        if (jobs == 0) return script_fail(error, size,
            "JavaScript promise cannot resolve without an external event loop");
    }
    if (JS_PromiseState(context, *value) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(context, *value);
        const char *message = JS_ToCString(context, reason);
        if (error && size) snprintf(error, size, "JavaScript promise rejected: %s", message ? message : "unknown error");
        if (message) JS_FreeCString(context, message);
        JS_FreeValue(context, reason);
        return 0;
    }
    {
        JSValue resolved = JS_PromiseResult(context, *value);
        JS_FreeValue(context, *value);
        *value = resolved;
    }
    return 1;
}

static int script_load_javascript(ScriptModule *module, char *error, size_t size) {
    FILE *file = NULL;
    long length;
    char *source = NULL;
    JSContext *context;
    JSValue global = JS_UNDEFINED, console = JS_UNDEFINED;
    JSValue compiled = JS_UNDEFINED, namespace_value = JS_UNDEFINED;
    JSValue evaluated = JS_UNDEFINED, install = JS_UNDEFINED;
    JSModuleDef *module_definition = NULL;
    int legacy = 0, ok = 0;
    module->runtime = JS_NewRuntime();
    if (!module->runtime) return script_fail(error, size, "Could not create JavaScript runtime");
    JS_SetMemoryLimit(module->runtime, 64u * 1024u * 1024u);
    JS_SetMaxStackSize(module->runtime, 1024u * 1024u);
    JS_SetInterruptHandler(module->runtime, script_interrupt, module);
    module->context = JS_NewContext(module->runtime);
    context = module->context;
    if (!context) return script_fail(error, size, "Could not create JavaScript context");
    JS_SetContextOpaque(context, module);
    global = JS_GetGlobalObject(context);
    if (JS_SetPropertyStr(context, global, "__zsharpNativeHost",
            JS_NewCFunction(context, script_host, "ZSharp callback", 5)) < 0) goto exception;
    console = JS_NewObject(context);
    if (JS_IsException(console)) goto exception;
    JS_SetPropertyStr(context, console, "log", JS_NewCFunction(context, script_console, "log", 1));
    JS_SetPropertyStr(context, console, "warn", JS_NewCFunction(context, script_console, "warn", 1));
    JS_SetPropertyStr(context, console, "error", JS_NewCFunction(context, script_console, "error", 1));
    if (JS_SetPropertyStr(context, global, "console", JS_DupValue(context, console)) < 0) goto exception;
    module->bridge = JS_Eval(context, script_javascript_bridge, sizeof(script_javascript_bridge) - 1,
                             "ZSharp JavaScript bridge", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(module->bridge)) goto exception;
    file = fopen(module->path, "rb");
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0
        || length > 8 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        script_fail(error, size, "Could not read JavaScript module (maximum source size is 8 MiB)");
        goto cleanup;
    }
    source = (char *)malloc((size_t)length + 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length) {
        script_fail(error, size, "Could not read JavaScript module source");
        goto cleanup;
    }
    source[length] = '\0';
    /* Compile without running first. Plain legacy scripts use their global
     * scope; real ES modules retain their actual export namespace. Initializers
     * are never executed twice merely to detect which form a module uses. */
    compiled = JS_Eval(context, source, (size_t)length, module->path,
                        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    legacy = !JS_IsException(compiled);
    if (!legacy) {
        JSValue initial_error = JS_GetException(context);
        JS_FreeValue(context, initial_error);
        compiled = JS_Eval(context, source, (size_t)length, module->path,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) goto exception;
        module_definition = (JSModuleDef *)JS_VALUE_GET_PTR(compiled);
    }
    evaluated = JS_EvalFunction(context, compiled);
    compiled = JS_UNDEFINED;
    if (JS_IsException(evaluated)) goto exception;
    if (!script_resolve_promise(module, &evaluated, error, size)) goto cleanup;
    if (legacy) {
        namespace_value = JS_DupValue(context, global);
    } else {
        namespace_value = JS_GetModuleNamespace(context, module_definition);
        if (JS_IsException(namespace_value)) goto exception;
    }
    install = JS_GetPropertyStr(context, module->bridge, "install");
    if (JS_IsException(install)) goto exception;
    {
        JSValue arguments[2] = {namespace_value, JS_NewBool(context, legacy)};
        JSValue installed = JS_Call(context, install, module->bridge, 2, arguments);
        if (JS_IsException(installed)) { JS_FreeValue(context, installed); goto exception; }
        JS_FreeValue(context, installed);
    }
    ok = 1;
    goto cleanup;
exception:
    script_exception(module, error, size);
cleanup:
    if (file) fclose(file);
    free(source);
    JS_FreeValue(context, install);
    JS_FreeValue(context, evaluated);
    JS_FreeValue(context, compiled);
    JS_FreeValue(context, namespace_value);
    JS_FreeValue(context, console);
    JS_FreeValue(context, global);
    return ok;
}

static int script_javascript_request(ScriptModule *module, const char *operation,
                                     const char *target, const char *member,
                                     const char *handle, const char *arguments_json,
                                     char **result_json, char *error, size_t size) {
    JSContext *context = module->context;
    JSValue dispatch = JS_UNDEFINED, returned = JS_UNDEFINED, encoded = JS_UNDEFINED;
    JSValue stringify = JS_UNDEFINED, encoder = JS_UNDEFINED;
    JSValue arguments[5];
    const char *text = NULL;
    size_t length = 0;
    int index, ok = 0;
    for (index = 0; index < 5; index++) arguments[index] = JS_UNDEFINED;
    arguments[0] = JS_NewString(context, operation);
    arguments[1] = JS_NewString(context, target ? target : "");
    arguments[2] = JS_NewString(context, member ? member : "");
    arguments[3] = JS_NewString(context, handle ? handle : "");
    arguments[4] = JS_ParseJSON(context, arguments_json, strlen(arguments_json), "Z# bridge arguments");
    for (index = 0; index < 5; index++) if (JS_IsException(arguments[index])) goto exception;
    dispatch = JS_GetPropertyStr(context, module->bridge, "dispatch");
    returned = JS_Call(context, dispatch, module->bridge, 5, arguments);
    if (JS_IsException(returned)) goto exception;
    if (!script_resolve_promise(module, &returned, error, size)) goto cleanup;
    /* call/invoke may return promises; only encode after they resolve. Other
     * operations already return wire values from the bridge dispatcher. */
    if (strcmp(operation, "call") == 0 || strcmp(operation, "invoke") == 0) {
        encoder = JS_GetPropertyStr(context, module->bridge, "encode");
        encoded = JS_Call(context, encoder, module->bridge, 1, &returned);
    } else {
        encoded = JS_DupValue(context, returned);
    }
    if (JS_IsException(encoded)) goto exception;
    stringify = JS_JSONStringify(context, encoded, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(stringify)) goto exception;
    text = JS_ToCStringLen(context, &length, stringify);
    if (!text) goto exception;
    if (length > SCRIPT_MAX_FRAME || memchr(text, 0, length)) {
        script_fail(error, size, "JavaScript bridge result exceeds the 1 MiB limit");
        goto cleanup;
    }
    *result_json = script_copy(text, length);
    if (!*result_json) { script_fail(error, size, "Out of memory receiving JavaScript result"); goto cleanup; }
    ok = 1;
    goto cleanup;
exception:
    script_exception(module, error, size);
cleanup:
    if (text) JS_FreeCString(context, text);
    for (index = 0; index < 5; index++) JS_FreeValue(context, arguments[index]);
    JS_FreeValue(context, dispatch);
    JS_FreeValue(context, returned);
    JS_FreeValue(context, encoded);
    JS_FreeValue(context, stringify);
    JS_FreeValue(context, encoder);
    return ok;
}

static void script_dispose_runtime(ScriptModule *module) {
    if (module->context) {
        JS_FreeValue(module->context, module->bridge);
        module->bridge = JS_UNDEFINED;
        JS_FreeContext(module->context);
        module->context = NULL;
    }
    if (module->runtime) {
        JS_FreeRuntime(module->runtime);
        module->runtime = NULL;
    }
    module->initialized = 0;
}

static ScriptModule *script_module(ZSharpScriptInterop **cache, const char *language,
                                    const char *path, char *error, size_t size) {
    ScriptModule *module = NULL;
    SCRIPT_CACHE_LOCK();
    if (!*cache) *cache = (ZSharpScriptInterop *)calloc(1, sizeof(**cache));
    if (!*cache) goto cleanup;
    for (module = (*cache)->modules; module; module = module->next)
        if (strcmp(module->path, path) == 0 && strcmp(module->language, language) == 0) break;
    if (!module) {
        module = (ScriptModule *)calloc(1, sizeof(*module));
        if (!module) goto cleanup;
        module->path = script_copy(path, strlen(path));
        module->language = script_copy(language, strlen(language));
        module->bridge = JS_UNDEFINED;
        if (!module->path || !module->language) {
            free(module->path); free(module->language); free(module); module = NULL;
            goto cleanup;
        }
        script_mutex_init(&module->mutex);
        module->next = (*cache)->modules;
        (*cache)->modules = module;
    }
cleanup:
    SCRIPT_CACHE_UNLOCK();
    if (!module) script_fail(error, size, "Out of memory creating script module cache");
    return module;
}

int zsharp_script_interop_request(
    ZSharpScriptInterop **cache, const char *language, const char *module_path,
    const char *operation, const char *target, const char *member,
    const char *handle, const char *arguments_json,
    ZSharpInteropHostDispatch host_dispatch, void *host_context,
    char **result_json, char *error, size_t error_size) {
    ScriptModule *module;
    int ok = 0;
    if (result_json) *result_json = NULL;
    if (!cache || !language || !module_path || !operation || !arguments_json || !result_json)
        return script_fail(error, error_size, "Invalid script interop request");
    if (strcmp(language, "lua") == 0) {
        ZSharpScriptInterop *owner;
        SCRIPT_CACHE_LOCK();
        if (!*cache) *cache = (ZSharpScriptInterop *)calloc(1, sizeof(**cache));
        owner = *cache;
        SCRIPT_CACHE_UNLOCK();
        if (!owner) return script_fail(error, error_size, "Out of memory creating script module cache");
        return zsharp_lua_interop_request(&owner->lua, module_path, operation, target,
            member, handle, arguments_json, host_dispatch, host_context,
            result_json, error, error_size);
    }
    if (strcmp(language, "js") != 0)
        return script_fail(error, error_size, "Unsupported persistent script language");
    if (strlen(arguments_json) > SCRIPT_MAX_FRAME)
        return script_fail(error, error_size, "Script bridge arguments exceed the 1 MiB limit");
    module = script_module(cache, language, module_path, error, error_size);
    if (!module) return 0;
    script_mutex_enter(&module->mutex);
    if (module->busy) {
        script_mutex_leave(&module->mutex);
        return script_fail(error, error_size, "Reentrant call to the active script module is not supported");
    }
    module->busy = 1;
    module->deadline = script_clock() + 5000;
    module->host = host_dispatch;
    module->host_context = host_context;
    if (!module->initialized) {
        if (!script_load_javascript(module, error, error_size)) {
            script_dispose_runtime(module);
            goto cleanup;
        }
        module->initialized = 1;
    }
    JS_UpdateStackTop(module->runtime);
    ok = script_javascript_request(module, operation, target, member, handle,
                                   arguments_json, result_json, error, error_size);
cleanup:
    module->host = NULL;
    module->host_context = NULL;
    module->busy = 0;
    script_mutex_leave(&module->mutex);
    return ok;
}

void zsharp_script_interop_free(ZSharpScriptInterop *cache) {
    ScriptModule *module;
    if (!cache) return;
    zsharp_lua_interop_free(cache->lua);
    module = cache->modules;
    while (module) {
        ScriptModule *next = module->next;
        script_dispose_runtime(module);
        script_mutex_destroy(&module->mutex);
        free(module->path);
        free(module->language);
        free(module);
        module = next;
    }
    free(cache);
}
