/* Experimental browser VM: executes canonical parser bytecode, not generated JS. */
#include "parser.h"
#include "settings.h"
#include "zsharp.h"
#include "decimal.h"
#include "quickjs.h"
#include "time_api.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

__attribute__((import_module("bzvm"), import_name("read")))
extern int host_read(const char *, char *, int, unsigned, const char *);
__attribute__((import_module("bzvm"), import_name("input_session")))
extern void host_input_session(unsigned, int);
__attribute__((import_module("bzvm"), import_name("write")))
extern int host_write(const char *, const char *);
__attribute__((import_module("bzvm"), import_name("network")))
extern int host_network(const char *,const char *,char *,int);
__attribute__((import_module("bzvm"), import_name("bind")))
extern int host_bind(const char *, const char *);
__attribute__((import_module("bzvm"), import_name("raw")))
extern int host_raw(int, const char *);
__attribute__((import_module("bzvm"), import_name("print")))
extern void host_print(const char *);
__attribute__((import_module("bzvm"), import_name("schedule")))
extern int host_schedule(unsigned, double);
__attribute__((import_module("bzvm"), import_name("now")))
extern double host_now(void);
__attribute__((import_module("bzvm"), import_name("wall_time")))
extern double host_wall_time(void);
__attribute__((import_module("bzvm"), import_name("local_time")))
extern double host_local_time(double, int);
__attribute__((import_module("bzvm"), import_name("convert_local_time")))
extern double host_convert_local_time(int, int, int, int, int, int);

static int browser_now(int64_t *ms) {
    double n=host_wall_time();
    if(!isfinite(n) || n < -62135596800000.0 || n > 253402300799999.0)return 0;
    *ms=(int64_t)n;return 1;
}
static int browser_local(int64_t ms,int fields[7]) {
    int i;for(i=0;i<7;i++){double n=host_local_time((double)ms,i);if(!isfinite(n))return 0;fields[i]=(int)n;}return 1;
}
static int browser_convert(const int f[7],int64_t *ms) {
    double n=host_convert_local_time(f[0],f[1],f[2],f[3],f[4],f[5]);
    if(!isfinite(n) || n < -62135596800000.0 || n > 253402300799999.0)return 0;
    *ms=(int64_t)n;return 1;
}
static const ZSharpTimeHost time_host={browser_now,browser_local,browser_convert};

typedef struct Array Array;
typedef struct Object Object;
typedef struct Value { int type; char *text; double number; Array *array; Object *object; } Value;
struct Array { unsigned references; size_t count; Value *items; };
struct Object { unsigned references; size_t count; const ZSharpJsonSchema *schema; Value *fields; };
typedef struct Global { ZSharpVariable *definition; Value value; struct Global *next; } Global;
static Global *globals;
typedef struct Local { const char *name; Value value; } Local;
typedef struct Frame {
    ZSharpProgram *owner;
    ZSharpRoom *room; ZSharpFunction *function;
    Value *stack; Local *locals; size_t sp, nl, pc;
    int waiting_return;
    char *outcome;
    struct Frame *next;
} Frame;
typedef struct Task {
    unsigned id; double delay; Frame *head, *tail; struct Task *next;
    Value returned; int has_return;
} Task;
static Task *tasks, *active;
static unsigned next_task_id;
static ZSharpProgram modules[64];
static size_t module_count;
static ZSharpProgram *current_program = &modules[0];
#define program (*current_program)
static int initialized;
static char failure[512];
static unsigned budget;
static unsigned depth;
static ZSharpSettings settings;
static int has_settings;
static void drop(Value *v) { free(v->text);
    if(v->array && !--v->array->references){size_t i;for(i=0;i<v->array->count;i++)drop(&v->array->items[i]);free(v->array->items);free(v->array);}
    if(v->object && !--v->object->references){size_t i;for(i=0;i<v->object->count;i++)drop(&v->object->fields[i]);free(v->object->fields);free(v->object);}
    memset(v,0,sizeof(*v)); }
