/* Included after the VM's object/module helpers. No wire handles are pointers. */
typedef struct InteropVMFrame {
    ZSharpProgram *program;
    ZSharpRoom *room;
    RuntimeHeap *heap;
    const char *root;
    const ZSharpProviderBinding *providers;
    size_t provider_count;
    RuntimeModuleCache *cache;
    unsigned depth;
    const ZSharpFunction *callback_wrapper;
} InteropVMFrame;
#ifdef _MSC_VER
__declspec(thread) static InteropVMFrame *interop_current_frame;
#else
static _Thread_local InteropVMFrame *interop_current_frame;
#endif
typedef struct InteropSession {
    char *key;
    ZSharpInteropProcess *process;
    unsigned serial;
    int busy;
    int failed;
    struct InteropSession *next;
} InteropSession;
typedef struct InteropCapability {
    char *language, *module;
    char id[40];
    RuntimeObject *object;
    RuntimeHeap *owner_heap;
    ZSharpProgram *owner_program;
    const char *owner_root;
    struct InteropCapability *next;
} InteropCapability;
typedef struct InteropVMState {
#ifdef _WIN32
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
    InteropSession *sessions;
    InteropCapability *capabilities;
    size_t next_capability;
    ZSharpEmbeddedInterop *embedded;
    ZSharpScriptInterop *script;
    char **legacy_modules;
    size_t legacy_count;
} InteropVMState;
typedef struct InteropDispatch {
    InteropVMFrame *frame;
    InteropVMState *state;
    const char *language, *module;
} InteropDispatch;

static int execute_function(ZSharpProgram *, ZSharpRoom *, ZSharpFunction *,
    RuntimeObject *, RuntimeHeap *, const char *, const ZSharpProviderBinding *,
    size_t, RuntimeModuleCache *, unsigned, const RuntimeValue *, size_t,
    const char *, RuntimeValue *, int *, char *, size_t);
static int interop_host_dispatch(void *, const char *, const char *, const char *,
    const char *, const char *, char **, char *, size_t);

static InteropVMState *interop_state(RuntimeModuleCache *cache, char *error, size_t size) {
    if (cache->interop_owner) cache = cache->interop_owner;
#ifdef _WIN32
    static SRWLOCK creation = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&creation);
#else
    static pthread_mutex_t creation = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&creation);
#endif
    if (!cache->interop) {
        InteropVMState *state = calloc(1, sizeof(*state));
        if (state) {
#ifdef _WIN32
            InitializeCriticalSection(&state->mutex);
#else
            pthread_mutexattr_t attribute;
            pthread_mutexattr_init(&attribute);
            pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&state->mutex, &attribute);
            pthread_mutexattr_destroy(&attribute);
#endif
            cache->interop = state;
        }
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&creation);
#else
    pthread_mutex_unlock(&creation);
