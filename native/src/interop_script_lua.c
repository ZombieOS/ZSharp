#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "interop_script_lua.h"

#include "quickjs.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION LuaMutex;
static SRWLOCK lua_cache_lock = SRWLOCK_INIT;
#define CACHE_LOCK() AcquireSRWLockExclusive(&lua_cache_lock)
#define CACHE_UNLOCK() ReleaseSRWLockExclusive(&lua_cache_lock)
static void mutex_init(LuaMutex *mutex) { InitializeCriticalSection(mutex); }
static void mutex_lock(LuaMutex *mutex) { EnterCriticalSection(mutex); }
static void mutex_unlock(LuaMutex *mutex) { LeaveCriticalSection(mutex); }
static void mutex_free(LuaMutex *mutex) { DeleteCriticalSection(mutex); }
static uint64_t now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t LuaMutex;
static pthread_mutex_t lua_cache_lock = PTHREAD_MUTEX_INITIALIZER;
#define CACHE_LOCK() pthread_mutex_lock(&lua_cache_lock)
#define CACHE_UNLOCK() pthread_mutex_unlock(&lua_cache_lock)
static void mutex_init(LuaMutex *mutex) {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}
static void mutex_lock(LuaMutex *mutex) { pthread_mutex_lock(mutex); }
static void mutex_unlock(LuaMutex *mutex) { pthread_mutex_unlock(mutex); }
static void mutex_free(LuaMutex *mutex) { pthread_mutex_destroy(mutex); }
static uint64_t now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}
#endif

#define LUA_WIRE_LIMIT (1024u * 1024u)
#define LUA_VM_OBJECT "ZSharp.Interop.VMObject"
#define LUA_EXACT_NUMBER "ZSharp.Interop.ExactNumber"

typedef struct LuaObject {
    int reference;
    const void *identity;
} LuaObject;

typedef struct LuaJsonProperties {
    JSPropertyEnum *items;
    uint32_t count;
} LuaJsonProperties;

typedef struct LuaModule {
    char *path;
    lua_State *state;
    JSRuntime *json_runtime;
    JSContext *json;
    JSValue *owned_values;
    size_t owned_count, owned_capacity;
    const char **owned_strings;
    size_t string_count, string_capacity;
    LuaJsonProperties *owned_properties;
    size_t property_count, property_capacity;
    size_t memory_used;
    uint64_t expires;
    LuaMutex mutex;
    int active;
    int loaded;
    int exports;
    int identities;
    int records;
    LuaObject *objects;
    size_t object_count;
    ZSharpInteropHostDispatch host;
    void *host_context;
    struct LuaModule *next;
} LuaModule;

struct ZSharpLuaInterop { LuaModule *modules; };

typedef struct LuaVMObject {
    LuaModule *module;
    char *reference;
    char *type;
} LuaVMObject;

typedef struct LuaRequest {
    LuaModule *module;
    const char *operation;
    const char *target;
    const char *member;
    const char *handle;
    const char *arguments;
} LuaRequest;

static JSValue encode_value(LuaModule *, int, unsigned, int);
static int decode_value(LuaModule *, JSValueConst, unsigned);

/* Lua errors use longjmp. Keep ownership of every temporary JSON allocation
 * until it is explicitly consumed/freed, so a bad argument or Lua allocation
 * failure cannot leak QuickJS references across persistent calls. */
static JSValue keep_value(LuaModule *module, JSValue value) {
    JSValue *resized;
    if (!JS_VALUE_HAS_REF_COUNT(value)) return value;
    if (module->owned_count == module->owned_capacity) {
        size_t capacity = module->owned_capacity ? module->owned_capacity * 2 : 32;
        resized = (JSValue *)realloc(module->owned_values, capacity * sizeof(*resized));
        if (!resized) { JS_FreeValue(module->json, value); return JS_ThrowOutOfMemory(module->json); }
        module->owned_values = resized; module->owned_capacity = capacity;
    }
    module->owned_values[module->owned_count++] = value;
    return value;
}

static JSValue consume_value(LuaModule *module, JSValue value) {
    size_t index;
    if (JS_VALUE_HAS_REF_COUNT(value))
        for (index = module->owned_count; index > 0; index--)
            if (JS_VALUE_GET_TAG(module->owned_values[index - 1]) == JS_VALUE_GET_TAG(value) &&
                JS_VALUE_GET_PTR(module->owned_values[index - 1]) == JS_VALUE_GET_PTR(value)) {
                module->owned_values[index - 1] = module->owned_values[--module->owned_count];
                break;
            }
    return value;
}

static void free_value(LuaModule *module, JSValue value) {
    JS_FreeValue(module->json, consume_value(module, value));
}

static const char *keep_string(LuaModule *module, const char *text) {
    const char **resized;
    if (!text) return NULL;
    if (module->string_count == module->string_capacity) {
        size_t capacity = module->string_capacity ? module->string_capacity * 2 : 16;
        resized = (const char **)realloc(module->owned_strings, capacity * sizeof(*resized));
        if (!resized) { JS_FreeCString(module->json, text); JS_ThrowOutOfMemory(module->json); return NULL; }
        module->owned_strings = resized; module->string_capacity = capacity;
    }
    module->owned_strings[module->string_count++] = text;
    return text;
}

static void free_string(LuaModule *module, const char *text) {
    size_t index;
    for (index = module->string_count; index > 0; index--)
        if (module->owned_strings[index - 1] == text) {
            module->owned_strings[index - 1] = module->owned_strings[--module->string_count];
            break;
        }
    JS_FreeCString(module->json, text);
}

