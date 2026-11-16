#define _CRT_SECURE_NO_WARNINGS
#include "game_spring.h"
#include "game_model.h"

#include <ufbx.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPRING_STEP (1.0 / 120.0)
#define SPRING_MAX_DT 0.25
#define SPRING_PI 3.14159265358979323846
#define SPRING_EPS 1.0e-9

typedef ufbx_vec3 SpringVec;

typedef struct SpringOrder {
    uint32_t id, depth;
} SpringOrder;

typedef struct SpringAsset {
    char *path;
    ufbx_scene *scene;
    SpringOrder *order;
    double extent[3];
    struct SpringAsset *next;
} SpringAsset;

typedef struct SpringFK {
    ufbx_matrix world, unscaled;
    SpringVec scale;
} SpringFK;

typedef struct SpringSegment {
    uint32_t bone, child;
    const ZSharpSpringChain *chain;
    SpringVec tip, previous;
} SpringSegment;

typedef struct SpringCollision {
    SpringVec head, tail;
    double radius;
} SpringCollision;

struct ZSharpGameSpringState {
    SpringAsset *asset;
    const ZSharpSpringConfig *config;
    SpringSegment *segments;
    ZSharpSpringBonePose *poses;
    size_t segment_count;
    int *node_segment;
    ufbx_transform *authored;
    SpringFK *fk, *animated;
    SpringCollision *colliders;
    uint32_t *collider_bones;
    ufbx_matrix object_matrix;
    double radius_scale;
    double previous_position[3], previous_scale[3];
    double accumulator;
    uint64_t scene_generation;
    int initialized, reset;
    struct ZSharpGameSpringState *next;
};

struct ZSharpGameSpringSystem {
    SpringAsset *assets;
    ZSharpGameSpringState *states;
};

static int spring_error(char *error, size_t size, const char *format, ...) {
    va_list args;
    if (error != NULL && size != 0) {
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
    }
    return 0;
}