#endif
    if (!cache->interop) snprintf(error, size, "out of memory creating interoperability state");
    return cache->interop;
}
static void interop_lock(InteropVMState *state) {
#ifdef _WIN32
    EnterCriticalSection(&state->mutex);
#else
    pthread_mutex_lock(&state->mutex);
#endif
}
static void interop_unlock(InteropVMState *state) {
#ifdef _WIN32
    LeaveCriticalSection(&state->mutex);
#else
    pthread_mutex_unlock(&state->mutex);
#endif
}
static void interop_vm_free(InteropVMState *state) {
    InteropSession *session;
    InteropCapability *capability;
    if (!state) return;
    {
        size_t index;
        for (index = 0; index < state->legacy_count; index++) free(state->legacy_modules[index]);
        free(state->legacy_modules);
    }
    while ((session = state->sessions) != NULL) {
        state->sessions = session->next;
        zsharp_interop_process_destroy(session->process);
        free(session->key); free(session);
    }
    while ((capability = state->capabilities) != NULL) {
        state->capabilities = capability->next;
        free(capability->language); free(capability->module); free(capability);
    }
    zsharp_embedded_interop_free(state->embedded);
    zsharp_script_interop_free(state->script);
#ifdef _WIN32
    DeleteCriticalSection(&state->mutex);
#else
    pthread_mutex_destroy(&state->mutex);
#endif
    free(state);
}
static void interop_vm_detach_heap(InteropVMState *state, RuntimeHeap *heap) {
    InteropCapability **link;
    if (!state) return;
    interop_lock(state);
    link = &state->capabilities;
    while (*link) {
        InteropCapability *entry = *link;
        if (entry->owner_heap == heap) {
            *link = entry->next;
            free(entry->language); free(entry->module); free(entry);
        } else link = &entry->next;
    }
    interop_unlock(state);
}
static char *interop_text(RuntimeHeap *heap, const char *text) {
    return heap_add_text(heap, zsharp_copy_text(text ? text : "", strlen(text ? text : "")));
}
static char *interop_json_string(JSContext *context, JSValueConst value) {
    JSValue string = JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED);
    const char *utf8 = JS_IsException(string) ? NULL : JS_ToCString(context, string);
    char *result = utf8 ? zsharp_copy_text(utf8, strlen(utf8)) : NULL;
    if (utf8) JS_FreeCString(context, utf8);
    JS_FreeValue(context, string);
    return result;
}
static int interop_string_property(JSContext *context, JSValueConst value,
    const char *key, char *output, size_t size) {
    JSValue property = JS_GetPropertyStr(context, value, key);
    size_t length = 0;
    const char *text = JS_IsString(property) ? JS_ToCStringLen(context, &length, property) : NULL;
    int ok = text && length < size && !memchr(text, 0, length);
    if (ok) memcpy(output, text, length + 1);
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, property);
    return ok;
}
static int interop_decimal(const char *input, char **normalized, char *error, size_t size) {
    char digits[8192], expanded[8196];
    const char *cursor = input;
    size_t count = 0, integer = 0, at = 0, index;
    long exponent = 0, point;
    int negative = 0, fractional = 0, exponent_negative = 0;
    if (*cursor == '-' || *cursor == '+') { negative = *cursor == '-'; cursor++; }
    while ((*cursor >= '0' && *cursor <= '9') || *cursor == '.') {
        if (*cursor == '.') { if (fractional) goto invalid; fractional = 1; cursor++; continue; }
        if (count + 1 >= sizeof(digits)) goto invalid;
        digits[count++] = *cursor++; if (!fractional) integer++;
    }
    if (!count || count > 8190) goto invalid;
    if (*cursor == 'e' || *cursor == 'E') {
        cursor++;
        if (*cursor == '-' || *cursor == '+') { exponent_negative = *cursor == '-'; cursor++; }
        if (*cursor < '0' || *cursor > '9') goto invalid;
        while (*cursor >= '0' && *cursor <= '9') {
            if (exponent > 8192) goto invalid;
            exponent = exponent * 10 + *cursor++ - '0';
        }
    }
    if (*cursor || exponent > 8192) goto invalid;
    if (exponent_negative) exponent = -exponent;
    point = (long)integer + exponent;
    if (point < -8190 || point > 8190 || (long)count - point > 8190) goto invalid;
    if (negative) expanded[at++] = '-';
    if (point <= 0) {
        expanded[at++] = '0'; expanded[at++] = '.';
        for (index = 0; index < (size_t)-point; index++) expanded[at++] = '0';
        memcpy(expanded + at, digits, count); at += count;
    } else {
        for (index = 0; index < (size_t)point; index++) expanded[at++] = index < count ? digits[index] : '0';
        if ((size_t)point < count) {
            expanded[at++] = '.'; memcpy(expanded + at, digits + point, count - (size_t)point); at += count - (size_t)point;
        }
    }
    if (at >= sizeof(expanded)) goto invalid;
    expanded[at] = 0;
    return zsharp_decimal_normalize(expanded, normalized, error, size);
invalid:
    snprintf(error, size, "foreign number is invalid or exceeds the 8190-digit expanded decimal limit"); return 0;
}
static JSValue interop_to_wire(JSContext *context, InteropDispatch *dispatch,
    const RuntimeValue *value, unsigned depth, char *error, size_t size) {
    JSValue wire = JS_UNDEFINED;
    size_t index, count = 0;
    if (depth > 32) goto unsupported;
    switch (value->type) {
        case ZVALUE_NULL: return JS_NULL;
        case ZVALUE_STATUS: return JS_NewBool(context, value->number != 0);
        case ZVALUE_TEXT: case ZVALUE_FUNCTION:
            return JS_NewString(context, value->text ? value->text : "");
        case ZVALUE_NUMBER:
            wire = JS_NewObject(context);
            JS_SetPropertyStr(context, wire, "$number", JS_NewString(context,
                value->number_text ? value->number_text : "0"));
            return wire;
        case ZVALUE_OBJECT: {
            RuntimeObject *object = value->object;
            InteropCapability *capability;
            if (!object) return JS_NULL;
            wire = JS_NewObject(context);
            if (object->foreign_language) {
                if (object->foreign_released || strcmp(object->foreign_language, dispatch->language) ||
                    strcmp(object->foreign_module, dispatch->module)) {
                    JS_FreeValue(context, wire);
                    snprintf(error, size, "foreign object belongs to another module or has been released");
                    return JS_EXCEPTION;
                }
                JS_SetPropertyStr(context, wire, "$ref", JS_NewString(context, object->foreign_ref));
                return wire;
            }
            if (!object->room) {
                JSValue record = JS_NewObject(context);
                for (index = 0; index < object->field_count; index++) {
                    RuntimeValue entry = runtime_field_value(&object->fields[index]);
                    JSValue field = interop_to_wire(context, dispatch, &entry, depth + 1, error, size);
                    if (JS_IsException(field)) { JS_FreeValue(context, record); JS_FreeValue(context, wire); return field; }
                    JS_DefinePropertyValueStr(context, record, object->fields[index].name, field,
                        JS_PROP_C_W_E);
                }
                JS_SetPropertyStr(context, wire, "$record", record); return wire;
            }
            for (capability = dispatch->state->capabilities; capability; capability = capability->next)
                if (capability->object == object && !strcmp(capability->language, dispatch->language) &&
                    !strcmp(capability->module, dispatch->module)) break;
            if (!capability) {
                capability = calloc(1, sizeof(*capability));
                if (!capability) { JS_FreeValue(context, wire); goto unsupported; }
                capability->language = zsharp_copy_text(dispatch->language, strlen(dispatch->language));
                capability->module = zsharp_copy_text(dispatch->module, strlen(dispatch->module));
                if (!capability->language || !capability->module) {
                    free(capability->language); free(capability->module); free(capability);
                    JS_FreeValue(context, wire); goto unsupported;
                }
                snprintf(capability->id, sizeof(capability->id), "z%zu", ++dispatch->state->next_capability);
                capability->object = object;
                capability->owner_heap = dispatch->frame->heap;
                capability->owner_program = dispatch->frame->program;
                capability->owner_root = dispatch->frame->root;
                {
                    size_t module;
                    for (module = 0; module < dispatch->frame->cache->module_count; module++) {
                        RuntimeModule *candidate = &dispatch->frame->cache->modules[module];
                        size_t room_index;
                        for (room_index = 0; candidate->program && room_index < candidate->program->room_count; room_index++)
                            if (&candidate->program->rooms[room_index] == object->room) {
                                capability->owner_program = candidate->program;
                                capability->owner_root = candidate->project_root;
                            }
                    }
                }
                capability->next = dispatch->state->capabilities;
                dispatch->state->capabilities = capability;
            }
            JS_SetPropertyStr(context, wire, "$zref", JS_NewString(context, capability->id));
            JS_SetPropertyStr(context, wire, "$type", JS_NewString(context, object->room ? object->room->name : "record"));
            return wire;
        }
        case ZVALUE_TEXT_ARRAY: count = value->array ? value->array->text_item_count : 0; break;
        case ZVALUE_NUMBER_ARRAY: count = value->array ? value->array->number_item_count : 0; break;
        case ZVALUE_OBJECT_ARRAY: count = value->array ? value->array->object_item_count : 0; break;
        default: goto unsupported;
    }
    wire = JS_NewArray(context);
    for (index = 0; index < count; index++) {
        RuntimeValue item = {0}; JSValue next;
        item.type = value->type == ZVALUE_TEXT_ARRAY ? ZVALUE_TEXT :
            value->type == ZVALUE_NUMBER_ARRAY ? ZVALUE_NUMBER : ZVALUE_OBJECT;
        if (item.type == ZVALUE_TEXT) item.text = value->array->text_items[index];
        else if (item.type == ZVALUE_NUMBER) item.number_text = value->array->number_items[index];
        else item.object = value->array->object_items[index].runtime_object;
        next = interop_to_wire(context, dispatch, &item, depth + 1, error, size);
        if (JS_IsException(next)) { JS_FreeValue(context, wire); return next; }
        JS_SetPropertyUint32(context, wire, (uint32_t)index, next);
    }
    return wire;
unsupported:
    snprintf(error, size, "value cannot cross the foreign interface (maximum depth 32)");
    return JS_EXCEPTION;
}
static int interop_from_wire(JSContext *context, InteropDispatch *dispatch, JSValueConst wire,
    RuntimeValue *value, unsigned depth, char *error, size_t size) {
    char text[8192];
    RuntimeHeap *heap = dispatch->frame->heap;
    memset(value, 0, sizeof(*value));
    if (depth > 32) goto invalid;
    if (JS_IsNull(wire)) { value->type = ZVALUE_NULL; return 1; }
    if (JS_IsBool(wire)) { value->type = ZVALUE_STATUS; value->number = JS_ToBool(context, wire); return 1; }
    if (JS_IsString(wire)) {
        size_t length; const char *string = JS_ToCStringLen(context, &length, wire);
        if (!string || memchr(string, 0, length)) { if (string) JS_FreeCString(context, string); goto invalid; }
        value->text = interop_text(heap, string); JS_FreeCString(context, string);
        value->type = ZVALUE_TEXT; return value->text != NULL;
    }
    if (JS_IsNumber(wire)) {
        snprintf(error, size, "foreign numbers must use the exact $number decimal-text wire format");
        return 0;
    }
    if (!JS_IsObject(wire)) goto invalid;
    {
        const char *tags[] = {"$number", "$ref", "$zref", "$record"};
        size_t index; unsigned present = 0;
        for (index = 0; index < sizeof(tags) / sizeof(tags[0]); index++) {
            JSValue tagged = JS_GetPropertyStr(context, wire, tags[index]);
            if (!JS_IsUndefined(tagged)) present++;
            JS_FreeValue(context, tagged);
        }
        if (present > 1) { snprintf(error, size, "foreign value contains conflicting wire type tags"); return 0; }
    }
    if (interop_string_property(context, wire, "$number", text, sizeof(text))) {
        char *normalized = NULL;
        if (!interop_decimal(text, &normalized, error, size)) return 0;
        value->type = ZVALUE_NUMBER; value->number_text = heap_add_text(heap, normalized);
        return value->number_text != NULL;
    }
    if (interop_string_property(context, wire, "$ref", text, sizeof(text))) {
        RuntimeObject *object = NULL; size_t index;
        if (!*text) goto invalid;
        for (index = 0; index < heap->object_count; index++) {
            RuntimeObject *candidate = heap->objects[index];
            if (candidate->foreign_language && !strcmp(candidate->foreign_language, dispatch->language) &&
                !strcmp(candidate->foreign_module, dispatch->module) && !strcmp(candidate->foreign_ref, text)) {
                object = candidate; break;
            }
        }
        if (!object) {
            object = calloc(1, sizeof(*object));
            if (!object) goto invalid;
            object->foreign_language = interop_text(heap, dispatch->language);
            object->foreign_module = interop_text(heap, dispatch->module);
            object->foreign_ref = interop_text(heap, text);
            if (!object->foreign_language || !object->foreign_module || !object->foreign_ref) { free(object); goto invalid; }
            if (!heap_add_object(heap, object)) goto invalid;
        }
        if (object->foreign_released) { snprintf(error, size, "foreign object handle has been released"); return 0; }
        value->type = ZVALUE_OBJECT; value->object = object; return 1;
    }
    if (interop_string_property(context, wire, "$zref", text, sizeof(text))) {
        InteropCapability *capability;
        for (capability = dispatch->state->capabilities; capability; capability = capability->next)
            if (!strcmp(capability->id, text) && !strcmp(capability->language, dispatch->language) &&
                !strcmp(capability->module, dispatch->module)) {
                value->type = ZVALUE_OBJECT; value->object = capability->object; return 1;
            }
        snprintf(error, size, "unknown or unauthorized Z# object handle"); return 0;
    }
    if (JS_IsArray(wire)) {
        JSValue length = JS_GetPropertyStr(context, wire, "length");
        uint32_t count = 0, index; ZSharpVariable *array = calloc(1, sizeof(*array));
        JS_ToUint32(context, &count, length); JS_FreeValue(context, length);
        if (!array || count > 65536) { free(array); goto invalid; }
        for (index = 0; index < count; index++) {
            RuntimeValue item; JSValue entry = JS_GetPropertyUint32(context, wire, index);
            int ok = interop_from_wire(context, dispatch, entry, &item, depth + 1, error, size);
            JS_FreeValue(context, entry);
            if (!ok || (item.type != ZVALUE_TEXT && item.type != ZVALUE_NUMBER && item.type != ZVALUE_OBJECT)) goto array_failed;
            if (!index) {
                array->type = item.type == ZVALUE_TEXT ? ZVALUE_TEXT_ARRAY :
                    item.type == ZVALUE_NUMBER ? ZVALUE_NUMBER_ARRAY : ZVALUE_OBJECT_ARRAY;
                if (array->type == ZVALUE_TEXT_ARRAY) array->text_items = calloc(count, sizeof(char *));
                else if (array->type == ZVALUE_NUMBER_ARRAY) array->number_items = calloc(count, sizeof(char *));
                else array->object_items = calloc(count, sizeof(*array->object_items));
                if (!array->text_items && !array->number_items && !array->object_items) goto array_failed;
            }
            if (array->type == ZVALUE_TEXT_ARRAY && item.type == ZVALUE_TEXT) {
                array->text_items[index] = zsharp_copy_text(item.text, strlen(item.text));
                array->text_item_count++; if (!array->text_items[index]) goto array_failed;
            } else if (array->type == ZVALUE_NUMBER_ARRAY && item.type == ZVALUE_NUMBER) {
                array->number_items[index] = zsharp_copy_text(item.number_text, strlen(item.number_text));
                array->number_item_count++; if (!array->number_items[index]) goto array_failed;
            } else if (array->type == ZVALUE_OBJECT_ARRAY && item.type == ZVALUE_OBJECT) {
                array->object_items[index].runtime_object = item.object; array->object_item_count++;
            } else goto array_failed;
        }
        if (!count) array->type = ZVALUE_TEXT_ARRAY;
        value->type = array->type; value->array = heap_add_array(heap, array);
        return value->array != NULL;
array_failed:
        free(array->object_items); array->object_items = NULL;
        free_runtime_array(array);
        snprintf(error, size, "foreign arrays must contain only text, numbers, or objects of one value kind"); return 0;
    }
    {
        JSValue record = JS_GetPropertyStr(context, wire, "$record");
        JSPropertyEnum *properties = NULL; uint32_t count = 0, index;
        RuntimeObject *object;
        if (!JS_IsObject(record) || JS_GetOwnPropertyNames(context, &properties, &count,
                record, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0 || count > 65536) {
            JS_FreeValue(context, record); goto invalid;
        }
        object = calloc(1, sizeof(*object));
        if (!object || !(object->fields = calloc(count ? count : 1, sizeof(*object->fields)))) {
            free(object); JS_FreePropertyEnum(context, properties, count); JS_FreeValue(context, record); goto invalid;
        }
        if (!heap_add_object(heap, object)) {
            JS_FreePropertyEnum(context, properties, count); JS_FreeValue(context, record); goto invalid;
        }
        for (index = 0; index < count; index++) {
            const char *name = JS_AtomToCString(context, properties[index].atom);
            JSValue entry = JS_GetProperty(context, record, properties[index].atom);
            RuntimeField *field = &object->fields[index];
            field->name = name ? interop_text(heap, name) : NULL;
            if (name) JS_FreeCString(context, name);
            if (!field->name || !interop_from_wire(context, dispatch, entry, &field->value, depth + 1, error, size)) {
                JS_FreeValue(context, entry); JS_FreePropertyEnum(context, properties, count);
                JS_FreeValue(context, record); return 0;
            }
            field->type = field->value.type; object->field_count++;
            JS_FreeValue(context, entry);
        }
        JS_FreePropertyEnum(context, properties, count); JS_FreeValue(context, record);
        value->type = ZVALUE_OBJECT; value->object = object; return 1;
    }
invalid:
    snprintf(error, size, "foreign runtime returned an invalid or excessively nested value"); return 0;
}
static int interop_cancelled(void *context) {
    InteropVMFrame *frame = context;
    const ZSharpWindowRuntime *window = frame->cache->window_runtime;
    return window && window->is_cancelled && window->is_cancelled(window->state);
}
static void interop_executable_directory(char *path, size_t size) {
    char *separator;
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, path, (DWORD)size)) path[0] = 0;
    separator = strrchr(path, '\\');