static int keep_properties(LuaModule *module, JSPropertyEnum **items,
                            uint32_t *count, JSValueConst object, int flags) {
    LuaJsonProperties *resized;
    int result = JS_GetOwnPropertyNames(module->json, items, count, object, flags);
    if (result < 0) return result;
    if (module->property_count == module->property_capacity) {
        size_t capacity = module->property_capacity ? module->property_capacity * 2 : 8;
        resized = (LuaJsonProperties *)realloc(module->owned_properties, capacity * sizeof(*resized));
        if (!resized) {
            uint32_t index;
            for (index = 0; index < *count; index++) JS_FreeAtom(module->json, (*items)[index].atom);
            js_free(module->json, *items);
            JS_ThrowOutOfMemory(module->json); return -1;
        }
        module->owned_properties = resized; module->property_capacity = capacity;
    }
    module->owned_properties[module->property_count].items = *items;
    module->owned_properties[module->property_count++].count = *count;
    return result;
}

static void free_properties(LuaModule *module, JSPropertyEnum *items) {
    size_t index;
    for (index = module->property_count; index > 0; index--)
        if (module->owned_properties[index - 1].items == items) {
            uint32_t atom;
            for (atom = 0; atom < module->owned_properties[index - 1].count; atom++)
                JS_FreeAtom(module->json, items[atom].atom);
            module->owned_properties[index - 1] = module->owned_properties[--module->property_count];
            js_free(module->json, items);
            return;
        }
}

static void free_pending(LuaModule *module) {
    while (module->property_count)
        free_properties(module, module->owned_properties[module->property_count - 1].items);
    while (module->string_count) JS_FreeCString(module->json, module->owned_strings[--module->string_count]);
    while (module->owned_count) JS_FreeValue(module->json, module->owned_values[--module->owned_count]);
    { JSValue exception = JS_GetException(module->json); JS_FreeValue(module->json, exception); }
}

#define JS_NewArray(context) keep_value(module, JS_NewArray(context))
#define JS_NewObject(context) keep_value(module, JS_NewObject(context))
#define JS_NewString(context, text) keep_value(module, JS_NewString(context, text))
#define JS_NewStringLen(context, text, length) keep_value(module, JS_NewStringLen(context, text, length))
#define JS_ParseJSON(context, text, length, name) keep_value(module, JS_ParseJSON(context, text, length, name))
#define JS_JSONStringify(context, value, replacer, space) keep_value(module, JS_JSONStringify(context, value, replacer, space))
#define JS_GetException(context) keep_value(module, JS_GetException(context))
#define JS_GetPropertyStr(context, object, key) keep_value(module, JS_GetPropertyStr(context, object, key))
#define JS_GetPropertyUint32(context, object, key) keep_value(module, JS_GetPropertyUint32(context, object, key))
#define JS_GetProperty(context, object, key) keep_value(module, JS_GetProperty(context, object, key))
#define JS_SetPropertyStr(context, object, key, value) JS_SetPropertyStr(context, object, key, consume_value(module, value))
#define JS_SetPropertyUint32(context, object, key, value) JS_SetPropertyUint32(context, object, key, consume_value(module, value))
#define JS_FreeValue(context, value) free_value(module, value)
#define JS_ToCString(context, value) keep_string(module, JS_ToCString(context, value))
#define JS_ToCStringLen(context, length, value) keep_string(module, JS_ToCStringLen(context, length, value))
#define JS_AtomToCString(context, atom) keep_string(module, JS_AtomToCString(context, atom))
#define JS_FreeCString(context, text) free_string(module, text)
#define JS_GetOwnPropertyNames(context, items, count, object, flags) keep_properties(module, items, count, object, flags)

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static void *limited_alloc(void *context, void *pointer, size_t old_size,
                           size_t new_size) {
    LuaModule *module = (LuaModule *)context;
    const size_t limit = 48u * 1024u * 1024u;
    void *resized;
    if (!pointer) old_size = 0;
    if (!new_size) {
        free(pointer);
        module->memory_used = old_size > module->memory_used ? 0 : module->memory_used - old_size;
        return NULL;
    }
    if (new_size > old_size && new_size - old_size > limit - module->memory_used)
        return NULL;
    resized = realloc(pointer, new_size);
    if (resized)
        module->memory_used = old_size > module->memory_used ? new_size : module->memory_used - old_size + new_size;
    return resized;
}

static void timeout_hook(lua_State *state, lua_Debug *debug) {
    LuaModule *module = *(LuaModule **)lua_getextraspace(state);
    (void)debug;
    if (module && module->expires && now_ms() >= module->expires)
        luaL_error(state, "Lua execution exceeded the 5 second limit");
}

static int json_interrupt(JSRuntime *runtime, void *context) {
    LuaModule *module = (LuaModule *)context;
    (void)runtime;
    return module->expires && now_ms() >= module->expires;
}

static int lua_traceback(lua_State *state) {
    const char *message = lua_tostring(state, 1);
    luaL_traceback(state, state, message ? message : "Lua interop error", 1);
    return 1;
}

static int json_failure(LuaModule *module) {
    JSValue exception = JS_GetException(module->json);
    const char *message = JS_ToCString(module->json, exception);
    char copy[384];
    snprintf(copy, sizeof(copy), "Lua bridge JSON error: %s", message ? message : "out of memory");
    if (message) JS_FreeCString(module->json, message);
    JS_FreeValue(module->json, exception);
    return luaL_error(module->state, "%s", copy);
}