static Value copy(Value v) { if (v.text) v.text = zsharp_copy_text(v.text,strlen(v.text));if(v.array)v.array->references++;if(v.object)v.object->references++; return v; }
static Value text(const char *s) { Value v = {ZVALUE_TEXT,NULL,0}; v.text = zsharp_copy_text(s,strlen(s)); return v; }
static Value number(const char *s) {Value v=text(s);v.type=ZVALUE_NUMBER;v.number=strtod(s,NULL);return v;}
static int json_value(const char *source,Value *out) {
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx;JSValue value;int ok=1;
    if(!rt)return 0;JS_SetMemoryLimit(rt,4*1024*1024);ctx=JS_NewContext(rt);
    if(!ctx){JS_FreeRuntime(rt);return 0;}
    value=JS_ParseJSON(ctx,source,strlen(source),"Browser value");
    if(JS_IsException(value))ok=0;
    else if(JS_IsNull(value)){memset(out,0,sizeof(*out));out->type=ZVALUE_NULL;}
    else if(JS_IsBool(value)){out->type=ZVALUE_STATUS;out->number=JS_ToBool(ctx,value);}
    else if(JS_IsArray(value)){
        JSValue length=JS_GetPropertyStr(ctx,value,"length");uint32_t n=0,i;
        JS_ToUint32(ctx,&n,length);JS_FreeValue(ctx,length);
        if(n>10000)ok=0;else {
            out->type=ZVALUE_TEXT_ARRAY;out->array=calloc(1,sizeof(Array));
            if(!out->array)ok=0;else {
                out->array->references=1;out->array->count=n;out->array->items=calloc(n?n:1,sizeof(Value));
                if(!out->array->items){out->array->count=0;ok=0;}
                else for(i=0;i<n;i++){JSValue item=JS_GetPropertyUint32(ctx,value,i);const char *s=JS_ToCString(ctx,item);
                    if(!s)ok=0;else out->array->items[i]=text(s);
                    JS_FreeCString(ctx,s);JS_FreeValue(ctx,item);if(!ok)break;}
            }
        }
    }else {
        const char *s=JS_ToCString(ctx,value);if(!s)ok=0;else *out=JS_IsNumber(value)?number(s):text(s);
        JS_FreeCString(ctx,s);
    }
    JS_FreeValue(ctx,value);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    if(!ok)drop(out);return ok;
}
static Value event_args[4];
static size_t event_count;
static const char *string(Value *v, char *buffer) {
    if (v->type == ZVALUE_TEXT) return v->text ? v->text : "";
    if (v->type == ZVALUE_STATUS) return v->number ? "alive" : "dead";
    if (v->type == ZVALUE_NUMBER && v->text) return v->text;
    if (v->type == ZVALUE_FUNCTION) return v->text ? v->text : "";
    snprintf(buffer,64,"%.17g",v->number); return buffer;
}
static int fail(const char *message) { snprintf(failure,sizeof(failure),"%s",message); return 0; }
static Value *json_field(Value *base,const char *key) {
    size_t i;if(!base || !base->object)return NULL;
    for(i=0;i<base->object->count;i++)if(!strcmp(base->object->schema->fields[i].name,key))return &base->object->fields[i];
    fail("JSON object has no field with that name");return NULL;
}
static int safe_asset_path(const char *path) {
    const char *part=path,*p=path;if(!path || !*path)return 0;
    for(;;p++) {
        if(*p && (unsigned char)*p<32)return 0;
        if(*p=='\\' || *p=='%' || *p=='?' || *p=='#' || *p==':')return 0;
        if(*p=='/' || !*p) {
            size_t n=(size_t)(p-part);
            if(!n || (n==1 && part[0]=='.') || (n==2 && part[0]=='.' && part[1]=='.'))return 0;
            if(!*p)return 1;part=p+1;
        }
    }
}
// Keep the original decimal token, not QuickJS's rounded IEEE-754 conversion.
static char *json_number_token(JSContext *ctx,const char *source,const char *wanted) {
    const char *p=source;char *result=NULL;while(*p && *p!='{')p++;if(*p)p++;
    while(*p) {
        const char *key,*end,*value;JSValue decoded;const char *name;int match;
        while(*p && (unsigned char)*p<=32)p++;if(*p!='"')return result;
        key=p++;while(*p && *p!='"'){if(*p=='\\' && p[1])p++;p++;}if(!*p)return NULL;end=++p;
        char *key_copy=zsharp_copy_text(key,(size_t)(end-key));if(!key_copy)return NULL;
        decoded=JS_ParseJSON(ctx,key_copy,(size_t)(end-key),"JSON key");free(key_copy);name=JS_ToCString(ctx,decoded);
        match=name && !strcmp(name,wanted);JS_FreeCString(ctx,name);JS_FreeValue(ctx,decoded);
        while(*p && (unsigned char)*p<=32)p++;if(*p++!=':')return NULL;
        while(*p && (unsigned char)*p<=32)p++;value=p;
        if(*p=='"'){p++;while(*p && *p!='"'){if(*p=='\\' && p[1])p++;p++;}if(*p)p++;}
        else while(*p && *p!=',' && *p!='}')p++;
        end=p;while(end>value && (unsigned char)end[-1]<=32)end--;
        if(match){free(result);result=zsharp_copy_text(value,(size_t)(end-value));}
        while(*p && (unsigned char)*p<=32)p++;if(*p!=',')return result;p++;
    }return result;
}
static int load_json(ZSharpRoom *room,const char *schema_name,const char *path,Value *out) {
    size_t i;ZSharpJsonSchema *schema=NULL;int imported=0,read_result,ok=0;
    char request[4096],source[65536];JSRuntime *rt=NULL;JSContext *ctx=NULL;JSValue parsed=JS_UNDEFINED;
    for(i=0;i<room->import_count;i++)if(!strcmp(room->imports[i].path,"ZSharp.JSON"))imported=1;
    if(!imported)return fail("JSON.load requires import ZSharp.JSON()");
    for(i=0;i<room->json_schema_count;i++)if(!strcmp(room->json_schemas[i].name,schema_name))schema=&room->json_schemas[i];
    if(!schema)return fail("JSON schema does not exist in this room");
    if(!safe_asset_path(path) || strlen(path)>4000)return fail("JSON.load requires a safe project-relative path");
    snprintf(request,sizeof(request),"JSON.file:%s",path);
    read_result=host_read(request,source,sizeof(source),active ? active->id : 0,"JSON.load");
    if(read_result==-2)return 2;
    if(read_result<0)return fail("could not read JSON file (safe project-relative path and at most 64 KiB required)");
    rt=JS_NewRuntime();if(!rt)return fail("out of memory");JS_SetMemoryLimit(rt,4*1024*1024);ctx=JS_NewContext(rt);
    if(!ctx)goto done;
    parsed=JS_ParseJSON(ctx,source,strlen(source),path);
    if(JS_IsException(parsed) || !JS_IsObject(parsed) || JS_IsArray(parsed) || JS_IsNull(parsed))goto done;
    {JSPropertyEnum *keys=NULL;uint32_t count=0;
        if(JS_GetOwnPropertyNames(ctx,&keys,&count,parsed,JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)<0)goto done;
        JS_FreePropertyEnum(ctx,keys,count);if(count!=schema->field_count)goto done;
    }
    out->type=ZVALUE_OBJECT;out->object=calloc(1,sizeof(Object));if(!out->object)goto done;
    out->object->references=1;out->object->schema=schema;
    out->object->fields=calloc(schema->field_count ? schema->field_count : 1,sizeof(Value));if(!out->object->fields)goto done;
    out->object->count=schema->field_count;
    for(i=0;i<schema->field_count;i++) {
        JSValue field=JS_GetPropertyStr(ctx,parsed,schema->fields[i].name);Value *v=&out->object->fields[i];int valid=0;
        if(schema->fields[i].type==ZVALUE_STATUS && JS_IsBool(field)){v->type=ZVALUE_STATUS;v->number=JS_ToBool(ctx,field);valid=1;}
        else if(schema->fields[i].type==ZVALUE_NUMBER && JS_IsNumber(field)) {
            char *raw=json_number_token(ctx,source,schema->fields[i].name);
            if(raw){*v=number(raw);valid=v->text!=NULL;free(raw);}
        }
        else if(schema->fields[i].type==ZVALUE_TEXT && JS_IsString(field)) {
            size_t length;const char *s=JS_ToCStringLen(ctx,&length,field);
            if(s && !memchr(s,0,length)){*v=text(s);valid=v->text!=NULL;}
            JS_FreeCString(ctx,s);
        }
        JS_FreeValue(ctx,field);if(!valid)goto done;
    }
    ok=1;
done:
    if(ctx){JS_FreeValue(ctx,parsed);JS_FreeContext(ctx);}JS_FreeRuntime(rt);
    if(!ok){drop(out);return fail("JSON file does not match schema (required keys and text/number/status types must match exactly)");}return 1;
}
static int engine_interrupt(JSRuntime *runtime,void *opaque){(void)runtime;return host_now()>*(double *)opaque;}
static int regex_execute(const ZSharpInstruction *in,Value *arguments,Value *result){
    static const char code[]="(function(text,pattern,replacement,flags,kind){"
        "if(/[^gimsu]/.test(flags))throw new Error('supported flags: g, i, m, s, u');"
        "if(kind!==1&&!flags.includes('g'))flags+='g';const re=new RegExp(pattern,flags);"
        "if(kind===1)return re.test(text);if(kind===3)return text.replace(re,replacement);"
        "const out=[];for(const m of text.matchAll(re)){if(out.length===100000)throw new Error('too many matches');out.push(m[0]);}return out;})";
    JSRuntime *runtime=NULL;JSContext *context=NULL;JSValue fn=JS_UNDEFINED,returned=JS_UNDEFINED,values[5];
    double deadline=host_now()+2000;size_t i;int ok=0;
    for(i=0;i<5;i++)values[i]=JS_UNDEFINED;
    for(i=0;i<in->argument_count;i++)if(arguments[i].type!=ZVALUE_TEXT)return fail("Regex arguments must be text");
    runtime=JS_NewRuntime();if(!runtime)return fail("out of memory");
    JS_SetMemoryLimit(runtime,16u*1024u*1024u);JS_SetMaxStackSize(runtime,512u*1024u);
    JS_SetInterruptHandler(runtime,engine_interrupt,&deadline);
    context=JS_NewContext(runtime);if(!context)goto done;
    fn=JS_Eval(context,code,sizeof(code)-1,"Regex",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(fn))goto exception;
    values[0]=JS_NewString(context,arguments[0].text);values[1]=JS_NewString(context,arguments[1].text);
    values[2]=JS_NewString(context,in->number_operand==3 ? arguments[2].text : "");
    values[3]=JS_NewString(context,in->argument_count==(in->number_operand==3 ? 4u : 3u) ? arguments[in->argument_count-1].text : "");
    values[4]=JS_NewInt32(context,in->number_operand);
    returned=JS_Call(context,fn,JS_UNDEFINED,5,values);if(JS_IsException(returned))goto exception;
    if(in->number_operand==1){result->type=ZVALUE_STATUS;result->number=JS_ToBool(context,returned);}
    else if(in->number_operand==3){size_t length;const char *s=JS_ToCStringLen(context,&length,returned);
        if(!s)goto exception;if(memchr(s,0,length)){JS_FreeCString(context,s);fail("Regex result contains an unsupported NUL");goto done;}
        *result=text(s);JS_FreeCString(context,s);if(!result->text)goto done;
    }else {
        uint32_t count;JSValue length=JS_GetPropertyStr(context,returned,"length");
        int converted=JS_ToUint32(context,&count,length);JS_FreeValue(context,length);if(converted<0)goto exception;
        if(count>100000){fail("too many Regex matches");goto done;}
        result->type=ZVALUE_TEXT_ARRAY;result->array=calloc(1,sizeof(Array));if(!result->array)goto done;
        result->array->references=1;result->array->items=calloc(count ? count : 1,sizeof(Value));if(!result->array->items)goto done;
        for(i=0;i<count;i++){JSValue item=JS_GetPropertyUint32(context,returned,(uint32_t)i);size_t length;const char *s=JS_ToCStringLen(context,&length,item);
            JS_FreeValue(context,item);if(!s)goto exception;
            if(memchr(s,0,length)){JS_FreeCString(context,s);fail("Regex match contains an unsupported NUL");goto done;}
            result->array->items[i]=text(s);JS_FreeCString(context,s);if(!result->array->items[i].text)goto done;result->array->count++;
        }
    }ok=1;goto done;
exception: {
    JSValue exception=JS_GetException(context);const char *s=JS_ToCString(context,exception);
    snprintf(failure,sizeof(failure),"Regex failed: %s",s ? s : "interrupted or out of memory");
    if(s)JS_FreeCString(context,s);JS_FreeValue(context,exception);
}
done:
    if(context){for(i=0;i<5;i++)JS_FreeValue(context,values[i]);JS_FreeValue(context,returned);JS_FreeValue(context,fn);JS_FreeContext(context);}
    JS_FreeRuntime(runtime);if(!ok){drop(result);if(!*failure)fail("Regex out of memory");}return ok;
}
static ZSharpRoom *find_room(ZSharpProgram *module,const char *name){size_t r;
    for(r=0;r<module->room_count;r++)if(!strcmp(module->rooms[r].qualified_name ? module->rooms[r].qualified_name : module->rooms[r].name,name))return &module->rooms[r];
    for(r=0;r<module->room_count;r++)if(!strcmp(module->rooms[r].name,name))return &module->rooms[r];return NULL;
}
static int room_visible(ZSharpRoom *target,ZSharpRoom *caller,int foreign){
    if(target==caller)return 1;if(foreign)return target->visibility==ZVISIBILITY_NOTICED;
    if(target->visibility!=ZVISIBILITY_SILENT)return 1;
    return target->parent_name && caller->qualified_name && !strcmp(target->parent_name,caller->qualified_name);
}
static Global *global_value(ZSharpRoom *room,const char *name){size_t i;Global *g;
    for(i=0;i<room->variable_count;i++)if(!strcmp(room->variables[i].name,name))break;
    if(i==room->variable_count){fail("room variable does not exist");return NULL;}
    for(g=globals;g;g=g->next)if(g->definition==&room->variables[i])return g;
    g=calloc(1,sizeof(*g));if(!g){fail("out of memory");return NULL;}
    g->definition=&room->variables[i];g->value.type=g->definition->type;
    if(g->value.type==ZVALUE_NUMBER)g->value=number(g->definition->number_text ? g->definition->number_text : "0");
    else if(g->value.type==ZVALUE_TEXT || g->value.type==ZVALUE_FUNCTION){g->value=text(g->definition->text_value ? g->definition->text_value : "");g->value.type=g->definition->type;}
    else if(g->value.type==ZVALUE_STATUS)g->value.number=g->definition->number_value;
    else if(g->value.type==ZVALUE_TEXT_ARRAY || g->value.type==ZVALUE_NUMBER_ARRAY){
        size_t n=g->value.type==ZVALUE_TEXT_ARRAY ? g->definition->text_item_count : g->definition->number_item_count;
        g->value.array=calloc(1,sizeof(Array));
        if(!g->value.array){free(g);fail("out of memory");return NULL;}
        g->value.array->references=1;g->value.array->count=n;g->value.array->items=calloc(n ? n : 1,sizeof(Value));
        if(!g->value.array->items){free(g->value.array);free(g);fail("out of memory");return NULL;}
        for(i=0;i<n;i++)g->value.array->items[i]=g->value.type==ZVALUE_TEXT_ARRAY ? text(g->definition->text_items[i]) : number(g->definition->number_items[i]);
    }else if(g->value.type==ZVALUE_OBJECT && g->definition->object_type && !strncmp(g->definition->object_type,"$json:",6)) {
        if(load_json(room,g->definition->object_type+6,g->definition->constructor_arguments[0].text_value,&g->value)!=1){drop(&g->value);free(g);return NULL;}
    }else {free(g);fail("object instances are not ported to bZVM yet");return NULL;}
    g->next=globals;globals=g;return g;
}
static int execute(ZSharpRoom *, ZSharpFunction *);
static int execute_frame(ZSharpRoom *, ZSharpFunction *, Frame *);
static ZSharpProgram *find_module(const char *name){size_t i;
    for(i=0;i<module_count;i++)if(!strcmp(modules[i].source_name,name))return &modules[i];
    return NULL;
}
static Global *path_global(ZSharpRoom *caller,const char *path){
    char buffer[1024],room_name[1024];char *parts[5],*p;size_t n=0,i,start=0;
    ZSharpProgram *target=current_program;ZSharpRoom *target_room;Global *g;
    if(strlen(path)>=sizeof(buffer)){fail("variable path is too long");return NULL;}
    strcpy(buffer,path);p=buffer;parts[n++]=p;
    while((p=strchr(p,'.'))){*p++=0;if(n==5){fail("variable path is too long");return NULL;}parts[n++]=p;}
    if(n<2){fail("qualified variable path needs a room and variable");return NULL;}
    if(has_settings && !strcmp(parts[0],settings.project_id))start=1;
    if(n-start>=3 && find_module(parts[start])){target=find_module(parts[start]);start++;}
    if(target!=current_program){int imported=0;
        for(i=0;i<caller->import_count;i++){const char *imp=caller->imports[i].path,*last=strrchr(imp,'.');
            if(has_settings && !strncmp(imp,settings.project_id,strlen(settings.project_id)) && imp[strlen(settings.project_id)]=='.' &&
                last && !strcmp(last+1,target->source_name))imported=1;}
        if(!imported){fail("cross-file variable access requires an import");return NULL;}
    }
    room_name[0]=0;for(i=start;i+1<n;i++){if(i>start)strcat(room_name,".");strcat(room_name,parts[i]);}
    target_room=find_room(target,room_name);
    if(!target_room || !room_visible(target_room,caller,target!=current_program)){fail("room variable target is missing or not visible");return NULL;}
    g=global_value(target_room,parts[n-1]);
    if(g && target_room!=caller && !g->definition->is_public){fail("room variable is silent outside its own room");return NULL;}
    return g;
}
static Value *json_path_field(ZSharpRoom *room,Local *locals,size_t count,const char *path,int *found) {
    char prefix[1024];const char *dot=strchr(path,'.');size_t i,n;Global *g;
    *found=0;if(!dot)return NULL;n=(size_t)(dot-path);
    for(i=count;i>0;i--)if(strlen(locals[i-1].name)==n && !strncmp(locals[i-1].name,path,n))break;
    if(i && locals[i-1].value.object){*found=1;return json_field(&locals[i-1].value,dot+1);}
    for(i=0;i<room->variable_count;i++)if(strlen(room->variables[i].name)==n && !strncmp(room->variables[i].name,path,n))break;
    if(i<room->variable_count && room->variables[i].object_type && !strncmp(room->variables[i].object_type,"$json:",6)) {
        *found=1;g=global_value(room,room->variables[i].name);return g ? json_field(&g->value,dot+1) : NULL;
    }
    dot=strrchr(path,'.');n=(size_t)(dot-path);if(n>=sizeof(prefix))return NULL;
    memcpy(prefix,path,n);prefix[n]=0;g=path_global(room,prefix);
    if(g && g->value.object){*found=1;return json_field(&g->value,dot+1);}
    failure[0]=0;return NULL;
}
static ZSharpProgram *resolve_target(ZSharpRoom *caller,const ZSharpInstruction *in){
    ZSharpProgram *target;size_t i;int imported=0;
    if(in->operand && *in->operand && in->op!=ZOP_BROWSER_BIND &&
        (!has_settings || strcmp(in->operand,settings.project_id))){fail("calls to external projects are not supported in bZVM yet");return NULL;}
    target=in->call_file ? find_module(in->call_file) : NULL;
    if(!target){fail("called script file is not loaded; import it first");return NULL;}
    if(target==current_program)imported=1;
    for(i=0;i<caller->import_count;i++){
        const char *path=caller->imports[i].path,*last=strrchr(path,'.');
        if(has_settings && !strncmp(path,settings.project_id,strlen(settings.project_id)) &&
            path[strlen(settings.project_id)]=='.' && last && !strcmp(last+1,in->call_file))imported=1;
    }
    if(!imported){fail("cross-file function call requires an import in the calling room");return NULL;}
    for(i=0;i<target->room_count;i++)if(!strcmp(target->rooms[i].name,in->call_room)){
        size_t f;for(f=0;f<target->rooms[i].function_count;f++)if(!strcmp(target->rooms[i].functions[f].name,in->call_function)){
            if(!room_visible(&target->rooms[i],caller,target!=current_program) ||
                (&target->rooms[i]!=caller && !target->rooms[i].functions[f].is_public)){fail("function target is not visible");return NULL;}
            return target;
        }
    }
    fail("cross-file function target does not exist");return NULL;
}
static int call_at(const char *room, const char *name, size_t entry,Value *arguments,size_t count,const char *outcome) {
    size_t r,f;
    for (r=0;r<program.room_count;r++) if (!strcmp(program.rooms[r].name,room))
        for (f=0;f<program.rooms[r].function_count;f++) if (!strcmp(program.rooms[r].functions[f].name,name))
        {
            ZSharpFunction *function=&program.rooms[r].functions[f];
            Frame *frame;
            size_t a;
            if(!entry && count!=function->parameter_count)return fail("function argument count does not match its parameters");
            if(entry && entry>=function->instruction_count)return fail("invalid inline handler entry");
            frame=calloc(1,sizeof(Frame));if(!frame)return fail("out of memory");
            frame->stack=calloc(function->instruction_count+function->parameter_count+1,sizeof(Value));
            frame->locals=calloc(function->instruction_count+function->parameter_count+1,sizeof(Local));frame->pc=entry;frame->owner=current_program;
            frame->outcome=outcome ? zsharp_copy_text(outcome,strlen(outcome)) : NULL;
            if(!frame->stack || !frame->locals || (outcome && !frame->outcome)){free(frame->stack);free(frame->locals);free(frame->outcome);free(frame);return fail("out of memory");}
            for(a=0;a<count;a++){
                if(arguments[a].type!=function->parameters[a].type){size_t j;for(j=0;j<a;j++)drop(&frame->locals[j].value);
                    free(frame->stack);free(frame->locals);free(frame->outcome);free(frame);return fail("function argument type does not match its parameter");}
                frame->locals[a].name=function->parameters[a].name;frame->locals[a].value=copy(arguments[a]);frame->nl++;
            }
            return execute_frame(&program.rooms[r],function,frame);
        }
    return fail("browser callback/function target does not exist");
}
static int call(const char *room,const char *name){return call_at(room,name,0,NULL,0,NULL);}
static int execute(ZSharpRoom *room, ZSharpFunction *function) {
    return execute_frame(room, function, NULL);
}
static int execute_frame(ZSharpRoom *room, ZSharpFunction *function, Frame *saved) {
    ZSharpProgram *previous=current_program,*owner=saved ? saved->owner : current_program;
    size_t capacity=function->instruction_count+function->parameter_count+1;
    Value *stack = saved ? saved->stack : calloc(capacity,sizeof(Value));
    Local *locals = saved ? saved->locals : calloc(capacity,sizeof(Local));
    size_t sp=saved ? saved->sp : 0,nl=saved ? saved->nl : 0,pc=saved ? saved->pc : 0,i; int ok=1;
    int waiting_return=0,expected_return=saved ? saved->waiting_return : 0;
    char *outcome=saved ? saved->outcome : NULL;
    free(saved);
    if (!stack || !locals) { free(stack); free(locals);free(outcome);return fail("out of memory"); }
    if (++depth>64) { depth--;for(i=0;i<nl;i++)drop(&locals[i].value);free(stack);free(locals);free(outcome);return fail("browser call depth exceeded"); }
    current_program=owner;
    if(expected_return){
        if(!active->has_return){ok=fail("function did not feed a return value");goto done;}
        stack[sp++]=active->returned;memset(&active->returned,0,sizeof(Value));active->has_return=0;
    }else {drop(&active->returned);active->has_return=0;}
    for (;pc<function->instruction_count && ok==1;pc++) {
        ZSharpInstruction *in=&function->instructions[pc]; Value v={0},a={0},b={0};
        if(sp>=capacity){ok=fail("browser stack limit exceeded");break;}
        if (!budget--) { ok=fail("browser instruction budget exceeded; use event handlers, not infinite loops"); break; }
        switch(in->op) {
        case ZOP_DELAY: {
            char *end;
            double delay = strtod(in->operand, &end);
            if (*end || !isfinite(delay) || delay < 0 || delay > 2147483647.0) {
                ok=fail("browser wait must be between 0 and 2147483647 milliseconds"); break;
            }
            active->delay=delay; ok=2; break;
        }
        case ZOP_PUSH_TEXT: stack[sp++]=text(in->operand); break;
        case ZOP_PUSH_NUMBER: stack[sp++]=number(in->operand);break;
        case ZOP_PUSH_FUNCTION: v=text(in->operand);v.type=ZVALUE_FUNCTION;stack[sp++]=v;break;
        case ZOP_PUSH_NULL: v.type=ZVALUE_NULL;stack[sp++]=v;break;
        case ZOP_PUSH_STATUS: v.type=ZVALUE_STATUS;v.number=in->number_operand;stack[sp++]=v;break;
        case ZOP_LOAD_NAME:
            for(i=nl;i>0;i--) if(!strcmp(locals[i-1].name,in->operand)) break;
            if(i) stack[sp++]=copy(locals[i-1].value);
            else {Global *g=global_value(room,in->operand);if(!g){ok=0;break;}stack[sp++]=copy(g->value);}break;
        case ZOP_LOAD_PATH: {
            char buffer[65536];
            if(strncmp(in->operand,"Browser.",8)) {
                int object_found=0;Value *field=json_path_field(room,locals,nl,in->operand,&object_found);
                if(object_found){if(!field)ok=0;else stack[sp++]=copy(*field);}
                else {Global *g=path_global(room,in->operand);if(!g)ok=0;else stack[sp++]=copy(g->value);}break;
            }
            {char site[512];snprintf(site,sizeof(site),"%s:%s:%zu",owner->source_name,function->name,pc);
            if(host_read(in->operand,buffer,sizeof(buffer),active->id,site)<0) {ok=fail("browser element/property read failed (missing ID or oversized value)");break;}}
            if(buffer[0]==30){if(!json_value(buffer+1,&v))ok=fail("invalid typed Browser result");else stack[sp++]=v;}
            else stack[sp++]=text(buffer);break;
        }
        case ZOP_STORE_PATH: {
            Global *g;int found=0;Value *field;if(!sp){ok=fail("assignment stack underflow");break;}
            field=json_path_field(room,locals,nl,in->operand,&found);
            if(found){
                v=stack[--sp];memset(&stack[sp],0,sizeof(Value));
                if(!field)ok=0;else if(field->type!=v.type)ok=fail("JSON field assignment requires the declared type");
                else {drop(field);*field=v;memset(&v,0,sizeof(v));}drop(&v);break;
            }
            v=stack[--sp];memset(&stack[sp],0,sizeof(Value));g=path_global(room,in->operand);
            if(!g)ok=0;else if(g->value.type!=v.type)ok=fail("qualified assignment has the wrong type");
            else {drop(&g->value);g->value=v;memset(&v,0,sizeof(Value));}
            drop(&v);break;
        }
        case ZOP_ARRAY_LENGTH:
            if(!sp){ok=fail("Length stack underflow");break;}
            a=stack[--sp];memset(&stack[sp],0,sizeof(Value));
            if(!a.array)ok=fail("Length is only available on arrays");
            else {char count[32];snprintf(count,sizeof(count),"%zu",a.array->count);stack[sp++]=number(count);}
            drop(&a);break;
        case ZOP_GET_INDEX: case ZOP_SET_INDEX: {
            size_t index;
            if(sp<(in->op==ZOP_SET_INDEX ? 3u : 2u)){ok=fail("array stack underflow");break;}
            if(in->op==ZOP_SET_INDEX){v=stack[--sp];memset(&stack[sp],0,sizeof(Value));}
            b=stack[--sp];a=stack[--sp];memset(&stack[sp],0,2*sizeof(Value));
            if(a.object && b.type==ZVALUE_TEXT){
                Value *field=json_field(&a,b.text);
                if(!field)ok=0;
                else if(in->op==ZOP_GET_INDEX)stack[sp++]=copy(*field);
                else if(field->type!=v.type)ok=fail("JSON field assignment requires the declared type");
                else {drop(field);*field=v;memset(&v,0,sizeof(v));}
                drop(&a);drop(&b);drop(&v);break;
            }
            if(!a.array || b.type!=ZVALUE_NUMBER || !zsharp_decimal_to_size(b.text,&index) || index>=a.array->count)ok=fail("array index must be a whole nonnegative number inside the array");
            else if(in->op==ZOP_GET_INDEX)stack[sp++]=copy(a.array->items[index]);
            else {
                if(a.type==ZVALUE_TEXT_ARRAY && v.type==ZVALUE_NUMBER){v.type=ZVALUE_TEXT;}
                if(v.type!=a.array->items[index].type)host_print("runtime warning: array assignment has the wrong type; skipped");
                else {drop(&a.array->items[index]);a.array->items[index]=v;memset(&v,0,sizeof(Value));}
            }
            drop(&a);drop(&b);drop(&v);break;
        }
        case ZOP_MATH: {
            double first,second=0,result=0;char buffer[768],*end;int precision=17;
            if(!sp || (in->argument_count==2 && sp<2)){ok=fail("Math argument stack underflow");break;}
            b=stack[--sp];memset(&stack[sp],0,sizeof(Value));
            if(in->argument_count==2){a=stack[--sp];memset(&stack[sp],0,sizeof(Value));second=b.number;first=a.number;}else first=b.number;
            if(b.type!=ZVALUE_NUMBER || (in->argument_count==2 && a.type!=ZVALUE_NUMBER)){ok=fail("Math requires numbers");drop(&a);drop(&b);break;}
            switch(in->number_operand){
            case 1:result=sin(first);break;case 2:result=cos(first);break;case 3:result=tan(first);break;
            case 4:if(first<0)ok=fail("Math.sqrt requires a nonnegative number");else result=sqrt(first);break;
            case 5:result=fabs(first);break;case 6:result=fmin(first,second);break;case 7:result=fmax(first,second);break;
            case 8:result=first*3.14159265358979323846/180.0;break;case 9:result=first*180.0/3.14159265358979323846;break;
            case 10:if((first<0 && second!=trunc(second)) || (!first && second<0))ok=fail("Math.pow has an invalid base/exponent");else result=pow(first,second);break;
            default:ok=fail("unknown Math function");break;}
            if(ok && !isfinite(result))ok=fail("Math result exceeds the supported finite number range");
            if(ok){
                if(in->number_operand!=10 && fabs(result)<1e-12)result=0;
                if(in->number_operand==10 && result!=0 && fabs(result)<1)precision=(int)ceil(-log10(fabs(result)))+17;
                snprintf(buffer,sizeof(buffer),"%.*f",precision,result);end=buffer+strlen(buffer);
                while(end>buffer && end[-1]=='0')*--end=0;if(end>buffer && end[-1]=='.')*--end=0;
                if(!strcmp(buffer,"-0"))strcpy(buffer,"0");stack[sp++]=number(buffer);
            }drop(&a);drop(&b);break;
        }
        case ZOP_NETWORK: {
            size_t n=in->argument_count,j;JSRuntime *rt;JSContext *ctx;JSValue list;
            const char *encoded=NULL;char result[65536];
            if(n>32 || sp<n){ok=fail("invalid network argument stack");break;}
            rt=JS_NewRuntime();ctx=rt?JS_NewContext(rt):NULL;
            if(!ctx){if(rt)JS_FreeRuntime(rt);ok=fail("out of memory");break;}
            list=JS_NewArray(ctx);
            for(j=0;j<n;j++) {
                Value *arg=&stack[sp-n+j];JSValue item;
                if(arg->type!=ZVALUE_TEXT && arg->type!=ZVALUE_NUMBER){ok=fail("network arguments require text or numbers");break;}
                if(arg->type==ZVALUE_TEXT &&
                   ((!strcmp(in->operand,"HTTP.request") && j>0 && (!strcmp(stack[sp-n+j-1].text,"onResponse") || !strcmp(stack[sp-n+j-1].text,"onError"))) ||
                    (j==1 && (!strcmp(in->operand,"Websocket.callback") || !strcmp(in->operand,"Websocket.connected") || !strcmp(in->operand,"Websocket.error") || !strcmp(in->operand,"Websocket.closed"))))) {
                    ZSharpInstruction target={0};char storage[2048],canonical[2048];ZSharpProgram *module;
                    if(!zsharp_call_target_decode(arg->text,&target,storage,sizeof(storage),failure,sizeof(failure)) || !(module=resolve_target(room,&target))){ok=0;break;}
                    snprintf(canonical,sizeof(canonical),"@%s:%s:%s",module->source_name,target.call_room,target.call_function);
                    item=JS_NewString(ctx,canonical);
                }else item=arg->type==ZVALUE_TEXT?JS_NewString(ctx,arg->text):JS_NewFloat64(ctx,arg->number);
                JS_SetPropertyUint32(ctx,list,(uint32_t)j,item);
            }
            if(ok){JSValue json=JS_JSONStringify(ctx,list,JS_UNDEFINED,JS_UNDEFINED);encoded=JS_ToCString(ctx,json);JS_FreeValue(ctx,json);
                if(!encoded || !host_network(in->operand,encoded,result,sizeof(result)))ok=fail("network operation failed; see host error");
                else if(in->number_operand){if(!json_value(result,&v))ok=fail("invalid network result");}
            }
            JS_FreeCString(ctx,encoded);JS_FreeValue(ctx,list);JS_FreeContext(ctx);JS_FreeRuntime(rt);
            for(j=0;j<n;j++)drop(&stack[--sp]);
            if(ok && in->number_operand)stack[sp++]=v;break;
        }
        case ZOP_FILE_READ: case ZOP_FILE_EXISTS: {
            char request[4096],source[65536];int result;
            if(!sp || stack[sp-1].type!=ZVALUE_TEXT || !safe_asset_path(stack[sp-1].text) || strlen(stack[sp-1].text)>4000){ok=fail("File read requires a safe project-relative text path");break;}
            snprintf(request,sizeof(request),"%s%s",in->op==ZOP_FILE_READ ? "JSON.file:" : "File.exists:",stack[sp-1].text);
            result=host_read(request,source,sizeof(source),active->id,"File.read");
            if(result==-2){active->delay=10;pc--;ok=2;break;}
            if(result<0){ok=fail("could not read project file");break;}
            drop(&stack[--sp]);
            if(in->op==ZOP_FILE_READ)stack[sp++]=text(source);
            else {v.type=ZVALUE_STATUS;v.number=!strcmp(source,"alive");stack[sp++]=v;}
            break;
        }
        case ZOP_JSON_LOAD: {
            if(!sp || stack[sp-1].type!=ZVALUE_TEXT){ok=fail("JSON.load requires a text file path");break;}
            int loaded=load_json(room,in->operand,stack[sp-1].text,&v);
            if(loaded==2){active->delay=10;pc--;ok=2;break;}
            drop(&stack[--sp]);if(!loaded)ok=0;else stack[sp++]=v;break;
        }
        case ZOP_TIME: {
            Value args[3]={{0}};const char *texts[3];size_t n=in->argument_count,j;
            char result[512],error[512];int is_text;
            if(n>3 || sp<n){ok=fail("invalid Time argument stack");break;}
            for(j=n;j>0;j--){args[j-1]=stack[--sp];memset(&stack[sp],0,sizeof(Value));texts[j-1]=args[j-1].text;
                if(args[j-1].type!=ZVALUE_TEXT && args[j-1].type!=ZVALUE_NUMBER)ok=fail("Time arguments must be text or numbers");}
            if(ok && !zsharp_time_evaluate(in->number_operand,texts,n,&time_host,result,sizeof(result),&is_text,error,sizeof(error)))ok=fail(error);
            if(ok)stack[sp++]=is_text?text(result):number(result);
            for(j=0;j<n;j++)drop(&args[j]);break;
        }
        case ZOP_REGEX: {
            Value arguments[4]={{0}};size_t n=in->argument_count;
            if(n>4 || sp<n){ok=fail("invalid Regex argument stack");break;}
            for(i=n;i>0;i--){arguments[i-1]=stack[--sp];memset(&stack[sp],0,sizeof(Value));}
            ok=regex_execute(in,arguments,&v);for(i=0;i<n;i++)drop(&arguments[i]);
            if(ok)stack[sp++]=v;break;
        }
        case ZOP_STORE_LOCAL: case ZOP_STORE_LOCAL_TEXT: case ZOP_STORE_LOCAL_VALUE: case ZOP_STORE_LOCAL_FUNCTION: case ZOP_STORE_LOCAL_TEXT_ARRAY:
        case ZOP_STORE_NAME: case ZOP_STORE_GLOBAL: case ZOP_STORE_FIELD:
            if(!sp){ok=fail("stack underflow");break;}
            v=stack[--sp]; memset(&stack[sp],0,sizeof(Value));
            for(i=nl;i>0;i--)if(!strcmp(locals[i-1].name,in->operand))break;
            if(in->op==ZOP_STORE_LOCAL && v.type!=ZVALUE_NUMBER){drop(&v);v=number("0");host_print("runtime warning: local number received a non-number; assignment skipped");}
            if(in->op==ZOP_STORE_LOCAL_TEXT && v.type!=ZVALUE_TEXT){drop(&v);v=text("");host_print("runtime warning: local text received non-text; assignment skipped");}
            if(in->op==ZOP_STORE_LOCAL_FUNCTION && v.type!=ZVALUE_FUNCTION){drop(&v);ok=fail("function reference requires a function value");break;}
            if(in->op==ZOP_STORE_LOCAL_TEXT_ARRAY && v.type!=ZVALUE_TEXT_ARRAY){drop(&v);ok=fail("text() requires a text array");break;}
            if(i){
                if((in->op==ZOP_STORE_GLOBAL && (v.type!=ZVALUE_NUMBER || locals[i-1].value.type!=ZVALUE_NUMBER)) ||
                   (in->op==ZOP_STORE_FIELD && (v.type!=ZVALUE_TEXT || locals[i-1].value.type!=ZVALUE_TEXT))){drop(&v);ok=fail("typed assignment requires matching variable types");break;}
                drop(&locals[i-1].value);locals[i-1].value=v;
            }else if(in->op==ZOP_STORE_GLOBAL || in->op==ZOP_STORE_NAME || in->op==ZOP_STORE_FIELD){
                Global *g=global_value(room,in->operand);if(!g){drop(&v);ok=0;break;}
                if(v.type!=g->value.type){drop(&v);ok=fail("room variable assignment has the wrong type");break;}
                drop(&g->value);g->value=v;
            }else {locals[nl].name=in->operand;locals[nl++].value=v;}break;
        case ZOP_UI_SET_VALUE: {
            char buffer[64];
            if(!sp){ok=fail("stack underflow");break;}
            v=stack[--sp];memset(&stack[sp],0,sizeof(Value));
            if(strncmp(in->operand,"Browser.",8)||!host_write(in->operand,string(&v,buffer)))ok=fail("browser element/property write failed");
            drop(&v);break;
        }
        case ZOP_BROWSER_BIND: {
            char target[512];
            ZSharpProgram *module=resolve_target(room,in);if(!module){ok=0;break;}
            if(in->call_outcome && *in->call_outcome)
                snprintf(target,sizeof(target),"@%s:%s:%s:%s",module->source_name,in->call_room,in->call_function,in->call_outcome);
            else snprintf(target,sizeof(target),"@%s:%s:%s",module->source_name,in->call_room,in->call_function);
            if(!host_bind(in->operand,target))ok=fail("browser event binding failed");break;
        }
        case ZOP_BROWSER_JS: case ZOP_BROWSER_CSS:
            if(!host_raw(in->op==ZOP_BROWSER_JS ? 1:2,in->operand))ok=fail("embedded browser block failed");break;
        case ZOP_CALL_QUALIFIED: case ZOP_CALL_QUALIFIED_VALUE: case ZOP_CALL_LOCAL: {
            ZSharpInstruction decoded,*target=in;char storage[8192];Value callback={0};
            Value *arguments;size_t n=in->argument_count;ZSharpProgram *module;
            int produces=in->op==ZOP_CALL_QUALIFIED_VALUE;
            if(in->operand && !strcmp(in->operand,"@callback")){
                if(!sp){ok=fail("missing function reference");break;}
                callback=stack[--sp];memset(&stack[sp],0,sizeof(Value));
                if((callback.type!=ZVALUE_FUNCTION && callback.type!=ZVALUE_TEXT) ||
                    !zsharp_call_target_decode(callback.text,&decoded,storage,sizeof(storage),failure,sizeof(failure))){drop(&callback);ok=0;break;}
                decoded.argument_count=(uint32_t)n;target=&decoded;
            }
            if(sp<n){drop(&callback);ok=fail("function argument stack underflow");break;}
            arguments=calloc(n ? n : 1,sizeof(Value));if(!arguments){drop(&callback);ok=fail("out of memory");break;}
            for(i=n;i>0;i--){arguments[i-1]=stack[--sp];memset(&stack[sp],0,sizeof(Value));}
            module=in->op==ZOP_CALL_LOCAL ? owner : resolve_target(room,target);
            drop(&active->returned);active->has_return=0;
            if(!module)ok=0;
            else {current_program=module;
                ok=call_at(in->op==ZOP_CALL_LOCAL ? room->name : target->call_room,
                    in->op==ZOP_CALL_LOCAL ? in->operand : target->call_function,0,arguments,n,target->call_outcome);
                current_program=owner;
            }
            for(i=0;i<n;i++)drop(&arguments[i]);free(arguments);drop(&callback);
            if(ok==2)waiting_return=produces;
            else if(ok==1 && produces){
                if(!active->has_return)ok=fail("function did not feed a return value");
                else {stack[sp++]=active->returned;memset(&active->returned,0,sizeof(Value));active->has_return=0;}
            }else if(ok==1){drop(&active->returned);active->has_return=0;}
            break;
        }
        case ZOP_PRINT: {
            char buffer[64];if(!sp){ok=fail("stack underflow");break;}
            v=stack[--sp];memset(&stack[sp],0,sizeof(Value));host_print(string(&v,buffer));drop(&v);break;
        }
        case ZOP_JUMP_IF_FALSE: case ZOP_RETURN_IF_FALSE:
            if(!sp){ok=fail("stack underflow");break;}
            v=stack[--sp];memset(&stack[sp],0,sizeof(Value));
            if(v.type!=ZVALUE_STATUS && v.type!=ZVALUE_NUMBER){ok=fail("condition requires a status or number");drop(&v);break;}
            i=v.type==ZVALUE_NUMBER ? zsharp_decimal_is_zero(v.text) : !v.number;drop(&v);
            if(i){if(in->op==ZOP_RETURN_IF_FALSE)goto done;pc=in->index_operand-1;}break;
        case ZOP_JUMP: pc=in->index_operand-1;break;
        case ZOP_RETURN_VOID: goto done;
        case ZOP_RETURN_VALUE:
            if(!sp){ok=fail("return stack underflow");break;}
            v=stack[--sp];memset(&stack[sp],0,sizeof(Value));
            if((function->return_type==ZRETURN_NUMBER && v.type!=ZVALUE_NUMBER) ||
                (function->return_type==ZRETURN_TEXT && v.type!=ZVALUE_TEXT)){drop(&v);ok=fail("feed value does not match function return type");break;}
            drop(&active->returned);active->returned=v;active->has_return=1;goto done;
        case ZOP_NAMED_IF_START:
            if(outcome && strcmp(outcome,in->operand))pc=in->index_operand-1;break;
        case ZOP_NOT: case ZOP_NEGATE:
            if(!sp){ok=fail("unary operation stack underflow");break;}
            v=stack[--sp];memset(&stack[sp],0,sizeof(Value));
            if(in->op==ZOP_NOT){if(v.type!=ZVALUE_STATUS)ok=fail("not requires a status");else v.number=!v.number;}
            else {if(v.type!=ZVALUE_NUMBER)ok=fail("negation requires a number");else {char *neg=zsharp_decimal_negate(v.text,failure,sizeof(failure));if(!neg)ok=0;else {free(v.text);v.text=neg;v.number=strtod(neg,NULL);}}}
            if(ok)stack[sp++]=v;else drop(&v);break;
        case ZOP_ADD: case ZOP_SUBTRACT: case ZOP_MULTIPLY: case ZOP_DIVIDE:
        case ZOP_EQUAL: case ZOP_NOT_EQUAL: case ZOP_GREATER: case ZOP_LESS:
        case ZOP_GREATER_EQUAL: case ZOP_LESS_EQUAL: case ZOP_AND: case ZOP_OR: case ZOP_REMAINDER:
            if(sp<2){ok=fail("stack underflow");break;}
            b=stack[--sp];a=stack[--sp];memset(&stack[sp],0,2*sizeof(Value));
            if(in->op==ZOP_ADD && (a.type==ZVALUE_TEXT || b.type==ZVALUE_TEXT)){
                char ab[64],bb[64]; const char *as=string(&a,ab), *bs=string(&b,bb);
                size_t n=strlen(as)+strlen(bs)+1;v.type=ZVALUE_TEXT;v.text=malloc(n);
                if(v.text){snprintf(v.text,n,"%s%s",as,bs);}else ok=fail("out of memory");
            }else if(in->op==ZOP_EQUAL || in->op==ZOP_NOT_EQUAL){
                v.type=ZVALUE_STATUS;v.number=a.type==b.type && (a.type==ZVALUE_TEXT || a.type==ZVALUE_FUNCTION ? !strcmp(a.text,b.text) :
                    a.type==ZVALUE_NUMBER ? zsharp_decimal_compare(a.text,b.text)==0 : a.array || b.array ? a.array==b.array : a.object || b.object ? a.object==b.object : a.number==b.number);
                if(in->op==ZOP_NOT_EQUAL)v.number=!v.number;
            }else if(in->op==ZOP_AND || in->op==ZOP_OR){
                if(a.type!=ZVALUE_STATUS || b.type!=ZVALUE_STATUS)ok=fail("boolean operation requires status operands");
                v.type=ZVALUE_STATUS;v.number=in->op==ZOP_AND ? a.number&&b.number : a.number||b.number;
            }else if(a.type!=ZVALUE_NUMBER || b.type!=ZVALUE_NUMBER)ok=fail("numeric operation requires numbers");
            else{
                int comparison=zsharp_decimal_compare(a.text,b.text);char *result=NULL;
                v.type=ZVALUE_NUMBER;
                switch(in->op){
                case ZOP_ADD:result=zsharp_decimal_add(a.text,b.text,failure,sizeof(failure));break;
                case ZOP_SUBTRACT:result=zsharp_decimal_subtract(a.text,b.text,failure,sizeof(failure));break;
                case ZOP_MULTIPLY:result=zsharp_decimal_multiply(a.text,b.text,failure,sizeof(failure));break;
                case ZOP_DIVIDE:result=zsharp_decimal_divide(a.text,b.text,failure,sizeof(failure));break;
                case ZOP_REMAINDER:result=zsharp_decimal_remainder(a.text,b.text,failure,sizeof(failure));break;
                case ZOP_GREATER:v.type=ZVALUE_STATUS;v.number=comparison>0;break;case ZOP_LESS:v.type=ZVALUE_STATUS;v.number=comparison<0;break;
                case ZOP_GREATER_EQUAL:v.type=ZVALUE_STATUS;v.number=comparison>=0;break;case ZOP_LESS_EQUAL:v.type=ZVALUE_STATUS;v.number=comparison<=0;break;
                default:break;
                }if(v.type==ZVALUE_NUMBER){if(!result)ok=0;else {v.text=result;v.number=strtod(result,NULL);}}
            }
            drop(&a);drop(&b);if(ok)stack[sp++]=v;else drop(&v);break;
        default: snprintf(failure,sizeof(failure),"opcode %d is not supported by the browser prototype",in->op);ok=0;break;
        }
    }
done:
    if(ok==2) {
        Frame *frame=calloc(1,sizeof(Frame));
        if(frame) {
            frame->owner=owner;frame->room=room;frame->function=function;frame->stack=stack;frame->locals=locals;
            frame->sp=sp;frame->nl=nl;frame->pc=pc;
            frame->waiting_return=waiting_return;frame->outcome=outcome;
            if(active->tail)active->tail->next=frame;else active->head=frame;
            active->tail=frame;depth--;current_program=previous;return 2;
        }
        ok=fail("out of memory saving browser task");
    }
    for(i=0;i<sp;i++)drop(&stack[i]);for(i=0;i<nl;i++)drop(&locals[i].value);
    free(stack);free(locals);free(outcome);depth--;current_program=previous;return ok;
}
static void free_frames(Frame *frame) {
    while(frame){Frame *next=frame->next;size_t i;
        for(i=0;i<frame->sp;i++)drop(&frame->stack[i]);
        for(i=0;i<frame->nl;i++)drop(&frame->locals[i].value);
        free(frame->stack);free(frame->locals);free(frame->outcome);free(frame);frame=next;}
}
static int finish_task(Task *task,int result) {
    active=NULL;
    if(result==2){size_t count=0;Frame *frame;
        for(frame=task->head;frame;frame=frame->next)count++;
        if(count>64)result=fail("browser suspended call depth exceeded");}
    if(result==2 && host_schedule(task->id,task->delay))return 1;
    if(result==2)result=fail("could not schedule browser wait");
    {Task **link=&tasks;while(*link && *link!=task)link=&(*link)->next;
        if(*link)*link=task->next;}
    host_input_session(task->id,0);free_frames(task->head);drop(&task->returned);free(task);return result;
}
static Task *new_task(void) {
    Task *task,*other;size_t count=0;
    for(other=tasks;other;other=other->next)count++;
    if(count>=256){fail("browser task limit exceeded (256 suspended tasks)");return NULL;}
    task=calloc(1,sizeof(Task));
    if(!task){fail("out of memory");return NULL;}
    task->id=++next_task_id;host_input_session(task->id,1);task->next=tasks;tasks=task;active=task;return task;
}
__attribute__((visibility("default"))) int bzvm_resume(unsigned id) {
    Task *task;int result=1;
    for(task=tasks;task && task->id!=id;task=task->next){}
    if(!task)return fail("browser task no longer exists");
    active=task;budget=100000;failure[0]=0;
    while(task->head && result==1){
        Frame *frame=task->head,*remaining=frame->next,*tail=task->tail;
        task->head=task->tail=NULL;
        result=execute_frame(frame->room,frame->function,frame);
        if(task->tail)task->tail->next=remaining;else task->head=remaining;
        if(remaining)task->tail=tail;
    }
    return finish_task(task,result);
}
__attribute__((visibility("default"))) void bzvm_cancel(void) {
    while(tasks){Task *next=tasks->next;host_input_session(tasks->id,0);free_frames(tasks->head);drop(&tasks->returned);free(tasks);tasks=next;}
    active=NULL;
}
__attribute__((visibility("default"))) const char *bzvm_error(void){return failure;}
__attribute__((visibility("default"))) const char *bzvm_project_id(void){return has_settings ? settings.project_id : "";}
__attribute__((visibility("default"))) const char *bzvm_project_name(void){return has_settings ? settings.project_name : "";}
__attribute__((visibility("default"))) int bzvm_settings(const char *source){
    ZSharpDiagnostic diagnostic={0};char error[512]={0};ZSharpSettings parsed;
    failure[0]=0;
    if(!zsharp_settings_parse_source(source,&parsed,&diagnostic,error,sizeof(error))){
        snprintf(failure,sizeof(failure),"project.zsettings:%u:%u: %.400s",diagnostic.line,diagnostic.column,
            *diagnostic.message ? diagnostic.message : error);return 0;
    }
    if(parsed.has_window || parsed.game_start_scene || parsed.native_target_count || parsed.splash_count){
        zsharp_settings_free(&parsed);return fail("native window/game settings are not supported in bZVM");
    }
    if(parsed.dependency_count){
        zsharp_settings_free(&parsed);return fail("Store dependencies are not supported in the browser prototype yet");
    }
    {const unsigned version[4]={ZSHARP_VERSION_MAJOR,ZSHARP_VERSION_MINOR,ZSHARP_VERSION_PATCH,ZSHARP_VERSION_REVISION};size_t i;
        for(i=0;i<4;i++){if(parsed.zsharp_version[i]<version[i])break;
            if(parsed.zsharp_version[i]>version[i]){zsharp_settings_free(&parsed);return fail("project requires a newer bZVM version");}}
    }
    if(has_settings)zsharp_settings_free(&settings);
    settings=parsed;has_settings=1;return 1;
}
__attribute__((visibility("default"))) void bzvm_reset(void){size_t i;
    bzvm_cancel();for(i=0;i<module_count;i++)zsharp_program_free(&modules[i]);
    while(globals){Global *next=globals->next;drop(&globals->value);free(globals);globals=next;}
    module_count=0;current_program=&modules[0];initialized=0;failure[0]=0;depth=0;
}
__attribute__((visibility("default"))) int bzvm_load(const char *source,const char *name){
    ZSharpDiagnostic diagnostic;
    if(module_count>=64)return fail("browser project supports at most 64 script files");
    if(find_module(name))return fail("duplicate script filenames in browser project; use unique filenames");
    zsharp_program_init(&modules[module_count]);
    if(!zsharp_parse_source(source,name,&modules[module_count],&diagnostic)){
        zsharp_program_free(&modules[module_count]);
        snprintf(failure,sizeof(failure),"%s:%u:%u: %s",name,diagnostic.line,diagnostic.column,diagnostic.message);return 0;
    }
    if(modules[module_count].has_window){zsharp_program_free(&modules[module_count]);return fail("native windows are not supported in bZVM");}
    if(has_settings && (settings.zsharp_version[0]<1 ||
       (settings.zsharp_version[0]==1 && settings.zsharp_version[1]<3))) {
        size_t r,f,i;
        for(r=0;r<modules[module_count].room_count;r++)
            for(f=0;f<modules[module_count].rooms[r].function_count;f++)
                for(i=0;i<modules[module_count].rooms[r].functions[f].instruction_count;i++)
                    if(modules[module_count].rooms[r].functions[f].instructions[i].op==ZOP_TIME ||
                       modules[module_count].rooms[r].functions[f].instructions[i].op==ZOP_NETWORK) {
                        zsharp_program_free(&modules[module_count]);
                        return fail("Time/networking requires ZSharp: [1.3.0.0]: or newer in project.zsettings");
                    }
    }
    return (int)++module_count;
}
__attribute__((visibility("default"))) unsigned bzvm_import_count(unsigned module){size_t r;unsigned count=0;
    if(module>=module_count)return 0;
    for(r=0;r<modules[module].room_count;r++)count+=(unsigned)modules[module].rooms[r].import_count;
    return count;
}
__attribute__((visibility("default"))) const char *bzvm_import(unsigned module,unsigned index){size_t r;
    if(module>=module_count)return "";
    for(r=0;r<modules[module].room_count;r++){
        if(index<modules[module].rooms[r].import_count)return modules[module].rooms[r].imports[index].path;
        index-=(unsigned)modules[module].rooms[r].import_count;
    }return "";
}
__attribute__((visibility("default"))) const char *bzvm_json_path(unsigned module,unsigned index){size_t r,v;
    if(module>=module_count)return "";
    for(r=0;r<modules[module].room_count;r++)for(v=0;v<modules[module].rooms[r].variable_count;v++) {
        ZSharpVariable *value=&modules[module].rooms[r].variables[v];
        if(value->object_type && !strncmp(value->object_type,"$json:",6)) {
            if(!index--)return value->constructor_arguments[0].text_value;
        }
    }return "";
}
__attribute__((visibility("default"))) int bzvm_run(void){size_t r,f;
    if(!module_count)return fail("no browser startup script loaded");
    current_program=&modules[0];initialized=1;budget=100000;depth=0;
    {size_t m,v;for(m=0;m<module_count;m++)for(r=0;r<modules[m].room_count;r++)for(v=0;v<modules[m].rooms[r].variable_count;v++) {
        ZSharpVariable *definition=&modules[m].rooms[r].variables[v];
        if(definition->object_type && !strncmp(definition->object_type,"$json:",6) && !global_value(&modules[m].rooms[r],definition->name))return 0;
    }}
    for(r=0;r<program.room_count;r++)for(f=0;f<program.rooms[r].function_count;f++)
        if(!strcmp(program.rooms[r].functions[f].name,"Start") && !program.rooms[r].functions[f].parameter_count &&
            !program.rooms[r].functions[f].disable_auto_run) {
            Task *task=new_task();int result;if(!task)return 0;
            result=execute(&program.rooms[r],&program.rooms[r].functions[f]);
            if(!finish_task(task,result))return 0;
        }
    return 1;
}
__attribute__((visibility("default"))) int bzvm_start(const char *source,const char *name){
    bzvm_reset();if(!bzvm_load(source,name))return 0;return bzvm_run();
}
__attribute__((visibility("default"))) int bzvm_call(const char *target){
    char *s=zsharp_copy_text(target,strlen(target)),*colon,*entry_text;size_t entry=0;int ok;
    if(!initialized || !s)return fail("browser VM is not initialized");
    current_program=&modules[0];
    if(*s=='@'){
        char *file_end=strchr(s,':');ZSharpProgram *module;
        if(!file_end){free(s);return fail("invalid cross-file callback");}
        *file_end=0;module=find_module(s+1);
        if(!module){free(s);return fail("callback script file is not loaded");}
        current_program=module;memmove(s,file_end+1,strlen(file_end+1)+1);
    }
    colon=strchr(s,':');if(!colon){free(s);return fail("invalid callback target");}
    *colon++=0;entry_text=strchr(colon,':');
    if(entry_text){char *end;*entry_text++=0;entry=strtoul(entry_text,&end,10);
        if(!*entry_text || *end){free(s);return fail("invalid inline callback entry");}}
    failure[0]=0;budget=100000;
    {Task *task=new_task();if(!task){free(s);return 0;}
        ok=call_at(s,colon,entry,event_args,event_count,NULL);free(s);return finish_task(task,ok);}
}
__attribute__((visibility("default"))) int bzvm_event(const char *target,const char *arguments) {
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx;JSValue list,len;uint32_t count=0,i;int ok=1;
    if(!rt)return fail("out of memory");ctx=JS_NewContext(rt);
    if(!ctx){JS_FreeRuntime(rt);return fail("out of memory");}
    list=JS_ParseJSON(ctx,arguments,strlen(arguments),"network callback");
    if(!JS_IsArray(list))ok=0;
    len=ok?JS_GetPropertyStr(ctx,list,"length"):JS_UNDEFINED;
    if(ok && (JS_ToUint32(ctx,&count,len)<0 || count>4))ok=0;JS_FreeValue(ctx,len);
    event_count=0;
    for(i=0;ok && i<count;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,list,i);const char *s=JS_ToCString(ctx,item);
        if(!s || (!JS_IsString(item) && !JS_IsNumber(item)))ok=0;
        else {event_args[i]=JS_IsNumber(item)?number(s):text(s);event_count++;}
        JS_FreeCString(ctx,s);JS_FreeValue(ctx,item);
    }
    JS_FreeValue(ctx,list);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    if(ok)ok=bzvm_call(target);else fail("invalid callback argument payload");
    for(i=0;i<event_count;i++)drop(&event_args[i]);event_count=0;return ok;
}
