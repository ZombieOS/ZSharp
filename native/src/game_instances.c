#include "game_instances.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct InstanceSlot {
    const char *scene, *key;
    size_t object;
    int priority, ambiguous, explicit_id;
} InstanceSlot;
struct ZSharpGameInstanceIndex {
    InstanceSlot *slots;
    size_t capacity;
};

int zsharp_game_instance_identifier(const char *key) {
    const unsigned char *p=(const unsigned char *)key;
    if(key && strlen(key)>127)return 0;
    if(!p || !((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||*p=='_'))return 0;
    for(p++;*p;p++)if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'))return 0;
    return 1;
}
static size_t hash_key(const char *scene,const char *key) {
    uint64_t hash=UINT64_C(14695981039346656037);
    const unsigned char *p;
    for(p=(const unsigned char *)scene;*p;p++){hash^=*p;hash*=UINT64_C(1099511628211);}
    hash^=0xff;hash*=UINT64_C(1099511628211);
    for(p=(const unsigned char *)key;*p;p++){hash^=*p;hash*=UINT64_C(1099511628211);}
    return (size_t)hash;
}
static InstanceSlot *slot_for(struct ZSharpGameInstanceIndex *index,const char *scene,const char *key) {
    size_t slot=hash_key(scene,key)&(index->capacity-1);
    while(index->slots[slot].key && (strcmp(index->slots[slot].scene,scene)||strcmp(index->slots[slot].key,key)))
        slot=(slot+1)&(index->capacity-1);
    return &index->slots[slot];
}
static int insert(struct ZSharpGameInstanceIndex *index,const char *scene,const char *key,
                  size_t object,int priority,int explicit_id,char *error,size_t size) {
    InstanceSlot *slot;
    if(!key || !key[0] || !scene)return 1;
    slot=slot_for(index,scene,key);
    if(!slot->key || priority>slot->priority) {
        *slot=(InstanceSlot){scene,key,object,priority,0,explicit_id};return 1;
    }
    if(priority<slot->priority || slot->object==object)return 1;
    if(explicit_id || slot->explicit_id) {
        if(error&&size)snprintf(error,size,"duplicate scene instance identifier '%s.%s'",scene,key);
        return 0;
    }
    slot->ambiguous=1;return 1;
}
void zsharp_game_instances_free(ZSharpGameModel *model) {
    if(model->instance_index){free(model->instance_index->slots);free(model->instance_index);model->instance_index=NULL;}
}
int zsharp_game_instances_build(ZSharpGameModel *model,char *error,size_t size) {
    struct ZSharpGameInstanceIndex *index=calloc(1,sizeof(*index));
    size_t i,capacity=16;
    if(model->object_count>SIZE_MAX/4)goto oom;
    while(capacity<model->object_count*4){if(capacity>SIZE_MAX/2)goto oom;capacity*=2;}
    if(!index)goto oom;
    index->capacity=capacity;index->slots=calloc(capacity,sizeof(*index->slots));
    if(!index->slots)goto oom;
    for(i=0;i<model->object_count;i++) {
        ZSharpGameObject *o=&model->objects[i];
        const char *identity=o->instance_id?o->instance_id:
            zsharp_game_instance_identifier(o->display_name)?o->display_name:NULL;
        if(o->instance_id && !zsharp_game_instance_identifier(o->instance_id)) {
            if(error&&size)snprintf(error,size,"invalid scene instanceId '%s': use letters, digits, and underscores, starting with a letter or underscore",o->instance_id);
            goto failed;
        }
        if(!insert(index,o->scene,identity,i,2,o->instance_id!=NULL,error,size))goto failed;
        if(!insert(index,o->scene,o->name,i,1,0,error,size))goto failed;
    }
    zsharp_game_instances_free(model);model->instance_index=index;return 1;
oom:
    if(error&&size)snprintf(error,size,"out of memory indexing scene instances");
failed:
    if(index){free(index->slots);free(index);}return 0;
}
ZSharpGameObject *zsharp_game_instance(const ZSharpGameModel *model,const char *scene,
                                    const char *key,int *ambiguous) {
    InstanceSlot *slot;
    if(ambiguous)*ambiguous=0;
    if(!scene)scene=model->active_scene;
    if(!scene||!key||!model->instance_index)return NULL;
    slot=slot_for(model->instance_index,scene,key);
    if(!slot->key)return NULL;
    if(slot->ambiguous){if(ambiguous)*ambiguous=1;return NULL;}
    return &model->objects[slot->object];
}