static char *json_string(LuaModule *module, JSValueConst value) {
    JSValue encoded = JS_JSONStringify(module->json, value, JS_UNDEFINED, JS_UNDEFINED);
    const char *text;
    char *copy = NULL;
    size_t length = 0;
    if (JS_IsException(encoded)) return NULL;
    text = JS_ToCStringLen(module->json, &length, encoded);
    if (text && length <= LUA_WIRE_LIMIT) copy = copy_text(text);
    if (text) JS_FreeCString(module->json, text);
    JS_FreeValue(module->json, encoded);
    return copy;
}

static int object_gc(lua_State *state) {
    LuaVMObject *object = (LuaVMObject *)luaL_checkudata(state, 1, LUA_VM_OBJECT);
    free(object->reference);
    free(object->type);
    object->reference = object->type = NULL;
    return 0;
}

static int valid_decimal(const char *text, size_t *significant) {
    const char *cursor = text;
    int started = 0;
    size_t count = 0;
    if (*cursor == '-') cursor++;
    if (*cursor == '0') cursor++;
    else {
        if (*cursor < '1' || *cursor > '9') return 0;
        do { cursor++; } while (*cursor >= '0' && *cursor <= '9');
    }
    if (*cursor == '.') {
        cursor++;
        if (*cursor < '0' || *cursor > '9') return 0;
        do { cursor++; } while (*cursor >= '0' && *cursor <= '9');
    }
    { const char *digit;
      for (digit = text; digit < cursor; digit++)
          if (*digit >= '0' && *digit <= '9') {
              if (*digit != '0') started = 1;
              if (started) count++;
          }
    }
    if (*cursor == 'e' || *cursor == 'E') {
        cursor++;
        if (*cursor == '+' || *cursor == '-') cursor++;
        if (*cursor < '0' || *cursor > '9') return 0;
        do { cursor++; } while (*cursor >= '0' && *cursor <= '9');
    }
    if (*cursor) return 0;
    *significant = count;
    return 1;
}

static void push_exact_number(lua_State *state, const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)lua_newuserdatauv(state, length + 1, 0);
    memcpy(copy, text, length + 1);
    luaL_setmetatable(state, LUA_EXACT_NUMBER);
}

static int exact_tostring(lua_State *state) {
    const char *text = (const char *)luaL_checkudata(state, 1, LUA_EXACT_NUMBER);
    lua_pushstring(state, text);
    return 1;
}

static int exact_arithmetic(lua_State *state) {
    return luaL_error(state, "exact decimal requires explicit tonumber(tostring(value)) conversion before native Lua arithmetic");
}

static int make_exact_number(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);
    size_t digits;
    if (!valid_decimal(text, &digits)) return luaL_error(state, "ZSharp.number requires finite decimal text");
    push_exact_number(state, text);
    return 1;
}

static LuaModule *closure_module(lua_State *state) {
    return (LuaModule *)lua_touserdata(state, lua_upvalueindex(1));
}

static int callback(lua_State *state, LuaModule *module, const char *operation,
                     const char *target, const char *member,
                     const char *reference, int first_argument) {
    JSValue arguments, item, response;
    char *wire, *result = NULL;
    char error[1024] = {0};
    int index, count = lua_gettop(state) - first_argument + 1, ok;
    if (!module->active || !module->host)
        return luaL_error(state, "Z# callbacks are only available during an active Lua call");
    arguments = JS_NewArray(module->json);
    for (index = 0; index < count; index++) {
        item = encode_value(module, first_argument + index, 0, 0);
        if (JS_IsException(item) || JS_SetPropertyUint32(module->json, arguments, (uint32_t)index, item) < 0) {
            JS_FreeValue(module->json, arguments);
            return json_failure(module);
        }
    }
    wire = json_string(module, arguments);
    JS_FreeValue(module->json, arguments);
    if (!wire) return luaL_error(state, "Lua callback arguments exceed the 1 MiB wire limit or cannot be encoded");
    ok = module->host(module->host_context, operation, target, member, reference,
                       wire, &result, error, sizeof(error));
    free(wire);
    if (!ok) {
        free(result);
        return luaL_error(state, "Z# callback failed: %s", error);
    }
    if (!result || strlen(result) > LUA_WIRE_LIMIT) {
        free(result);
        return luaL_error(state, "Z# callback returned missing or oversized wire JSON");
    }
    response = JS_ParseJSON(module->json, result, strlen(result), "<Z# callback>");
    free(result);
    if (JS_IsException(response)) return json_failure(module);
    ok = decode_value(module, response, 0);
    JS_FreeValue(module->json, response);
    return ok;
}

static int host_call(lua_State *state) {
    LuaModule *module = closure_module(state);
    const char *target = luaL_checkstring(state, 1);
    return callback(state, module, "host.call", target, "", "", 2);
}

static int object_get(lua_State *state) {
    LuaVMObject *object = (LuaVMObject *)luaL_checkudata(state, 1, LUA_VM_OBJECT);
    const char *member = luaL_checkstring(state, 2);
    if (lua_gettop(state) != 2) return luaL_error(state, "Z# object get requires one member name");
    return callback(state, object->module, "host.get", "", member, object->reference, 3);
}