static char *spring_text(const char *text) {
    size_t size;
    char *copy;
    if (text == NULL) return NULL;
    size = strlen(text) + 1;
    copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static SpringVec vec(double x, double y, double z) {
    SpringVec result = {0};
    result.x = x; result.y = y; result.z = z;
    return result;
}
static SpringVec add(SpringVec a, SpringVec b) { return vec(a.x+b.x,a.y+b.y,a.z+b.z); }
static SpringVec sub(SpringVec a, SpringVec b) { return vec(a.x-b.x,a.y-b.y,a.z-b.z); }
static SpringVec mul(SpringVec a, double s) { return vec(a.x*s,a.y*s,a.z*s); }
static double dot(SpringVec a, SpringVec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static SpringVec cross(SpringVec a, SpringVec b) {
    return vec(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);
}
static double length(SpringVec a) { return sqrt(dot(a,a)); }
static int finite_vec(SpringVec a) { return isfinite(a.x)&&isfinite(a.y)&&isfinite(a.z); }
static int finite_matrix(const ufbx_matrix *m) {
    size_t i;
    for (i=0;i<12;i++) if (!isfinite(m->v[i])) return 0;
    return 1;
}
static SpringVec unit(SpringVec a, SpringVec fallback) {
    double n=length(a);
    return isfinite(n)&&n>SPRING_EPS ? mul(a,1.0/n) : fallback;
}
static ufbx_quat identity_quat(void) {
    ufbx_quat q={0}; q.w=1.0; return q;
}
static ufbx_quat safe_quat(ufbx_quat q) {
    double n=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if (!isfinite(n)||n<SPRING_EPS*SPRING_EPS) return identity_quat();
    n=1.0/sqrt(n); q.x*=n;q.y*=n;q.z*=n;q.w*=n;
    return q;
}
static void set_pose(ZSharpSpringBonePose *pose, ufbx_quat q) {
    q=safe_quat(q);
    pose->rotation[0]=(float)q.x;pose->rotation[1]=(float)q.y;
    pose->rotation[2]=(float)q.z;pose->rotation[3]=(float)q.w;
}

/* Minimal swing between two joint-local vectors, with a deterministic
 * perpendicular for the antiparallel case. No world-axis assumption enters FK. */
static ufbx_quat swing(SpringVec from, SpringVec to, double max_degrees) {
    SpringVec axis;
    ufbx_quat q={0};
    double d, angle, limit=max_degrees*SPRING_PI/180.0;
    from=unit(from,vec(0,1,0));to=unit(to,from);
    d=dot(from,to);if(d>1)d=1;if(d< -1)d= -1;
    axis=cross(from,to);
    if (length(axis)<SPRING_EPS) {
        if (d>=0) return identity_quat();
        axis=cross(from, fabs(from.x)<fabs(from.y) ? vec(1,0,0):vec(0,1,0));
        if (length(axis)<SPRING_EPS) axis=cross(from,vec(0,0,1));
    }
    axis=unit(axis,vec(1,0,0));angle=acos(d);
    if (angle>limit) angle=limit;
    q.x=axis.x*sin(angle*.5);q.y=axis.y*sin(angle*.5);
    q.z=axis.z*sin(angle*.5);q.w=cos(angle*.5);
    return safe_quat(q);
}

static int valid_name(const char *name) {
    return name!=NULL && name[0]!='\0' && strlen(name)<=255;
}
static int bounded(double value,double low,double high) {
    return isfinite(value)&&value>=low&&value<=high;
}

static int config_shape(const ZSharpSpringConfig *config,char *error,size_t size) {
    size_t c,b,k,total=0;
    if (config==NULL) return 1;
    if (config->chain_count==0||config->chain_count>ZSHARP_SPRING_MAX_CHAINS||
        config->chains==NULL||config->collider_count>ZSHARP_SPRING_MAX_COLLIDERS||
        (config->collider_count&&config->colliders==NULL))
        return spring_error(error,size,"springBones has invalid chain/collider counts");
    for(c=0;c<config->chain_count;c++) {
        const ZSharpSpringChain *chain=&config->chains[c];
        if(!valid_name(chain->name)||chain->bone_count<2||
           chain->bone_count>ZSHARP_SPRING_MAX_BONES||chain->bones==NULL)
            return spring_error(error,size,"spring chain requires a name and 2..64 bones including its tip marker");
        total+=chain->bone_count;
        if(total>ZSHARP_SPRING_MAX_TOTAL_BONES)
            return spring_error(error,size,"springBones exceeds the total bone limit");
        if(!bounded(chain->stiffness,0,ZSHARP_SPRING_MAX_STIFFNESS)||
           !bounded(chain->damping,0,ZSHARP_SPRING_MAX_DAMPING)||
           !bounded(chain->radius,0,ZSHARP_SPRING_MAX_RADIUS)||
           !bounded(chain->max_angle,0,180))
            return spring_error(error,size,"spring chain '%s' has invalid finite parameter bounds",chain->name);
        for(k=0;k<3;k++) if(!bounded(chain->gravity[k],-ZSHARP_SPRING_MAX_GRAVITY,ZSHARP_SPRING_MAX_GRAVITY))
            return spring_error(error,size,"spring chain '%s' has invalid gravity",chain->name);
        for(k=0;k<c;k++) if(!strcmp(chain->name,config->chains[k].name))
            return spring_error(error,size,"duplicate spring chain '%s'",chain->name);
        for(b=0;b<chain->bone_count;b++) {
            size_t pc,pb;
            if(!valid_name(chain->bones[b]))
                return spring_error(error,size,"spring chain '%s' has an invalid bone name",chain->name);
            for(pc=0;pc<=c;pc++) for(pb=0;pb<(pc==c?b:config->chains[pc].bone_count);pb++)
                if(!strcmp(chain->bones[b],config->chains[pc].bones[pb]))
                    return spring_error(error,size,"spring bone '%s' is repeated or overlaps another chain",chain->bones[b]);
        }
    }
    for(c=0;c<config->collider_count;c++) {
        const ZSharpSpringCollider *collider=&config->colliders[c];
        if(!valid_name(collider->name)||!valid_name(collider->bone)||
           !bounded(collider->radius,0,ZSHARP_SPRING_MAX_RADIUS)||collider->radius<=0||
           (collider->capsule!=0&&collider->capsule!=1))
            return spring_error(error,size,"spring collider requires a valid name, bone and radius");
        for(k=0;k<c;k++) if(!strcmp(collider->name,config->colliders[k].name))
            return spring_error(error,size,"duplicate spring collider '%s'",collider->name);
        for(k=0;k<3;k++) if(!bounded(collider->offset[k],-ZSHARP_SPRING_MAX_OFFSET,ZSHARP_SPRING_MAX_OFFSET)||
            !bounded(collider->tail[k],-ZSHARP_SPRING_MAX_OFFSET,ZSHARP_SPRING_MAX_OFFSET))
            return spring_error(error,size,"spring collider '%s' has invalid offsets",collider->name);
    }
    return 1;
}

void zsharp_game_spring_config_free(ZSharpSpringConfig *config) {
    size_t c,b;
    if(config==NULL)return;
    if(config->chains)for(c=0;c<config->chain_count;c++) {
        free(config->chains[c].name);
        if(config->chains[c].bones)for(b=0;b<config->chains[c].bone_count;b++)free(config->chains[c].bones[b]);
        free(config->chains[c].bones);
    }
    if(config->colliders)for(c=0;c<config->collider_count;c++) {
        free(config->colliders[c].name);free(config->colliders[c].bone);
    }
    free(config->chains);free(config->colliders);free(config);
}

int zsharp_game_spring_config_copy(ZSharpSpringConfig **destination,
                                  const ZSharpSpringConfig *source) {
    ZSharpSpringConfig *copy;
    size_t c,b;
    if(destination==NULL)return 0;
    *destination=NULL;
    if(source==NULL)return 1;
    if(!config_shape(source,NULL,0))return 0;
    copy=(ZSharpSpringConfig *)calloc(1,sizeof(*copy));if(copy==NULL)return 0;
    copy->chain_count=source->chain_count;copy->collider_count=source->collider_count;
    copy->chains=(ZSharpSpringChain *)calloc(copy->chain_count,sizeof(*copy->chains));
    if(copy->collider_count)copy->colliders=(ZSharpSpringCollider *)calloc(copy->collider_count,sizeof(*copy->colliders));
    if(copy->chains==NULL||(copy->collider_count&&copy->colliders==NULL))goto failed;
    for(c=0;c<copy->chain_count;c++) {
        ZSharpSpringChain *chain=&copy->chains[c];
        *chain=source->chains[c];chain->name=NULL;chain->bones=NULL;
        chain->name=spring_text(source->chains[c].name);
        chain->bones=(char **)calloc(chain->bone_count,sizeof(*chain->bones));
        if(chain->name==NULL||chain->bones==NULL)goto failed;
        for(b=0;b<chain->bone_count;b++)if((chain->bones[b]=spring_text(source->chains[c].bones[b]))==NULL)goto failed;
    }
    for(c=0;c<copy->collider_count;c++) {
        copy->colliders[c]=source->colliders[c];
        copy->colliders[c].name=spring_text(source->colliders[c].name);
        copy->colliders[c].bone=spring_text(source->colliders[c].bone);
        if(copy->colliders[c].name==NULL||copy->colliders[c].bone==NULL)goto failed;
    }
    *destination=copy;return 1;
failed:
    zsharp_game_spring_config_free(copy);return 0;
}

static const ufbx_node *unique_bone(const ufbx_scene *scene,const char *name) {
    const ufbx_node *found=NULL;size_t i;
    for(i=0;i<scene->nodes.count;i++) {
        const ufbx_node *node=scene->nodes.data[i];
        if(node->name.length==strlen(name)&&!memcmp(node->name.data,name,node->name.length)) {
            if(found!=NULL||node->bone==NULL)return NULL;
            found=node;
        }
    }
    return found;
}

int zsharp_game_spring_validate(const ZSharpSpringConfig *config,
                               const ufbx_scene *scene,char *error,size_t error_size) {
    size_t c,b,i;int skinned=0;
    if(config==NULL)return 1;
    if(!config_shape(config,error,error_size))return 0;
    if(scene==NULL)return spring_error(error,error_size,"springBones requires an FBX skeleton");
    for(i=0;i<scene->nodes.count;i++)if(scene->nodes.data[i]->mesh&&
        scene->nodes.data[i]->mesh->skin_deformers.count>0)skinned=1;
    if(!skinned)return spring_error(error,error_size,"springBones requires a skinned FBX mesh");
    for(c=0;c<config->chain_count;c++) {
        const ZSharpSpringChain *chain=&config->chains[c];
        const ufbx_node *previous=NULL;
        for(b=0;b<chain->bone_count;b++) {
            const ufbx_node *bone=unique_bone(scene,chain->bones[b]);
            if(bone==NULL)return spring_error(error,error_size,"spring bone '%s' is missing, ambiguous or not a bone",chain->bones[b]);
            if(!finite_matrix(&bone->node_to_world)||!finite_vec(bone->local_transform.scale)||
               fabs(bone->local_transform.scale.x)<SPRING_EPS||
               fabs(bone->local_transform.scale.y)<SPRING_EPS||fabs(bone->local_transform.scale.z)<SPRING_EPS)
                return spring_error(error,error_size,"spring bone '%s' has a singular or non-finite transform",chain->bones[b]);
            if(previous) {
                double segment=length(sub(bone->node_to_world.cols[3],previous->node_to_world.cols[3]));
                if(bone->parent!=previous)return spring_error(error,error_size,"spring chain '%s' bones must be adjacent direct parent/child nodes",chain->name);
                if(!isfinite(segment)||segment<SPRING_EPS)
                    return spring_error(error,error_size,"spring chain '%s' has a zero-length segment",chain->name);
            }
            previous=bone;
        }
    }
    for(c=0;c<config->collider_count;c++)if(unique_bone(scene,config->colliders[c].bone)==NULL)
        return spring_error(error,error_size,"spring collider bone '%s' is missing or ambiguous",config->colliders[c].bone);
    return 1;
}

static int order_compare(const void *a,const void *b) {
    const SpringOrder *x=(const SpringOrder *)a,*y=(const SpringOrder *)b;
    if(x->depth!=y->depth)return x->depth<y->depth?-1:1;
    return x->id<y->id?-1:x->id>y->id;
}

/* Match the renderer's bounds over triangulated, renderable mesh faces. */
static int asset_bounds(SpringAsset *asset) {
    double low[3]={DBL_MAX,DBL_MAX,DBL_MAX},high[3]={-DBL_MAX,-DBL_MAX,-DBL_MAX};
    size_t i,f,total=0;
    for(i=0;i<asset->scene->nodes.count;i++) {
        const ufbx_node *node=asset->scene->nodes.data[i];const ufbx_mesh *mesh=node->mesh;
        if(!mesh)continue;
        for(f=0;f<mesh->faces.count;f++) {
            ufbx_face face=mesh->faces.data[f];uint32_t *indices,count,t;size_t capacity;
            if(face.num_indices<3||face.num_indices>100000)continue;
            capacity=(size_t)(face.num_indices-2)*3;
            indices=(uint32_t *)malloc(capacity*sizeof(*indices));if(!indices)return 0;
            count=ufbx_triangulate_face(indices,capacity,mesh,face);
            if(total+count>200000){free(indices);return 0;}
            total+=count;
            for(t=0;t<count*3;t++) {
                ufbx_vec3 p=ufbx_get_vertex_vec3(&mesh->vertex_position,indices[t]);int axis;
                p=ufbx_transform_position(&node->geometry_to_world,p);
                if(!finite_vec(p)){free(indices);return 0;}
                for(axis=0;axis<3;axis++) {
                    /* The renderer stores each imported point as float first. */
                    double value=(float)p.v[axis];
                    if(value<low[axis])low[axis]=value;if(value>high[axis])high[axis]=value;
                }
            }
            free(indices);
        }
    }
    if(!total)return 0;
    for(i=0;i<3;i++) {
        float extent=(float)high[i]-(float)low[i];
        asset->extent[i]=extent>0.000001f?extent:1.0;
    }
    return 1;
}

static SpringAsset *load_asset(ZSharpGameSpringSystem *system,const char *root,
                              const char *mesh,char *error,size_t size) {
    SpringAsset *asset;size_t a,b,i;char *path;ufbx_error detail;
    if(!root||!mesh)return NULL;
    a=strlen(root);b=strlen(mesh);if(a>SIZE_MAX-b-2)return NULL;
    path=(char *)malloc(a+b+2);if(!path)return NULL;
    snprintf(path,a+b+2,"%s/%s",root,mesh);
    for(asset=system->assets;asset;asset=asset->next)if(!strcmp(asset->path,path)){free(path);return asset;}
    asset=(SpringAsset *)calloc(1,sizeof(*asset));if(!asset){free(path);return NULL;}
    asset->path=path;asset->scene=ufbx_load_file(path,NULL,&detail);
    if(!asset->scene){spring_error(error,size,"could not load springBones FBX '%s': %.160s",mesh,detail.description.data);goto failed;}
    asset->order=(SpringOrder *)calloc(asset->scene->nodes.count,sizeof(*asset->order));
    if(!asset->order||!asset_bounds(asset)) {spring_error(error,size,"springBones FBX has invalid mesh bounds or exceeds renderer limits");goto failed;}
    for(i=0;i<asset->scene->nodes.count;i++) {
        asset->order[i].id=(uint32_t)i;asset->order[i].depth=asset->scene->nodes.data[i]->node_depth;
    }
    qsort(asset->order,asset->scene->nodes.count,sizeof(*asset->order),order_compare);
    asset->next=system->assets;system->assets=asset;return asset;
failed:
    if(asset->scene)ufbx_free_scene(asset->scene);free(asset->order);free(asset->path);free(asset);return NULL;
}

static void free_state(ZSharpGameSpringState *state) {
    free(state->segments);free(state->poses);free(state->node_segment);
    free(state->authored);free(state->fk);free(state->animated);
    free(state->colliders);free(state->collider_bones);free(state);
}

void zsharp_game_springs_free(ZSharpGameModel *model) {
    ZSharpGameSpringSystem *system;size_t i;
    if(!model)return;system=model->spring_system;if(!system)return;
    for(i=0;i<model->object_count;i++)model->objects[i].spring_state=NULL;
    for(i=0;i<model->definition_count;i++)model->definitions[i].spring_state=NULL;
    while(system->states){ZSharpGameSpringState *next=system->states->next;free_state(system->states);system->states=next;}
    while(system->assets){SpringAsset *next=system->assets->next;ufbx_free_scene(system->assets->scene);
        free(system->assets->order);free(system->assets->path);free(system->assets);system->assets=next;}
    free(system);model->spring_system=NULL;
}

int zsharp_game_springs_prepare(ZSharpGameModel *model,char *error,size_t error_size) {
    ZSharpGameSpringSystem *system;size_t i,c,b;
    if(!model)return spring_error(error,error_size,"springBones requires a model");
    zsharp_game_springs_free(model);
    for(i=0;i<model->object_count;i++)if(model->objects[i].spring_config)break;
    if(i==model->object_count)return 1;
    system=(ZSharpGameSpringSystem *)calloc(1,sizeof(*system));
    if(!system)return spring_error(error,error_size,"out of memory preparing springBones");
    model->spring_system=system;
    for(i=0;i<model->object_count;i++) {
        ZSharpGameObject *object=&model->objects[i];ZSharpGameSpringState *state;SpringAsset *asset;size_t n,s=0;
        if(!object->spring_config)continue;
        if(object->shape!=ZGAME_SHAPE_MESH||!object->mesh_path) {
            spring_error(error,error_size,"springBones requires an imported FBX model");goto failed;
        }
        asset=load_asset(system,model->project_root,object->mesh_path,error,error_size);if(!asset)goto failed;
        if(!zsharp_game_spring_validate(object->spring_config,asset->scene,error,error_size))goto failed;
        state=(ZSharpGameSpringState *)calloc(1,sizeof(*state));if(!state)goto memory;
        state->asset=asset;state->config=object->spring_config;state->reset=1;
        state->next=system->states;system->states=state;object->spring_state=state;
        for(c=0;c<state->config->chain_count;c++)state->segment_count+=state->config->chains[c].bone_count-1;
        n=asset->scene->nodes.count;
        state->segments=(SpringSegment *)calloc(state->segment_count,sizeof(*state->segments));
        state->poses=(ZSharpSpringBonePose *)calloc(state->segment_count,sizeof(*state->poses));
        state->node_segment=(int *)malloc(n*sizeof(*state->node_segment));
        state->authored=(ufbx_transform *)calloc(n,sizeof(*state->authored));
        state->fk=(SpringFK *)calloc(n,sizeof(*state->fk));state->animated=(SpringFK *)calloc(n,sizeof(*state->animated));
        if(state->config->collider_count) {
            state->colliders=(SpringCollision *)calloc(state->config->collider_count,sizeof(*state->colliders));
            state->collider_bones=(uint32_t *)calloc(state->config->collider_count,sizeof(*state->collider_bones));
            if(!state->colliders||!state->collider_bones)goto memory;
        }
        if(!state->segments||!state->poses||!state->node_segment||!state->authored||!state->fk||!state->animated)goto memory;
        for(b=0;b<n;b++)state->node_segment[b]=-1;
        for(c=0;c<state->config->chain_count;c++)for(b=0;b+1<state->config->chains[c].bone_count;b++,s++) {
            const ZSharpSpringChain *chain=&state->config->chains[c];
            const ufbx_node *node=unique_bone(asset->scene,chain->bones[b]);
            state->segments[s].bone=node->typed_id;
            state->segments[s].child=unique_bone(asset->scene,chain->bones[b+1])->typed_id;
            state->segments[s].chain=chain;state->node_segment[node->typed_id]=(int)s;
            state->poses[s].name=node->name.data;set_pose(&state->poses[s],identity_quat());
        }
        for(c=0;c<state->config->collider_count;c++)
            state->collider_bones[c]=unique_bone(asset->scene,state->config->colliders[c].bone)->typed_id;
    }
    return 1;
memory:
    spring_error(error,error_size,"out of memory preparing springBones");
failed:
    zsharp_game_springs_free(model);return 0;
}

/* Same public inheritance equations as ufbx_node documents. Scaling is never
 * decomposed from a sheared world matrix, which preserves FBX scale compensation. */
static SpringFK compose_node(const ufbx_node *node,ufbx_transform local,const SpringFK *nodes) {
    SpringFK result;ufbx_transform unscaled=local;ufbx_matrix matrix,umatrix;
    const SpringFK *parent=node->parent?&nodes[node->parent->typed_id]:NULL;
    if(parent&&node->inherit_mode!=UFBX_INHERIT_MODE_NORMAL) {
        SpringVec scale=node->inherit_scale_node?nodes[node->inherit_scale_node->typed_id].scale:vec(1,1,1);
        local.scale.x*=scale.x;local.scale.y*=scale.y;local.scale.z*=scale.z;
        local.translation.x*=parent->scale.x;local.translation.y*=parent->scale.y;local.translation.z*=parent->scale.z;
    }
    result.scale=local.scale;unscaled=local;unscaled.scale=vec(1,1,1);
    matrix=ufbx_transform_to_matrix(&local);umatrix=ufbx_transform_to_matrix(&unscaled);
    if(parent) {
        const ufbx_matrix *basis=node->inherit_mode==UFBX_INHERIT_MODE_NORMAL?&parent->world:&parent->unscaled;
        result.world=ufbx_matrix_mul(basis,&matrix);result.unscaled=ufbx_matrix_mul(basis,&umatrix);
    } else {result.world=matrix;result.unscaled=umatrix;}
    return result;
}

static int update_authored(ZSharpGameSpringState *state,const ZSharpGameObject *object) {
    size_t i,p,n=state->asset->scene->nodes.count;
    for(i=0;i<n;i++) {
        const ufbx_node *node=state->asset->scene->nodes.data[i];ufbx_transform local=node->local_transform;
        if(node->bone)for(p=0;p<object->part_pose_count;p++) {
            const ZSharpGamePartPose *pose=&object->part_poses[p];ufbx_vec3 angles;ufbx_quat offset;
            if(!pose->name||strlen(pose->name)!=node->name.length||memcmp(pose->name,node->name.data,node->name.length))continue;
            local.translation=add(local.translation,vec(pose->position[0],pose->position[1],pose->position[2]));
            angles=vec(pose->rotation[0],pose->rotation[1],pose->rotation[2]);
            if(!finite_vec(angles)||!finite_vec(local.translation))return 0;
            offset=ufbx_euler_to_quat(angles,node->rotation_order);
            local.rotation=safe_quat(ufbx_quat_mul(local.rotation,offset));break;
        }
        state->authored[i]=local;
    }
    for(i=0;i<n;i++) {
        uint32_t id=state->asset->order[i].id;
        state->animated[id]=compose_node(state->asset->scene->nodes.data[id],state->authored[id],state->animated);
        if(!finite_matrix(&state->animated[id].world))return 0;
    }
    return 1;
}

static SpringVec world_position(const ZSharpGameSpringState *state,SpringVec point) {
    return ufbx_transform_position(&state->object_matrix,point);
}

static int object_transform(ZSharpGameSpringState *state,const ZSharpGameObject *object,double scale[3]) {
    ufbx_transform transform;ufbx_quat qx,qy,qz;ufbx_vec3 angles;
    size_t a;
    scale[0]=object->width*object->scale_x/state->asset->extent[0];
    scale[1]=object->height*object->scale_y/state->asset->extent[1];
    scale[2]=object->depth*object->scale_z/state->asset->extent[2];
    for(a=0;a<3;a++)if(!isfinite(scale[a])||fabs(scale[a])<SPRING_EPS)return 0;
    transform.translation=vec(object->x,object->y,object->z);
    if(!finite_vec(transform.translation)||!isfinite(object->rotation)||!isfinite(object->rotation_x)||
       !isfinite(object->rotation_y)||!isfinite(object->rotation_z))return 0;
    angles=vec(object->rotation_x,0,0);qx=ufbx_euler_to_quat(angles,UFBX_ROTATION_ORDER_XYZ);
    angles=vec(0,(double)object->rotation_y+object->rotation,0);qy=ufbx_euler_to_quat(angles,UFBX_ROTATION_ORDER_XYZ);
    angles=vec(0,0,object->rotation_z);qz=ufbx_euler_to_quat(angles,UFBX_ROTATION_ORDER_XYZ);
    transform.rotation=safe_quat(ufbx_quat_mul(qz,ufbx_quat_mul(qy,qx)));
    transform.scale=vec(scale[0],scale[1],scale[2]);
    state->object_matrix=ufbx_transform_to_matrix(&transform);
    state->radius_scale=fmax(fabs(object->scale_x),fmax(fabs(object->scale_y),fabs(object->scale_z)));
    return isfinite(state->radius_scale)&&finite_matrix(&state->object_matrix);
}

static void identity_poses(ZSharpGameSpringState *state) {
    size_t i;for(i=0;i<state->segment_count;i++)set_pose(&state->poses[i],identity_quat());
}

static void seed_state(ZSharpGameSpringState *state) {
    size_t i;identity_poses(state);state->accumulator=0;
    for(i=0;i<state->segment_count;i++) {
        SpringSegment *segment=&state->segments[i];
        segment->tip=world_position(state,state->animated[segment->child].world.cols[3]);
        segment->previous=segment->tip;
    }
    state->initialized=1;state->reset=0;
}

static void update_colliders(ZSharpGameSpringState *state) {
    size_t i;
    for(i=0;i<state->config->collider_count;i++) {
        const ZSharpSpringCollider *config=&state->config->colliders[i];
        const ufbx_matrix *anchor=&state->animated[state->collider_bones[i]].world;
        SpringCollision *collider=&state->colliders[i];
        collider->head=world_position(state,ufbx_transform_position(anchor,vec(config->offset[0],config->offset[1],config->offset[2])));
        collider->tail=config->capsule?world_position(state,ufbx_transform_position(anchor,
            vec(config->tail[0],config->tail[1],config->tail[2]))):collider->head;
        collider->radius=config->radius*state->radius_scale;
    }
}

/* Project a tip out of authored bone-anchored spheres/capsules. Capsule end
 * points move with their anchor; radii remain game units, scaled per instance. */
static SpringVec contact_fallback(const ufbx_matrix *basis,SpringVec bone,
                                  SpringVec capsule_axis,double axis_squared) {
    SpringVec best=vec(0,0,0);double best_length=0;size_t i;
    bone=unit(bone,vec(0,1,0));
    for(i=0;i<3;i++) {
        SpringVec candidate=unit(basis->cols[i],vec(0,0,0));double size;
        candidate=sub(candidate,mul(bone,dot(candidate,bone)));
        if(axis_squared>SPRING_EPS)
            candidate=sub(candidate,mul(capsule_axis,dot(candidate,capsule_axis)/axis_squared));
        size=length(candidate);
        if(size>best_length){best=candidate;best_length=size;}
    }
    /* At the center of a collider a radial push along the bone would be
     * undone by its fixed length. Choose a tangent from the animated joint's
     * own basis, also perpendicular to a capsule's spine. */
    return unit(best,bone);
}

static int collide(ZSharpGameSpringState *state,SpringVec *tip,double radius,
                   const ufbx_matrix *basis,SpringVec bone,SpringVec *normal) {
    size_t c;int hit=0;
    for(c=0;c<state->config->collider_count;c++) {
        const SpringCollision *collider=&state->colliders[c];
        SpringVec axis=sub(collider->tail,collider->head),center,offset;
        double axis2=dot(axis,axis),along=axis2>SPRING_EPS?dot(sub(*tip,collider->head),axis)/axis2:0;
        double distance,combined=radius+collider->radius;
        if(!finite_vec(collider->head)||!finite_vec(collider->tail)||!isfinite(combined))continue;
        if(along<0)along=0;if(along>1)along=1;
        center=add(collider->head,mul(axis,along));offset=sub(*tip,center);distance=length(offset);
        if(combined>0&&isfinite(distance)&&distance<combined) {
            *normal=unit(offset,contact_fallback(basis,bone,axis,axis2));
            *tip=add(center,mul(*normal,combined));hit=1;
        }
    }
    return hit;
}

static SpringVec posed_child(ZSharpGameSpringState *state,const SpringSegment *segment) {
    const ufbx_node *child=state->asset->scene->nodes.data[segment->child];
    SpringFK fk=compose_node(child,state->authored[segment->child],state->fk);
    return world_position(state,fk.world.cols[3]);
}

static int solve_segment(ZSharpGameSpringState *state,size_t index,SpringFK base) {
    SpringSegment *segment=&state->segments[index];const ZSharpSpringChain *chain=segment->chain;
    const ufbx_node *node=state->asset->scene->nodes.data[segment->bone];
    SpringVec origin=world_position(state,base.world.cols[3]);
    SpringVec rest=posed_child(state,segment),velocity,target,local_rest,normal=vec(0,0,0),result;
    ufbx_matrix basis=ufbx_matrix_mul(&state->object_matrix,&base.unscaled),inverse;
    ufbx_transform local=state->authored[segment->bone];
    double determinant=dot(basis.cols[0],cross(basis.cols[1],basis.cols[2]));
    double norm=length(basis.cols[0])*length(basis.cols[1])*length(basis.cols[2]);
    size_t iteration;int contact=0;
    if(!finite_vec(origin)||!finite_vec(rest)||!isfinite(determinant)||norm<SPRING_EPS||fabs(determinant)<norm*1.0e-10)return 0;
    inverse=ufbx_matrix_invert(&basis);
    local_rest=ufbx_transform_direction(&inverse,sub(rest,origin));
    if(!finite_vec(local_rest)||length(local_rest)<SPRING_EPS)return 0;
    velocity=mul(sub(segment->tip,segment->previous),exp(-chain->damping*SPRING_STEP));
    target=add(add(segment->tip,velocity),mul(add(vec(chain->gravity[0],chain->gravity[1],chain->gravity[2]),
        mul(sub(rest,segment->tip),chain->stiffness)),SPRING_STEP*SPRING_STEP));
    if(!finite_vec(target))return 0;
    result=rest;
    for(iteration=0;iteration<8;iteration++) {
        SpringVec wanted=ufbx_transform_direction(&inverse,sub(target,origin));
        ufbx_quat delta=swing(local_rest,wanted,chain->max_angle);
        set_pose(&state->poses[index],delta);
        local.rotation=safe_quat(ufbx_quat_mul(state->authored[segment->bone].rotation,delta));
        state->fk[segment->bone]=compose_node(node,local,state->fk);
        result=posed_child(state,segment);
        if(!finite_vec(result))return 0;
        target=result;
        if(!collide(state,&target,chain->radius*state->radius_scale,&basis,sub(rest,origin),&normal))break;
        contact=1;
    }
    /* Save the actual FK endpoint rather than an independently length-clamped
     * particle: even anisotropic instance dimensions must agree with rendering. */
    if(contact) {
        SpringVec motion=sub(result,segment->tip);
        motion=sub(motion,mul(normal,dot(motion,normal)));
        segment->previous=sub(result,mul(motion,.5));
    } else segment->previous=segment->tip;
    segment->tip=result;return 1;
}

static int simulate_step(ZSharpGameSpringState *state) {
    size_t i,n=state->asset->scene->nodes.count;
    for(i=0;i<n;i++) {
        uint32_t id=state->asset->order[i].id;int segment=state->node_segment[id];
        SpringFK base=compose_node(state->asset->scene->nodes.data[id],state->authored[id],state->fk);
        state->fk[id]=base;
        if(segment>=0&&!solve_segment(state,(size_t)segment,base))return 0;
        if(!finite_matrix(&state->fk[id].world))return 0;
    }
    return 1;
}

void zsharp_game_springs_reset(ZSharpGameModel *model) {
    ZSharpGameSpringState *state;
    if(!model||!model->spring_system)return;
    for(state=model->spring_system->states;state;state=state->next) {
        state->reset=1;state->initialized=0;state->accumulator=0;identity_poses(state);
    }
}

void zsharp_game_springs_update(ZSharpGameModel *model,double delta_seconds) {
    size_t i;
    if(!model||!model->spring_system)return;
    if(!isfinite(delta_seconds)||delta_seconds<0||delta_seconds>SPRING_MAX_DT) {
        zsharp_game_springs_reset(model);return;
    }
    for(i=0;i<model->object_count;i++) {
        ZSharpGameObject *object=&model->objects[i];ZSharpGameSpringState *state=object->spring_state;
        double scale[3],distance=0,reach=0,chain_reach=0;size_t a,s;int reset;
        if(!state)continue;
        if(!object->spring_bones_enabled||!object->scene||!model->active_scene||strcmp(object->scene,model->active_scene)||
           !object_transform(state,object,scale)||!update_authored(state,object)) {
            state->initialized=0;state->reset=1;state->accumulator=0;identity_poses(state);continue;
        }
        reset=state->reset||!state->initialized||state->scene_generation!=model->scene_generation;
        for(a=0;a<3;a++) {
            double position=a==0?object->x:a==1?object->y:object->z;
            double difference=position-state->previous_position[a];distance+=difference*difference;
            if(fabs(scale[a]-state->previous_scale[a])>1.0e-6*fmax(1.0,fabs(scale[a])))reset=1;
            state->previous_position[a]=position;state->previous_scale[a]=scale[a];
        }
        state->scene_generation=model->scene_generation;
        for(s=0;s<state->segment_count;s++) {
            const SpringSegment *segment=&state->segments[s];
            if(s&&segment->chain!=state->segments[s-1].chain){reach=fmax(reach,chain_reach);chain_reach=0;}
            chain_reach+=length(sub(world_position(state,state->animated[segment->child].world.cols[3]),
                world_position(state,state->animated[segment->bone].world.cols[3])));
        }
        reach=fmax(reach,chain_reach);
        if(!isfinite(reach)||!isfinite(distance)||sqrt(distance)>fmax(2.0,4.0*reach))reset=1;
        if(reset){seed_state(state);continue;}
        update_colliders(state);
        state->accumulator+=delta_seconds;
        while(state->accumulator+1.0e-12>=SPRING_STEP) {
            if(!simulate_step(state)){seed_state(state);break;}
            state->accumulator-=SPRING_STEP;
        }
        if(state->accumulator<0)state->accumulator=0;
    }
}

const ZSharpSpringBonePose *zsharp_game_spring_poses(const ZSharpGameSpringState *state,size_t *count) {
    if(count)*count=state?state->segment_count:0;
    return state?state->poses:NULL;
}