#elif defined(__APPLE__)
    uint32_t length = (uint32_t)size;
    if (_NSGetExecutablePath(path, &length)) path[0] = 0;
    separator = strrchr(path, '/');
#else
    ssize_t length = readlink("/proc/self/exe", path, size - 1);
    if (length > 0) path[length] = 0; else path[0] = 0;
    separator = strrchr(path, '/');
#endif
    if (separator) *separator = 0;
}
static int interop_helper(const char *relative, char *path, size_t size, char *error, size_t error_size) {
    const char *override = getenv("ZSHARP_INTEROP_ROOT");
    char executable[2048]; const char *candidates[] = {"interop", "../share/zsharp/interop", "../../native/interop"};
    size_t index;
    if (override && *override) {
        snprintf(path, size, "%s/%s", override, relative);
        if (runtime_file_exists(path)) return 1;
        snprintf(error, error_size, "ZSHARP_INTEROP_ROOT is missing helper '%s'", relative); return 0;
    }
    interop_executable_directory(executable, sizeof(executable));
    for (index = 0; index < sizeof(candidates)/sizeof(candidates[0]); index++) {
        snprintf(path, size, "%s/%s/%s", executable, candidates[index], relative);
        if (runtime_file_exists(path)) return 1;
    }
    snprintf(error, error_size, "interoperability helper '%s' is not installed; rebuild/install the 1.4 helpers", relative);
    return 0;
}
static InteropSession *interop_session(InteropDispatch *dispatch, char *error, size_t size) {
    InteropSession *session;
    const char *language = dispatch->language;
    const char *key = !strcmp(language, "go") ? dispatch->module : language;
    const char *argv[6] = {0}; char helper[4096], runtime[2048] = {0}, old_bridge[2048] = {0};
    for (session = dispatch->state->sessions; session; session = session->next)
        if (!strcmp(session->key, key)) return session;
    if (!strcmp(language, "go")) argv[0] = dispatch->module;
    else if (!strcmp(language, "java")) {
        if (!interop_helper("java/zsharp-java-worker.jar", helper, sizeof(helper), error, size)) return NULL;
        argv[0] = getenv("ZSHARP_JAVA"); if (!argv[0] || !*argv[0]) argv[0] = "java";
        argv[1] = "-cp"; argv[2] = helper; argv[3] = "ZSharp.Worker";
    } else if (!strcmp(language, "cs")) {
        if (!interop_helper("csharp/ZSharp.Interop.Worker.dll", helper, sizeof(helper), error, size)) return NULL;
        argv[0] = getenv("ZSHARP_DOTNET"); if (!argv[0] || !*argv[0]) argv[0] = "dotnet";
        argv[1] = helper;
    } else if (!strcmp(language, "rb")) {
        if (!interop_helper("ruby/zsharp_worker.rb", helper, sizeof(helper), error, size)) return NULL;
        argv[0] = getenv("ZSHARP_RUBY"); if (!argv[0] || !*argv[0]) argv[0] = "ruby";
        argv[1] = helper;
    } else if (!strcmp(language, "py")) {
        if (!interop_helper("python/zsharp_worker.py", helper, sizeof(helper), error, size)) return NULL;
        if (!python_runtime_paths(runtime, sizeof(runtime), old_bridge, sizeof(old_bridge), error, size)) {
            snprintf(error, size, "Python runtime is not installed"); return NULL;
        }
        argv[0] = runtime; argv[1] = "-u"; argv[2] = helper;
    } else { snprintf(error, size, "unknown foreign runtime '%s'", language); return NULL; }
    session = calloc(1, sizeof(*session));
    if (!session) { snprintf(error, size, "out of memory"); return NULL; }
    session->key = zsharp_copy_text(key, strlen(key));
    session->process = session->key ? zsharp_interop_process_start(argv, dispatch->frame->root, error, size) : NULL;
    if (!session->process) { free(session->key); free(session); return NULL; }
    session->next = dispatch->state->sessions; dispatch->state->sessions = session;
    return session;
}
static int interop_process_request(JSContext *context, InteropDispatch *dispatch,
    const char *operation, const char *target, const char *member, const char *handle,
    JSValueConst arguments, JSValue *result, char *error, size_t size) {
    InteropSession *session = interop_session(dispatch, error, size);
    JSValue request; char *line = NULL; int ok = 0; unsigned id;
    if (!session) return 0;
    if (session->failed) { snprintf(error, size, "foreign worker protocol failed earlier; restart the application (calls are not replayed)"); return 0; }
    if (session->busy) { snprintf(error, size, "cannot re-enter the same active foreign worker; return from its callback first"); return 0; }
    session->busy = 1; id = ++session->serial;
    request = JS_NewObject(context);
    JS_SetPropertyStr(context, request, "id", JS_NewInt64(context, id));
    JS_SetPropertyStr(context, request, "op", JS_NewString(context, operation));
    JS_SetPropertyStr(context, request, "module", JS_NewString(context, dispatch->module));
    JS_SetPropertyStr(context, request, "target", JS_NewString(context, target ? target : ""));
    JS_SetPropertyStr(context, request, "member", JS_NewString(context, member ? member : ""));
    JS_SetPropertyStr(context, request, "ref", JS_NewString(context, handle ? handle : ""));
    JS_SetPropertyStr(context, request, "args", JS_DupValue(context, arguments));
    line = interop_json_string(context, request); JS_FreeValue(context, request);
    if (!line || !zsharp_interop_process_write(session->process, line, 30000, interop_cancelled, dispatch->frame, error, size)) goto done;
    free(line); line = NULL;
    for (;;) {
        JSValue response, field; char op[64] = {0}; int64_t received_id = 0;
        line = zsharp_interop_process_read(session->process, 30000, interop_cancelled, dispatch->frame, error, size);
        if (!line) goto done;
        response = JS_ParseJSON(context, line, strlen(line), "<foreign response>"); free(line); line = NULL;
        if (JS_IsException(response) || !interop_string_property(context, response, "op", op, sizeof(op))) {
            JS_FreeValue(context, response); session->failed = 1;
            snprintf(error, size, "foreign worker returned an invalid response"); goto done;
        }
        field = JS_GetPropertyStr(context, response, "id");
        JS_ToInt64(context, &received_id, field); JS_FreeValue(context, field);
        if (!strncmp(op, "host.", 5)) {
            char callback_target[8192] = {0}, callback_member[1024] = {0}, callback_handle[1024] = {0};
            char callback_error[2048] = {0}, *args_json, *returned = NULL;
            JSValue args = JS_GetPropertyStr(context, response, "args"), reply, value;
            int success;
            interop_string_property(context, response, "target", callback_target, sizeof(callback_target));
            interop_string_property(context, response, "member", callback_member, sizeof(callback_member));
            interop_string_property(context, response, "ref", callback_handle, sizeof(callback_handle));
            args_json = interop_json_string(context, args); JS_FreeValue(context, args);
            success = args_json && interop_host_dispatch(dispatch, op + 5, callback_target, callback_member,
                callback_handle, args_json, &returned, callback_error, sizeof(callback_error));
            free(args_json);
            reply = JS_NewObject(context);
            JS_SetPropertyStr(context, reply, "id", JS_GetPropertyStr(context, response, "id"));
            JS_SetPropertyStr(context, reply, "op", JS_NewString(context, "result"));
            JS_SetPropertyStr(context, reply, "ok", JS_NewBool(context, success));
            if (success) {
                value = JS_ParseJSON(context, returned, strlen(returned), "<host result>");
                JS_SetPropertyStr(context, reply, "value", value);
            } else JS_SetPropertyStr(context, reply, "error", JS_NewString(context, *callback_error ? callback_error : "host callback failed"));
            free(returned); JS_FreeValue(context, response);
            line = interop_json_string(context, reply); JS_FreeValue(context, reply);
            if (!line || !zsharp_interop_process_write(session->process, line, 30000, interop_cancelled, dispatch->frame, error, size)) goto done;
            free(line); line = NULL; continue;
        }
        if (strcmp(op, "result") || received_id != id) {
            JS_FreeValue(context, response); session->failed = 1;
            snprintf(error, size, "foreign worker response ID mismatch"); goto done;
        }
        field = JS_GetPropertyStr(context, response, "ok"); ok = JS_IsBool(field) && JS_ToBool(context, field); JS_FreeValue(context, field);
        if (ok) *result = JS_GetPropertyStr(context, response, "value");
        else if (!interop_string_property(context, response, "error", error, size)) snprintf(error, size, "foreign function failed");
        JS_FreeValue(context, response); break;
    }
done:
    if (session->failed) {
        zsharp_interop_process_destroy(session->process); session->process = NULL;
    }
    free(line); session->busy = 0; return ok;
}
static int interop_request(const char *language, const char *module, const char *operation,
    const char *target, const char *member, const char *handle, const RuntimeValue *arguments,
    size_t count, RuntimeValue *result, char *error, size_t size) {
    InteropVMFrame *frame = interop_current_frame;
    InteropVMState *state = interop_state(frame->cache, error, size);
    InteropDispatch dispatch = {frame, state, language, module};
    JSRuntime *runtime; JSContext *context; JSValue args, wire = JS_UNDEFINED;
    size_t index; int ok = 0, released = 0; char *args_json = NULL, *returned = NULL;
    if (!state) return 0;
    /* Never wait for the interop lock while owning a window task's room lock:
     * an active foreign callback may itself be waiting to resume that task. */
    if (!cooperative_window_suspend(frame->cache, &released, error, size)) return 0;
    interop_lock(state);
    if (!cooperative_window_resume(frame->cache, frame->heap, released, error, size)) {
        interop_unlock(state); return 0;
    }
    for (index = 0; index < state->legacy_count; index++) {
        if (!strcmp(state->legacy_modules[index], module)) {
            interop_unlock(state);
            if (!strcmp(operation, "call")) return -1;
            snprintf(error, size, "foreign objects require a module built with the Z# interop v2 interface"); return 0;
        }
    }
    runtime = JS_NewRuntime(); context = runtime ? JS_NewContext(runtime) : NULL;
    if (!context) { if (runtime) JS_FreeRuntime(runtime); interop_unlock(state); snprintf(error, size, "out of memory"); return 0; }
    JS_SetMemoryLimit(runtime, 32 * 1024 * 1024);
    args = JS_NewArray(context);
    for (index = 0; index < count; index++) {
        JSValue next = interop_to_wire(context, &dispatch, &arguments[index], 0, error, size);
        if (JS_IsException(next)) goto done;
        JS_SetPropertyUint32(context, args, (uint32_t)index, next);
    }
    if (!strcmp(language, "c") || !strcmp(language, "cpp") || !strcmp(language, "rust") || !strcmp(language, "kt") ||
        !strcmp(language, "js") || !strcmp(language, "lua")) {
        args_json = interop_json_string(context, args);
        if (!args_json) goto done;
        if (!strcmp(language, "js") || !strcmp(language, "lua"))
            ok = zsharp_script_interop_request(&state->script, language, module, operation, target, member, handle,
                args_json, interop_host_dispatch, &dispatch, &returned, error, size);
        else ok = zsharp_embedded_interop_request(&state->embedded, language, module, operation, target, member, handle,
                args_json, interop_host_dispatch, &dispatch, &returned, error, size);
        if (ok == -1) {
            char *copy = zsharp_copy_text(module, strlen(module));
            char **resized = copy ? realloc(state->legacy_modules, (state->legacy_count + 1) * sizeof(*resized)) : NULL;
            if (resized) { state->legacy_modules = resized; resized[state->legacy_count++] = copy; }
            else free(copy);
        }
        if (ok == -1 && strcmp(operation, "call")) {
            snprintf(error, size, "foreign objects require a module built with the Z# interop v2 interface"); ok = 0;
        }
        if (ok != 1) goto done;
        wire = JS_ParseJSON(context, returned, strlen(returned), "<foreign result>");
    } else if (!interop_process_request(context, &dispatch, operation, target, member, handle, args, &wire, error, size)) goto done;
    ok = interop_from_wire(context, &dispatch, wire, result, 0, error, size);
done:
    free(args_json); free(returned); JS_FreeValue(context, wire); JS_FreeValue(context, args);
    JS_FreeContext(context); JS_FreeRuntime(runtime); interop_unlock(state);
    return ok;
}
static int interop_member_get(RuntimeObject *object, const char *member, RuntimeValue *value, char *error, size_t size) {
    RuntimeField *field;
    if (object->foreign_language) {
        if (object->foreign_released) { snprintf(error, size, "foreign object has been released"); return 0; }
        return interop_request(object->foreign_language, object->foreign_module, "get", "", member,
            object->foreign_ref, NULL, 0, value, error, size) == 1;
    }
    field = find_field(object, member);
    if (!field || (field->definition && !field->definition->is_public && object->room != interop_current_frame->room)) {
        snprintf(error, size, "object has no visible field '%s'", member); return 0;
    }
    *value = runtime_field_value(field); return 1;
}
static int interop_member_set(RuntimeObject *object, const char *member, const RuntimeValue *value, char *error, size_t size) {
    RuntimeField *field; RuntimeValue ignored;
    if (object->foreign_language) {
        if (object->foreign_released) { snprintf(error, size, "foreign object has been released"); return 0; }
        return interop_request(object->foreign_language, object->foreign_module, "set", "", member,
            object->foreign_ref, value, 1, &ignored, error, size) == 1;
    }
    field = find_field(object, member);
    if (!field || (field->definition && !field->definition->is_public && object->room != interop_current_frame->room)) {
        snprintf(error, size, "object has no visible field '%s'", member); return 0;
    }
    return runtime_field_assign(field, value, error, size);
}
static int interop_object_invoke(RuntimeObject *object, const char *method, const RuntimeValue *args,
    size_t count, RuntimeValue *result, char *error, size_t size) {
    int release = !strcmp(method, "$foreign.release"); int ok;
    if (object->foreign_released) { snprintf(error, size, "foreign object has been released"); return 0; }
    ok = interop_request(object->foreign_language, object->foreign_module,
        release ? "release" : "invoke", "", release ? "" : method, object->foreign_ref, args, count, result, error, size);
    if (ok == 1 && release) object->foreign_released = 1;
    return ok == 1;
}
static int interop_local_set(ZSharpRoom *room, RuntimeObject *current, LocalValue *locals,
    size_t count, const char *path, const RuntimeValue *value, char *error, size_t size) {
    char *storage = zsharp_copy_text(path, strlen(path)), *member, *next;
    RuntimeValue base; int ok;
    if (!storage) { snprintf(error, size, "out of memory"); return -1; }
    member = strchr(storage, '.');
    if (!member) { free(storage); return 0; }
    *member++ = 0;
    if (!lookup_name(room, current, locals, count, storage, &base) || base.type != ZVALUE_OBJECT || !base.object) {
        free(storage); return 0;
    }
    while ((next = strchr(member, '.')) != NULL) {
        *next++ = 0;
        if (!interop_member_get(base.object, member, &base, error, size) || base.type != ZVALUE_OBJECT || !base.object) {
            free(storage); return -1;
        }
        member = next;
    }
    ok = interop_member_set(base.object, member, value, error, size);
    free(storage); return ok ? 1 : -1;
}
static int interop_qualified_call(const ZSharpInstruction *instruction, const RuntimeValue *args,
    const char *root, RuntimeValue *result, char *error, size_t size) {
    const char *language = instruction->operand + 1;
    const char *suffix, *module_name = instruction->call_file;
    char relative[2048], module[4096]; size_t index, pid_length;
    if (!strcmp(language, "py") && !strcmp(module_name, "@zsharp-network")) return -1;
    if (!strcmp(language, "java")) suffix = ".zjava.jar";
    else if (!strcmp(language, "cs")) suffix = ".zcs.dll";
    else if (!strcmp(language, "go")) suffix =
#ifdef _WIN32
        ".zgo.exe";
#else
        ".zgo";
#endif
    else if (!strcmp(language, "rb")) suffix = ".rb";
    else if (!strcmp(language, "py")) suffix = ".py";
    else if (!strcmp(language, "js")) suffix = ".js";
    else if (!strcmp(language, "lua")) suffix = ".lua";
    else if (!strcmp(language, "c")) suffix = ".zc";
    else if (!strcmp(language, "cpp")) suffix = ".zcpp";
    else if (!strcmp(language, "kt")) suffix = ".zkt";
    else if (!strcmp(language, "rust")) suffix = ".zrust";
    else { snprintf(error, size, "unknown foreign language '%s'", language); return 0; }
    pid_length = interop_current_frame->program->project_id ? strlen(interop_current_frame->program->project_id) : 0;
    if (pid_length && !strncmp(module_name, interop_current_frame->program->project_id, pid_length) && module_name[pid_length] == '.')
        module_name += pid_length + 1;
    if (strlen(module_name) >= sizeof(relative)) { snprintf(error, size, "foreign module name is too long"); return 0; }
    strcpy(relative, module_name);
    for (index = 0; relative[index]; index++) if (relative[index] == '.') relative[index] = '/';
    snprintf(module, sizeof(module), "%s/%s%s%s", root, relative, suffix,
        (!strcmp(language, "c") || !strcmp(language, "cpp") || !strcmp(language, "kt") || !strcmp(language, "rust")) ?
#ifdef _WIN32
        ".dll"
#elif defined(__APPLE__)
        ".dylib"
#else
        ".so"
#endif
        : "");
    return interop_request(language, module, instruction->call_room && !strcmp(instruction->call_room, "$new") ? "new" : "call",
        instruction->call_function, "", "", args, instruction->argument_count, result, error, size);
}
static int interop_host_dispatch(void *context, const char *operation, const char *target,
    const char *member, const char *handle, const char *args_json, char **result_json, char *error, size_t size) {
    InteropDispatch *dispatch = context; InteropVMFrame *frame = dispatch->frame;
    JSRuntime *runtime = JS_NewRuntime(); JSContext *js = runtime ? JS_NewContext(runtime) : NULL;
    JSValue wire = JS_UNDEFINED, returned = JS_UNDEFINED;
    RuntimeValue *args = NULL, result = {0}; uint32_t count = 0, index; int ok = 0;
    if (!strncmp(operation, "host.", 5)) operation += 5;
    if (!js) { if (runtime) JS_FreeRuntime(runtime); snprintf(error, size, "out of memory"); return 0; }
    JS_SetMemoryLimit(runtime, 32 * 1024 * 1024);
    wire = JS_ParseJSON(js, args_json, strlen(args_json), "<foreign callback arguments>");
    if (!JS_IsArray(wire)) { snprintf(error, size, "callback arguments must be an array"); goto done; }
    { JSValue length = JS_GetPropertyStr(js, wire, "length"); JS_ToUint32(js, &count, length); JS_FreeValue(js, length); }
    if (count > 1024 || !(args = calloc(count ? count : 1, sizeof(*args)))) { snprintf(error, size, "too many callback arguments"); goto done; }
    for (index = 0; index < count; index++) {
        JSValue item = JS_GetPropertyUint32(js, wire, index);
        int decoded = interop_from_wire(js, dispatch, item, &args[index], 0, error, size);
        JS_FreeValue(js, item); if (!decoded) goto done;
    }
    if (!strcmp(operation, "call")) {
        ZSharpInstruction decoded = {0}, *instructions = NULL;
        ZSharpParameter *parameters = NULL; ZSharpFunction wrapper = {0};
        ZSharpSettings settings; ZSharpDiagnostic diagnostic;
        char storage[8192], (*names)[32] = NULL; int did_return = 0;
        if (!zsharp_call_target_decode(target, &decoded, storage, sizeof(storage), error, size)) goto done;
        if (decoded.operand && decoded.operand[0] == '@') { snprintf(error, size, "ZSharp.call requires a Z# function target"); goto done; }
        zsharp_settings_init(&settings);
        if (!zsharp_settings_load(frame->root, &settings, &diagnostic, error, size)) { zsharp_settings_free(&settings); goto done; }
        ok = zsharp_project_authorize_call(frame->program, &settings, frame->room, &decoded, error, size);
        zsharp_settings_free(&settings); if (!ok) goto done;
        ok = 0;
        parameters = calloc(count ? count : 1, sizeof(*parameters));
        instructions = calloc((size_t)count + 2, sizeof(*instructions)); names = calloc(count ? count : 1, sizeof(*names));
        if (!parameters || !instructions || !names) {
            free(parameters); free(instructions); free(names); snprintf(error, size, "out of memory"); goto done;
        }
        for (index = 0; index < count; index++) {
            snprintf(names[index], 32, "arg%u", index);
            parameters[index].name = names[index]; parameters[index].type = args[index].type;
            instructions[index].op = ZOP_LOAD_NAME; instructions[index].operand = names[index];
        }
        decoded.op = ZOP_CALL_QUALIFIED_VALUE; decoded.argument_count = count;
        instructions[count] = decoded; instructions[count + 1].op = ZOP_RETURN_VALUE;
        wrapper.name = "<foreign callback>"; wrapper.parameters = parameters; wrapper.parameter_count = count;
        wrapper.instructions = instructions; wrapper.instruction_count = count + 2;
        {
            InteropVMFrame callback_context = *frame;
            InteropVMFrame *previous = interop_current_frame;
            callback_context.callback_wrapper = &wrapper;
            interop_current_frame = &callback_context;
            ok = execute_function(frame->program, frame->room, &wrapper, NULL, frame->heap, frame->root,
                frame->providers, frame->provider_count, frame->cache, frame->depth + 1, args, count,
                NULL, &result, &did_return, error, size);
            interop_current_frame = previous;
        }
        free(parameters); free(instructions); free(names);
        if (!ok) goto done;
        if (!did_return) result.type = ZVALUE_NULL;
    } else {
        InteropCapability *capability; RuntimeObject *object = NULL;
        for (capability = dispatch->state->capabilities; capability; capability = capability->next)
            if (!strcmp(capability->id, handle) && !strcmp(capability->language, dispatch->language) &&
                !strcmp(capability->module, dispatch->module)) { object = capability->object; break; }
        if (!object) { snprintf(error, size, "unknown or unauthorized Z# object handle"); goto done; }
        if (!strcmp(operation, "get") && count == 0) ok = interop_member_get(object, member, &result, error, size);
        else if (!strcmp(operation, "set") && count == 1) {
            ok = interop_member_set(object, member, &args[0], error, size); result.type = ZVALUE_NULL;
        } else if (!strcmp(operation, "invoke") && object->room) {
            ZSharpFunction *function = find_function(object->room, member);
            int did_return = 0; ZSharpProgram *owner = capability->owner_program;
            if (!function || function->is_horde || (!function->is_public && frame->room != object->room)) {
                snprintf(error, size, "Z# object has no visible method '%s'", member); goto done;
            }
            ok = execute_function(owner, object->room, function, object, frame->heap, capability->owner_root,
                frame->providers, frame->provider_count, frame->cache, frame->depth + 1, args, count,
                NULL, &result, &did_return, error, size);
            if (ok && !did_return) result.type = ZVALUE_NULL;
        } else snprintf(error, size, "invalid Z# object callback operation or argument count");
        if (!ok) goto done;
    }
    returned = interop_to_wire(js, dispatch, &result, 0, error, size);
    if (JS_IsException(returned)) { ok = 0; goto done; }
    *result_json = interop_json_string(js, returned); ok = *result_json != NULL;
done:
    free(args); JS_FreeValue(js, wire); JS_FreeValue(js, returned); JS_FreeContext(js); JS_FreeRuntime(runtime);
    return ok;
}