static int object_set(lua_State *state) {
    LuaVMObject *object = (LuaVMObject *)luaL_checkudata(state, 1, LUA_VM_OBJECT);
    const char *member = luaL_checkstring(state, 2);
    if (lua_gettop(state) != 3) return luaL_error(state, "Z# object set requires a member name and one value");
    return callback(state, object->module, "host.set", "", member, object->reference, 3);
}

static int object_invoke(lua_State *state) {
    LuaVMObject *object = (LuaVMObject *)luaL_checkudata(state, 1, LUA_VM_OBJECT);
    const char *member = luaL_checkstring(state, 2);
    return callback(state, object->module, "host.invoke", "", member, object->reference, 3);
}

static int host_get(lua_State *state) {
    return object_get(state);
}

static int host_set(lua_State *state) {
    return object_set(state);
}

static int host_invoke(lua_State *state) {
    return object_invoke(state);
}

static int mark_record(lua_State *state) {
    LuaModule *module = closure_module(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_rawgeti(state, LUA_REGISTRYINDEX, module->records);
    lua_pushvalue(state, 1);
    lua_pushboolean(state, 1);
    lua_rawset(state, -3);
    lua_pop(state, 1);
    lua_settop(state, 1);
    return 1;
}

static int module_loader(lua_State *state) {
    lua_getglobal(state, "ZSharp");
    return 1;
}

static void set_closure(lua_State *state, LuaModule *module,
                         const char *name, lua_CFunction function) {
    lua_pushlightuserdata(state, module);
    lua_pushcclosure(state, function, 1);
    lua_setfield(state, -2, name);
}

static int initialize_state(lua_State *state) {
    LuaModule *module = closure_module(state);
    luaL_openlibs(state);
    lua_newtable(state);
    module->identities = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state);
    lua_newtable(state);
    lua_pushliteral(state, "k");
    lua_setfield(state, -2, "__mode");
    lua_setmetatable(state, -2);
    module->records = luaL_ref(state, LUA_REGISTRYINDEX);
    if (luaL_newmetatable(state, LUA_VM_OBJECT)) {
        lua_pushcfunction(state, object_gc);
        lua_setfield(state, -2, "__gc");
        lua_newtable(state);
        lua_pushcfunction(state, object_get); lua_setfield(state, -2, "get");
        lua_pushcfunction(state, object_set); lua_setfield(state, -2, "set");
        lua_pushcfunction(state, object_invoke); lua_setfield(state, -2, "invoke");
        lua_setfield(state, -2, "__index");
    }
    lua_pop(state, 1);
    if (luaL_newmetatable(state, LUA_EXACT_NUMBER)) {
        static const char *operations[] = {"__add", "__sub", "__mul", "__div", "__idiv", "__mod", "__pow", "__unm", "__lt", "__le"};
        size_t index;
        lua_pushcfunction(state, exact_tostring); lua_setfield(state, -2, "__tostring");
        for (index = 0; index < sizeof(operations) / sizeof(operations[0]); index++) {
            lua_pushcfunction(state, exact_arithmetic); lua_setfield(state, -2, operations[index]);
        }
    }
    lua_pop(state, 1);
    lua_newtable(state);
    set_closure(state, module, "call", host_call);
    set_closure(state, module, "get", host_get);
    set_closure(state, module, "set", host_set);
    set_closure(state, module, "invoke", host_invoke);
    set_closure(state, module, "record", mark_record);
    set_closure(state, module, "number", make_exact_number);
    lua_setglobal(state, "ZSharp");
    lua_getglobal(state, "package");
    lua_getfield(state, -1, "preload");
    lua_pushcfunction(state, module_loader);
    lua_setfield(state, -2, "zsharp");
    lua_pop(state, 2);
    return 0;
}

static LuaObject *find_object(LuaModule *module, const char *handle) {
    char *end;
    unsigned long long id;
    if (!handle || !*handle || handle[0] == '-' || handle[0] == '+') return NULL;
    errno = 0;
    id = strtoull(handle, &end, 10);
    if (errno || *end || id == 0 || id > module->object_count ||
        module->objects[id - 1].reference == LUA_NOREF) return NULL;
    return &module->objects[id - 1];
}

static JSValue reference_value(LuaModule *module, int index) {
    lua_State *state = module->state;
    const void *identity = lua_topointer(state, index);
    lua_Integer id;
    JSValue result;
    char number[32];
    index = lua_absindex(state, index);
    lua_rawgeti(state, LUA_REGISTRYINDEX, module->identities);
    lua_rawgetp(state, -1, identity);
    id = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (!id) {
        LuaObject *resized;
        if (module->object_count >= 131072) {
            lua_pop(state, 1);
            luaL_error(state, "Lua object reference limit reached");
            return JS_EXCEPTION;
        }
        resized = (LuaObject *)realloc(module->objects, (module->object_count + 1) * sizeof(*resized));
        if (!resized) {
            lua_pop(state, 1);
            luaL_error(state, "out of memory retaining Lua object");
            return JS_EXCEPTION;
        }
        module->objects = resized;
        id = (lua_Integer)++module->object_count;
        module->objects[id - 1].identity = identity;
        lua_pushvalue(state, index);
        module->objects[id - 1].reference = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_pushinteger(state, id);
        lua_rawsetp(state, -2, identity);
    }
    lua_pop(state, 1);
    snprintf(number, sizeof(number), "%lld", (long long)id);
    result = JS_NewObject(module->json);
    JS_SetPropertyStr(module->json, result, "$ref", JS_NewString(module->json, number));
    JS_SetPropertyStr(module->json, result, "$type", JS_NewString(module->json,
                      lua_istable(state, index) ? "LuaTable" : "LuaObject"));
    return result;
}

