/* Test the real index independently of graphics timing. */
#include "../native/src/game_instances.c"
#include <time.h>
int main(void) {
    ZSharpGameModel model={0};
    char error[512],buffer[64];
    size_t i,trial,largest_probe=0;
    clock_t started;
    model.object_count=24000;
    model.objects=calloc(model.object_count,sizeof(*model.objects));
    if(!model.objects)return 1;
    for(i=0;i<model.object_count;i++) {
        ZSharpGameObject *o=&model.objects[i];
        o->scene=i<12000?"Floor1":"Floor2";o->name="Cube";
        snprintf(buffer,sizeof(buffer),"Cube_%zu",i%12000);
        o->display_name=malloc(strlen(buffer)+1);
        if(!o->display_name)return 2;
        strcpy(o->display_name,buffer);
    }
    model.active_scene="Floor1";
    if(!zsharp_game_instances_build(&model,error,sizeof(error)))return 3;
    started=clock();
    for(trial=0;trial<10;trial++)for(i=0;i<12000;i++) {
        size_t slot,probes=1;
        snprintf(buffer,sizeof(buffer),"Cube_%zu",i);
        if(zsharp_game_instance(&model,NULL,buffer,NULL)!=&model.objects[i])return 4;
        if(zsharp_game_instance(&model,"Floor2",buffer,NULL)!=&model.objects[i+12000])return 5;
        slot=hash_key("Floor1",buffer)&(model.instance_index->capacity-1);
        while(strcmp(model.instance_index->slots[slot].scene,"Floor1")||strcmp(model.instance_index->slots[slot].key,buffer)) {
            probes++;slot=(slot+1)&(model.instance_index->capacity-1);
        }
        if(probes>largest_probe)largest_probe=probes;
    }
    printf("240000 indexed lookups: %.3f seconds; largest probe chain: %zu\n",
        (double)(clock()-started)/CLOCKS_PER_SEC,largest_probe);
    if(largest_probe>128)return 6; /* structural bound, not a flaky FPS test */
    model.active_scene="Floor2";
    if(zsharp_game_instance(&model,NULL,"Cube_0",NULL)!=&model.objects[12000])return 7;
    {
        int ambiguous=0;
        if(zsharp_game_instance(&model,NULL,"Cube",&ambiguous)||!ambiguous)return 8;
    }
    free(model.objects[1].display_name);
    model.objects[1].display_name=malloc(7);strcpy(model.objects[1].display_name,"Cube_0");
    if(!zsharp_game_instances_build(&model,error,sizeof(error)))return 9;
    {
        int ambiguous=0;
        if(zsharp_game_instance(&model,"Floor1","Cube_0",&ambiguous)||!ambiguous)return 10;
    }
    model.objects[0].instance_id="Door";model.objects[1].instance_id="Door";
    if(zsharp_game_instances_build(&model,error,sizeof(error))||!strstr(error,"duplicate"))return 11;
    model.objects[1].instance_id="bad.identifier";
    if(zsharp_game_instances_build(&model,error,sizeof(error))||!strstr(error,"invalid"))return 12;
    model.objects[1].instance_id="OtherDoor";
    if(!zsharp_game_instances_build(&model,error,sizeof(error)))return 13;
    if(zsharp_game_instance(&model,"Floor1","Door",NULL)!=&model.objects[0])return 14;
    if(zsharp_game_instance(&model,NULL,"Door",NULL)!=NULL)return 15; /* no inactive-scene fallback */
    zsharp_game_instances_free(&model);
    for(i=0;i<model.object_count;i++)free(model.objects[i].display_name);
    free(model.objects);
    return 0;
}