static int table_is_array(LuaModule *module, int index, size_t *length) {
    lua_State *state = module->state;
    size_t count = 0, size;
    int array = 1;
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index)) { lua_pop(state, 1); return 0; }
    size = lua_rawlen(state, index);
    lua_pushnil(state);
    while (lua_next(state, index)) {
        lua_Integer key;
        if (!lua_isinteger(state, -2) || (key = lua_tointeger(state, -2)) < 1 ||
            (lua_Unsigned)key > size) array = 0;
        count++;
        lua_pop(state, 1);
    }
    *length = size;
    return array && count == size;
}

static JSValue encode_value(LuaModule *module, int index, unsigned depth, int force_object) {
    lua_State *state = module->state;
    JSContext *json = module->json;
    int type = lua_type(state, index);
    JSValue result;
    index = lua_absindex(state, index);
    if (depth > 128) { luaL_error(state, "Lua bridge value nesting limit exceeded"); return JS_EXCEPTION; }
    if (!lua_checkstack(state, 12)) { luaL_error(state, "Lua bridge stack limit exceeded"); return JS_EXCEPTION; }
    if (type == LUA_TNIL) return JS_NULL;
    if (type == LUA_TBOOLEAN) return JS_NewBool(json, lua_toboolean(state, index));
    if (type == LUA_TSTRING) {
        size_t length;
        const char *text = lua_tolstring(state, index, &length);
        if (memchr(text, '\0', length)) { luaL_error(state, "Lua text contains an embedded null byte"); return JS_EXCEPTION; }
        return JS_NewStringLen(json, text, length);
    }
    if (type == LUA_TNUMBER) {
        char number[96];
        if (lua_isinteger(state, index))
            snprintf(number, sizeof(number), "%lld", (long long)lua_tointeger(state, index));
        else {
            double value = (double)lua_tonumber(state, index);
            if (!isfinite(value)) { luaL_error(state, "non-finite Lua number cannot cross the bridge"); return JS_EXCEPTION; }
            { int precision;
              for (precision = 1; precision <= 17; precision++) {
                  snprintf(number, sizeof(number), "%.*g", precision, value);
                  if (strtod(number, NULL) == value) break;
              }
            }
        }
        result = JS_NewObject(json);
        JS_SetPropertyStr(json, result, "$number", JS_NewString(json, number));
        return result;
    }
    if (type == LUA_TUSERDATA) {
        const char *exact = (const char *)luaL_testudata(state, index, LUA_EXACT_NUMBER);
        LuaVMObject *object = (LuaVMObject *)luaL_testudata(state, index, LUA_VM_OBJECT);
        if (exact) {
            result = JS_NewObject(json);
            JS_SetPropertyStr(json, result, "$number", JS_NewString(json, exact));
            return result;
        }
        if (object) {
            result = JS_NewObject(json);
            JS_SetPropertyStr(json, result, "$zref", JS_NewString(json, object->reference));
            JS_SetPropertyStr(json, result, "$type", JS_NewString(json, object->type));
            return result;
        }
        return reference_value(module, index);
    }
    if (type == LUA_TTABLE) {
        size_t length, item;
        int record;
        lua_rawgeti(state, LUA_REGISTRYINDEX, module->records);
        lua_pushvalue(state, index);
        lua_rawget(state, -2);
        record = lua_toboolean(state, -1);
        lua_pop(state, 2);
        if (record && !force_object) {
            JSValue fields = JS_NewObject(json);
            lua_pushnil(state);
            while (lua_next(state, index)) {
                const char *key;
                JSValue value;
                if (lua_type(state, -2) != LUA_TSTRING) {
                    JS_FreeValue(json, fields);
                    lua_pop(state, 2);
                    luaL_error(state, "Lua records require text keys");
                    return JS_EXCEPTION;
                }
                key = lua_tostring(state, -2);
                value = encode_value(module, -1, depth + 1, 0);
                if (JS_IsException(value) || JS_SetPropertyStr(json, fields, key, value) < 0) {
                    JS_FreeValue(json, fields); lua_pop(state, 2); return JS_EXCEPTION;
                }
                lua_pop(state, 1);
            }
            result = JS_NewObject(json);
            JS_SetPropertyStr(json, result, "$record", fields);
            return result;
        }
        if (force_object || !table_is_array(module, index, &length)) return reference_value(module, index);
        if (length > 131072) { luaL_error(state, "Lua array exceeds bridge limit"); return JS_EXCEPTION; }
        result = JS_NewArray(json);
        for (item = 0; item < length; item++) {
            JSValue value;
            lua_rawgeti(state, index, (lua_Integer)item + 1);
            value = encode_value(module, -1, depth + 1, 0);
            lua_pop(state, 1);
            if (JS_IsException(value) || JS_SetPropertyUint32(json, result, (uint32_t)item, value) < 0) {
                JS_FreeValue(json, result); return JS_EXCEPTION;
            }
        }
        return result;
    }
    luaL_error(state, "unsupported Lua bridge value: %s", lua_typename(state, type));
    return JS_EXCEPTION;
}

static int decode_value(LuaModule *module, JSValueConst value, unsigned depth) {
    lua_State *state = module->state;
    JSContext *json = module->json;
    if (depth > 128) return luaL_error(state, "Lua bridge value nesting limit exceeded");
    if (!lua_checkstack(state, 12)) return luaL_error(state, "Lua bridge stack limit exceeded");
    if (JS_IsNull(value) || JS_IsUndefined(value)) { lua_pushnil(state); return 1; }
    if (JS_IsBool(value)) { lua_pushboolean(state, JS_ToBool(json, value)); return 1; }
    if (JS_IsString(value)) {
        size_t length;
        const char *text = JS_ToCStringLen(json, &length, value);
        if (!text) return json_failure(module);
        lua_pushlstring(state, text, length);
        JS_FreeCString(json, text);
        return 1;
    }
    if (JS_IsArray(value)) {
        JSValue length_value = JS_GetPropertyStr(json, value, "length");
        uint32_t length, index;
        if (JS_ToUint32(json, &length, length_value) < 0) { JS_FreeValue(json, length_value); return json_failure(module); }
        JS_FreeValue(json, length_value);
        if (length > 131072) return luaL_error(state, "Lua array exceeds bridge limit");
        lua_createtable(state, (int)length, 0);
        for (index = 0; index < length; index++) {
            JSValue item = JS_GetPropertyUint32(json, value, index);
            decode_value(module, item, depth + 1);
            JS_FreeValue(json, item);
            lua_rawseti(state, -2, (lua_Integer)index + 1);
        }
        return 1;
    }
    if (JS_IsObject(value)) {
        JSValue item = JS_GetPropertyStr(json, value, "$number");
        if (!JS_IsUndefined(item)) {
            const char *text;
            char *end;
            long long integer;
            double number;
            size_t significant;
            if (!JS_IsString(item)) { JS_FreeValue(json, item); return luaL_error(state, "$number requires decimal text"); }
            text = JS_ToCString(json, item);
            if (!text) { JS_FreeValue(json, item); return json_failure(module); }
            if (!valid_decimal(text, &significant)) {
                JS_FreeCString(json, text); JS_FreeValue(json, item);
                return luaL_error(state, "$number requires finite decimal text");
            }
            errno = 0;
            integer = strtoll(text, &end, 10);
            if (*text && !*end && !errno) lua_pushinteger(state, (lua_Integer)integer);
            else {
                errno = 0;
                number = strtod(text, &end);
                if (*end || !isfinite(number) || errno == ERANGE || significant > 15 ||
                    (number == 0 && significant > 0)) push_exact_number(state, text);
                else lua_pushnumber(state, (lua_Number)number);
            }
            JS_FreeCString(json, text); JS_FreeValue(json, item);
            return 1;
        }
        JS_FreeValue(json, item);
        item = JS_GetPropertyStr(json, value, "$ref");
        if (!JS_IsUndefined(item)) {
            const char *reference = JS_ToCString(json, item);
            LuaObject *object = reference ? find_object(module, reference) : NULL;
            if (reference) JS_FreeCString(json, reference);
            JS_FreeValue(json, item);
            if (!object) return luaL_error(state, "Lua object reference is invalid or has been released");
            lua_rawgeti(state, LUA_REGISTRYINDEX, object->reference);
            return 1;
        }
        JS_FreeValue(json, item);
        item = JS_GetPropertyStr(json, value, "$zref");
        if (!JS_IsUndefined(item)) {
            JSValue type_value = JS_GetPropertyStr(json, value, "$type");
            const char *reference = JS_ToCString(json, item);
            const char *type = JS_IsString(type_value) ? JS_ToCString(json, type_value) : NULL;
            LuaVMObject *object;
            char *reference_copy = reference ? copy_text(reference) : NULL;
            char *type_copy = copy_text(type ? type : "ZSharpObject");
            if (reference) JS_FreeCString(json, reference);
            if (type) JS_FreeCString(json, type);
            JS_FreeValue(json, item); JS_FreeValue(json, type_value);
            if (!reference_copy || !type_copy || !*reference_copy) {
                free(reference_copy); free(type_copy);
                return luaL_error(state, "missing Z# object reference or out of memory");
            }
            object = (LuaVMObject *)lua_newuserdatauv(state, sizeof(*object), 0);
            object->module = module;
            object->reference = reference_copy;
            object->type = type_copy;
            luaL_setmetatable(state, LUA_VM_OBJECT);
            return 1;
        }
        JS_FreeValue(json, item);
        item = JS_GetPropertyStr(json, value, "$record");
        if (JS_IsObject(item) && !JS_IsArray(item)) {
            JSPropertyEnum *properties;
            uint32_t count, index;
            if (JS_GetOwnPropertyNames(json, &properties, &count, item,
                                      JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                JS_FreeValue(json, item); return json_failure(module);
            }
            lua_createtable(state, 0, (int)count);
            for (index = 0; index < count; index++) {
                const char *key = JS_AtomToCString(json, properties[index].atom);
                JSValue field = JS_GetProperty(json, item, properties[index].atom);
                if (!key) { JS_FreeValue(json, field); continue; }
                decode_value(module, field, depth + 1);
                lua_setfield(state, -2, key);
                JS_FreeCString(json, key);
                JS_FreeValue(json, field);
            }
            free_properties(module, properties);
            JS_FreeValue(json, item);
            lua_rawgeti(state, LUA_REGISTRYINDEX, module->records);
            lua_pushvalue(state, -2); lua_pushboolean(state, 1); lua_rawset(state, -3); lua_pop(state, 1);
            return 1;
        }
        JS_FreeValue(json, item);
    }
    return luaL_error(state, "unknown or untagged Lua bridge value");
}

static void push_export(LuaModule *module, const char *target) {
    lua_State *state = module->state;
    const char *part = target, *dot;
    if (module->exports != LUA_NOREF) lua_rawgeti(state, LUA_REGISTRYINDEX, module->exports);
    else lua_pushglobaltable(state);
    do {
        dot = strchr(part, '.');
        lua_pushlstring(state, part, dot ? (size_t)(dot - part) : strlen(part));
        lua_gettable(state, -2);
        lua_remove(state, -2);
        if (dot && !lua_istable(state, -1)) luaL_error(state, "Lua export path '%s' is not a table", target);
        part = dot ? dot + 1 : NULL;
    } while (part);
}

static int dispatch_request(lua_State *state) {
    LuaRequest *request = (LuaRequest *)lua_touserdata(state, lua_upvalueindex(1));
    LuaModule *module = request->module;
    JSValue arguments, length_value;
    uint32_t count, index;
    int argument_count, self = 0;
    LuaObject *object;
    if (!module->loaded) {
        if (luaL_loadfile(state, module->path) != LUA_OK) return lua_error(state);
        lua_call(state, 0, 1);
        if (lua_istable(state, -1)) module->exports = luaL_ref(state, LUA_REGISTRYINDEX);
        else lua_pop(state, 1);
        module->loaded = 1;
    }
    if (!strcmp(request->operation, "call") || !strcmp(request->operation, "new")) {
        if (!request->target || !*request->target) return luaL_error(state, "Lua call requires an exported target");
        push_export(module, request->target);
        if (!lua_isfunction(state, -1) && !strcmp(request->operation, "new") && lua_istable(state, -1)) {
            lua_getfield(state, -1, "new");
            lua_remove(state, -2);
        }
        if (!lua_isfunction(state, -1)) return luaL_error(state, "Lua module has no exported function '%s'", request->target);
    } else {
        object = find_object(module, request->handle);
        if (!object) return luaL_error(state, "Lua object reference is invalid or has been released");
        if (!strcmp(request->operation, "release")) {
            lua_rawgeti(state, LUA_REGISTRYINDEX, module->identities);
            lua_pushnil(state); lua_rawsetp(state, -2, object->identity); lua_pop(state, 1);
            luaL_unref(state, LUA_REGISTRYINDEX, object->reference);
            object->reference = LUA_NOREF;
            lua_pushnil(state);
            return 1;
        }
        if (!request->member || !*request->member) return luaL_error(state, "Lua object access requires a member name");
        lua_rawgeti(state, LUA_REGISTRYINDEX, object->reference);
        if (!strcmp(request->operation, "get")) { lua_getfield(state, -1, request->member); return 1; }
        if (!strcmp(request->operation, "invoke")) {
            lua_getfield(state, -1, request->member);
            if (!lua_isfunction(state, -1)) return luaL_error(state, "Lua object member '%s' is not a function", request->member);
            lua_insert(state, -2);
            self = 1;
        } else if (strcmp(request->operation, "set")) return luaL_error(state, "unsupported Lua bridge operation '%s'", request->operation);
    }
    arguments = JS_ParseJSON(module->json, request->arguments, strlen(request->arguments), "<Lua bridge arguments>");
    if (JS_IsException(arguments)) return json_failure(module);
    if (!JS_IsArray(arguments)) { JS_FreeValue(module->json, arguments); return luaL_error(state, "Lua arguments must be a JSON array"); }
    length_value = JS_GetPropertyStr(module->json, arguments, "length");
    if (JS_ToUint32(module->json, &count, length_value) < 0) { JS_FreeValue(module->json, length_value); JS_FreeValue(module->json, arguments); return json_failure(module); }
    JS_FreeValue(module->json, length_value);
    if (count > 1024 || !lua_checkstack(state, (int)count + 16)) { JS_FreeValue(module->json, arguments); return luaL_error(state, "Lua bridge has too many arguments"); }
    for (index = 0; index < count; index++) {
        JSValue argument = JS_GetPropertyUint32(module->json, arguments, index);
        decode_value(module, argument, 0);
        JS_FreeValue(module->json, argument);
    }
    JS_FreeValue(module->json, arguments);
    if (!strcmp(request->operation, "set")) {
        if (count != 1) return luaL_error(state, "Lua object set requires one value");
        lua_setfield(state, -2, request->member);
        lua_pushnil(state);
        return 1;
    }
    argument_count = (int)count + self;
    lua_call(state, argument_count, 1);
    if (!strcmp(request->operation, "new") && !lua_istable(state, -1) && !lua_isuserdata(state, -1))
        return luaL_error(state, "Lua constructor must return an object table or userdata");
    return 1;
}

static int encode_result(lua_State *state) {
    LuaRequest *request = (LuaRequest *)lua_touserdata(state, lua_upvalueindex(1));
    LuaModule *module = request->module;
    JSValue value = encode_value(module, 1, 0, !strcmp(request->operation, "new"));
    char *wire;
    if (JS_IsException(value)) return json_failure(module);
    wire = json_string(module, value);
    JS_FreeValue(module->json, value);
    if (!wire) return luaL_error(state, "Lua result exceeds the 1 MiB wire limit or cannot be encoded");
    lua_pushstring(state, wire);
    free(wire);
    return 1;
}

static LuaModule *create_module(const char *path, char *error, size_t size) {
    LuaModule *module = (LuaModule *)calloc(1, sizeof(*module));
    if (!module) { snprintf(error, size, "out of memory creating Lua module"); return NULL; }
    module->path = copy_text(path);
    module->exports = module->identities = module->records = LUA_NOREF;
    mutex_init(&module->mutex);
    module->json_runtime = JS_NewRuntime();
    if (module->json_runtime) {
        JS_SetMemoryLimit(module->json_runtime, 16u * 1024u * 1024u);
        JS_SetMaxStackSize(module->json_runtime, 256u * 1024u);
        JS_SetInterruptHandler(module->json_runtime, json_interrupt, module);
        module->json = JS_NewContext(module->json_runtime);
    }
    module->state = lua_newstate(limited_alloc, module, (unsigned)time(NULL));
    if (!module->path || !module->state || !module->json) {
        snprintf(error, size, "could not create the retained Lua/JSON runtime");
        if (module->state) lua_close(module->state);
        if (module->json) JS_FreeContext(module->json);
        if (module->json_runtime) JS_FreeRuntime(module->json_runtime);
        mutex_free(&module->mutex); free(module->path); free(module); return NULL;
    }
    *(LuaModule **)lua_getextraspace(module->state) = module;
    module->expires = now_ms() + 5000;
    lua_sethook(module->state, timeout_hook, LUA_MASKCOUNT, 10000);
    lua_pushlightuserdata(module->state, module);
    lua_pushcclosure(module->state, initialize_state, 1);
    if (lua_pcall(module->state, 0, 0, 0) != LUA_OK) {
        snprintf(error, size, "could not initialize Lua: %s", lua_tostring(module->state, -1));
        lua_close(module->state); JS_FreeContext(module->json); JS_FreeRuntime(module->json_runtime);
        mutex_free(&module->mutex); free(module->path); free(module); return NULL;
    }
    module->expires = 0;
    return module;
}

int zsharp_lua_interop_request(ZSharpLuaInterop **cache, const char *module_path,
    const char *operation, const char *target, const char *member,
    const char *handle, const char *arguments_json, ZSharpInteropHostDispatch host,
    void *host_context, char **result_json, char *error, size_t error_size) {
    LuaModule *module;
    LuaRequest request;
    lua_State *state;
    int ok = 0;
    if (result_json) *result_json = NULL;
    if (!cache || !module_path || !operation || !arguments_json || !result_json ||
        strlen(arguments_json) > LUA_WIRE_LIMIT) {
        snprintf(error, error_size, "invalid or oversized Lua interop request"); return 0;
    }
    CACHE_LOCK();
    if (!*cache) *cache = (ZSharpLuaInterop *)calloc(1, sizeof(**cache));
    if (!*cache) { CACHE_UNLOCK(); snprintf(error, error_size, "out of memory creating Lua cache"); return 0; }
    for (module = (*cache)->modules; module; module = module->next)
        if (!strcmp(module->path, module_path)) break;
    if (!module) {
        module = create_module(module_path, error, error_size);
        if (module) { module->next = (*cache)->modules; (*cache)->modules = module; }
    }
    CACHE_UNLOCK();
    if (!module) return 0;
    mutex_lock(&module->mutex);
    if (module->active) { mutex_unlock(&module->mutex); snprintf(error, error_size, "reentrant call to the active Lua module is not supported"); return 0; }
    module->active = 1;
    JS_UpdateStackTop(module->json_runtime);
    module->host = host;
    module->host_context = host_context;
    module->expires = now_ms() + 5000;
    state = module->state;
    lua_settop(state, 0);
    request.module = module; request.operation = operation;
    request.target = target; request.member = member; request.handle = handle;
    request.arguments = arguments_json;
    lua_pushcfunction(state, lua_traceback);
    lua_pushlightuserdata(state, &request);
    lua_pushcclosure(state, dispatch_request, 1);
    if (lua_pcall(state, 0, 1, 1) != LUA_OK) goto failure;
    lua_pushlightuserdata(state, &request);
    lua_pushcclosure(state, encode_result, 1);
    lua_insert(state, -2);
    if (lua_pcall(state, 1, 1, 1) != LUA_OK) goto failure;
    *result_json = copy_text(lua_tostring(state, -1));
    if (!*result_json) snprintf(error, error_size, "out of memory copying Lua result");
    else ok = 1;
    goto cleanup;
failure:
    snprintf(error, error_size, "Lua %s failed: %s", operation,
             lua_tostring(state, -1) ? lua_tostring(state, -1) : "unknown Lua error");
cleanup:
    lua_settop(state, 0);
    free_pending(module);
    module->host = NULL; module->host_context = NULL;
    module->expires = 0; module->active = 0;
    mutex_unlock(&module->mutex);
    return ok;
}

void zsharp_lua_interop_free(ZSharpLuaInterop *cache) {
    LuaModule *module, *next;
    if (!cache) return;
    for (module = cache->modules; module; module = next) {
        next = module->next;
        lua_close(module->state);
        free_pending(module);
        JS_FreeContext(module->json);
        JS_FreeRuntime(module->json_runtime);
        mutex_free(&module->mutex);
        free(module->objects);
        free(module->owned_values);
        free(module->owned_strings);
        free(module->owned_properties);
        free(module->path);
        free(module);
    }
    free(cache);
}
