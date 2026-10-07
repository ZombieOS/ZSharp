/* Depth-tested triangle rasterization on SDL_GPU/Vulkan. CPU scene math and
   lighting remain shared with the software fallback. No runtime shader tool. */
#include "shaders/game_vertex_spirv.h"
#include "shaders/game_fragment_spirv.h"
#include "shaders/game_blend_spirv.h"

typedef struct GameGPUVertex { float position[4], color[4], uv[2]; } GameGPUVertex;
typedef struct GameGPUDraw { SDL_GPUTexture *texture; Uint32 first, count; int transparent; } GameGPUDraw;
typedef struct GameGPU {
    SDL_GPUDevice *device;
    SDL_GPUGraphicsPipeline *opaque, *blend;
    SDL_GPUSampler *sampler;
    SDL_GPUTexture *depth;
    SDL_Texture *target, *white;
    SDL_GPUBuffer *buffer;
    SDL_GPUTransferBuffer *transfer;
    Uint32 buffer_size;
    GameGPUVertex *vertices;
    GameGPUDraw *draws;
    size_t vertex_count, vertex_capacity, draw_count, draw_capacity;
    int failed;
} GameGPU;

static void game_gpu_destroy(GameGPU *gpu) {
    if (!gpu) return;
    if (gpu->device) {
        SDL_WaitForGPUIdle(gpu->device);
        if(gpu->opaque) SDL_ReleaseGPUGraphicsPipeline(gpu->device,gpu->opaque);
        if(gpu->blend) SDL_ReleaseGPUGraphicsPipeline(gpu->device,gpu->blend);
        if(gpu->sampler) SDL_ReleaseGPUSampler(gpu->device,gpu->sampler);
        if(gpu->depth) SDL_ReleaseGPUTexture(gpu->device,gpu->depth);
        if(gpu->buffer) SDL_ReleaseGPUBuffer(gpu->device,gpu->buffer);
        if(gpu->transfer) SDL_ReleaseGPUTransferBuffer(gpu->device,gpu->transfer);
    }
    if(gpu->target) SDL_DestroyTexture(gpu->target);
    if(gpu->white) SDL_DestroyTexture(gpu->white);
    free(gpu->vertices);free(gpu->draws);
    gpu->vertices=NULL;gpu->draws=NULL;
}

static int game_gpu_upload(GameGPU *gpu,SDL_Texture *texture,const void *pixels,Uint32 width,Uint32 height,Uint32 pitch) {
    SDL_GPUTransferBufferCreateInfo info={0};SDL_GPUTransferBuffer *transfer;
    SDL_GPUCommandBuffer *command;SDL_GPUCopyPass *copy;
    SDL_GPUTextureTransferInfo source={0};SDL_GPUTextureRegion destination={0};void *memory;
    info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;info.size=pitch*height;
    transfer=SDL_CreateGPUTransferBuffer(gpu->device,&info);if(!transfer)return 0;
    memory=SDL_MapGPUTransferBuffer(gpu->device,transfer,false);
    if(!memory){SDL_ReleaseGPUTransferBuffer(gpu->device,transfer);return 0;}
    memcpy(memory,pixels,info.size);SDL_UnmapGPUTransferBuffer(gpu->device,transfer);
    command=SDL_AcquireGPUCommandBuffer(gpu->device);
    if(!command){SDL_ReleaseGPUTransferBuffer(gpu->device,transfer);return 0;}
    copy=SDL_BeginGPUCopyPass(command);source.transfer_buffer=transfer;source.pixels_per_row=pitch/4;source.rows_per_layer=height;
    destination.texture=SDL_GetPointerProperty(SDL_GetTextureProperties(texture),SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,NULL);
    destination.w=width;destination.h=height;destination.d=1;
    SDL_UploadToGPUTexture(copy,&source,&destination,false);SDL_EndGPUCopyPass(copy);
    {
        int ok=SDL_SubmitGPUCommandBuffer(command);
        SDL_ReleaseGPUTransferBuffer(gpu->device,transfer);return ok;
    }
}

static int game_gpu_init(GameGPU *gpu, SDL_Renderer *renderer) {
    SDL_GPUShaderCreateInfo shader={0};
    SDL_GPUShader *vs=NULL,*fs=NULL,*blend_fs=NULL;
    SDL_GPUGraphicsPipelineCreateInfo pipeline={0};
    SDL_GPUColorTargetDescription color={0};
    SDL_GPUVertexBufferDescription buffer={0};
    SDL_GPUVertexAttribute attributes[3]={{0}};
    SDL_GPUTextureCreateInfo depth={0};
    SDL_GPUSamplerCreateInfo sampler={0};
    Uint32 white=0xffffffff;
    int white_uploaded=0;
    shader.code=(const Uint8*)zsharp_game_vertex_spirv;shader.code_size=sizeof(zsharp_game_vertex_spirv);
    shader.entrypoint="main";shader.format=SDL_GPU_SHADERFORMAT_SPIRV;shader.stage=SDL_GPU_SHADERSTAGE_VERTEX;
    vs=SDL_CreateGPUShader(gpu->device,&shader);
    shader.code=(const Uint8*)zsharp_game_fragment_spirv;shader.code_size=sizeof(zsharp_game_fragment_spirv);
    shader.stage=SDL_GPU_SHADERSTAGE_FRAGMENT;shader.num_samplers=1;
    fs=SDL_CreateGPUShader(gpu->device,&shader);
    if(!vs||!fs) goto done;
    buffer.pitch=sizeof(GameGPUVertex);buffer.input_rate=SDL_GPU_VERTEXINPUTRATE_VERTEX;
    attributes[0].location=0;attributes[0].format=SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].location=1;attributes[1].format=SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;attributes[1].offset=16;
    attributes[2].location=2;attributes[2].format=SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;attributes[2].offset=32;
    pipeline.vertex_shader=vs;pipeline.fragment_shader=fs;
    pipeline.vertex_input_state.vertex_buffer_descriptions=&buffer;pipeline.vertex_input_state.num_vertex_buffers=1;
    pipeline.vertex_input_state.vertex_attributes=attributes;pipeline.vertex_input_state.num_vertex_attributes=3;
    pipeline.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline.rasterizer_state.fill_mode=SDL_GPU_FILLMODE_FILL;
    pipeline.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
    pipeline.rasterizer_state.enable_depth_clip=true;
    pipeline.multisample_state.sample_count=SDL_GPU_SAMPLECOUNT_1;
    pipeline.depth_stencil_state.enable_depth_test=true;
    pipeline.depth_stencil_state.enable_depth_write=true;
    pipeline.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_LESS;
    color.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    pipeline.target_info.color_target_descriptions=&color;pipeline.target_info.num_color_targets=1;
    pipeline.target_info.has_depth_stencil_target=true;
    pipeline.target_info.depth_stencil_format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    gpu->opaque=SDL_CreateGPUGraphicsPipeline(gpu->device,&pipeline);
    shader.code=(const Uint8*)zsharp_game_blend_spirv;shader.code_size=sizeof(zsharp_game_blend_spirv);
    blend_fs=SDL_CreateGPUShader(gpu->device,&shader);
    if(!blend_fs)goto done;
    pipeline.fragment_shader=blend_fs;
    pipeline.depth_stencil_state.enable_depth_write=false;
    color.blend_state.enable_blend=true;
    color.blend_state.src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op=SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op=SDL_GPU_BLENDOP_ADD;
    gpu->blend=SDL_CreateGPUGraphicsPipeline(gpu->device,&pipeline);
    depth.type=SDL_GPU_TEXTURETYPE_2D;depth.format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    depth.usage=SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth.width=1280;depth.height=720;depth.layer_count_or_depth=1;depth.num_levels=1;
    gpu->depth=SDL_CreateGPUTexture(gpu->device,&depth);
    sampler.address_mode_u=sampler.address_mode_v=sampler.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    gpu->sampler=SDL_CreateGPUSampler(gpu->device,&sampler);
    gpu->target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,1280,720);
    gpu->white=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,1,1);
    if(gpu->white && !game_gpu_upload(gpu,gpu->white,&white,1,1,4)) goto done;
    white_uploaded=gpu->white!=NULL;
done:
    if(vs)SDL_ReleaseGPUShader(gpu->device,vs);
    if(fs)SDL_ReleaseGPUShader(gpu->device,fs);
    if(blend_fs)SDL_ReleaseGPUShader(gpu->device,blend_fs);
    return gpu->opaque&&gpu->blend&&gpu->depth&&gpu->sampler&&gpu->target&&white_uploaded;
}

static int game_gpu_face(GameGPU *gpu,const ZSharpProjectedCubeFace *face,SDL_FColor color,SDL_Texture *texture,int transparent) {
    size_t count=(face->point_count-2)*3, corner, out;
    SDL_GPUTexture *image=SDL_GetPointerProperty(SDL_GetTextureProperties(texture?texture:gpu->white),SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,NULL);
    GameGPUDraw *draw;
    if(!image||gpu->failed) return gpu->failed=1,0;
    if(gpu->vertex_count+count>gpu->vertex_capacity) {
        size_t capacity=(gpu->vertex_count+count)*2+1024;
        void *memory=realloc(gpu->vertices,capacity*sizeof(*gpu->vertices));
        if(!memory)return gpu->failed=1,0;
        gpu->vertices=memory;gpu->vertex_capacity=capacity;
    }
    if(gpu->draw_count==gpu->draw_capacity) {
        size_t capacity=gpu->draw_capacity*2+128;
        void *memory=realloc(gpu->draws,capacity*sizeof(*gpu->draws));
        if(!memory)return gpu->failed=1,0;
        gpu->draws=memory;gpu->draw_capacity=capacity;
    }
    if(gpu->draw_count && gpu->draws[gpu->draw_count-1].texture==image &&
        gpu->draws[gpu->draw_count-1].transparent==transparent) {
        draw=&gpu->draws[gpu->draw_count-1];draw->count+=(Uint32)count;
    } else {
        draw=&gpu->draws[gpu->draw_count++];draw->texture=image;
        draw->first=(Uint32)gpu->vertex_count;draw->count=(Uint32)count;draw->transparent=transparent;
    }
    for(corner=1;corner+1<face->point_count;corner++) {
        size_t indices[3]={0,corner,corner+1};
        for(out=0;out<3;out++) {
            size_t i=indices[out];float depth=face->point_depth[i];
            GameGPUVertex *vertex=&gpu->vertices[gpu->vertex_count++];
            vertex->position[0]=(face->points[i][0]/640.0f-1)*depth;
            vertex->position[1]=(1-face->points[i][1]/360.0f)*depth;
            vertex->position[2]=depth-.05f;vertex->position[3]=depth;
            vertex->color[0]=color.r;vertex->color[1]=color.g;vertex->color[2]=color.b;vertex->color[3]=color.a;
            vertex->uv[0]=face->texcoords[i][0];vertex->uv[1]=face->texcoords[i][1];
        }
    }
    return 1;
}

static int game_gpu_present(GameGPU *gpu,SDL_Renderer *renderer,unsigned background) {
    SDL_GPUCommandBuffer *command;
    SDL_GPUCopyPass *copy;
    SDL_GPURenderPass *pass;
    SDL_GPUColorTargetInfo color={0};SDL_GPUDepthStencilTargetInfo depth={0};
    SDL_GPUBufferBinding binding={0};
    SDL_GPUBufferRegion destination={0};SDL_GPUTransferBufferLocation source={0};
    size_t bytes=gpu->vertex_count*sizeof(*gpu->vertices),i;int phase;
    if(gpu->failed||bytes>UINT32_MAX) return 0;
    if(!SDL_FlushRenderer(renderer))return 0;
    if(bytes>gpu->buffer_size) {
        SDL_GPUBufferCreateInfo info={0};SDL_GPUTransferBufferCreateInfo transfer={0};
        if(gpu->buffer)SDL_ReleaseGPUBuffer(gpu->device,gpu->buffer);
        if(gpu->transfer)SDL_ReleaseGPUTransferBuffer(gpu->device,gpu->transfer);
        info.usage=SDL_GPU_BUFFERUSAGE_VERTEX;info.size=(Uint32)bytes;
        transfer.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;transfer.size=(Uint32)bytes;
        gpu->buffer=SDL_CreateGPUBuffer(gpu->device,&info);
        gpu->transfer=SDL_CreateGPUTransferBuffer(gpu->device,&transfer);gpu->buffer_size=(Uint32)bytes;
        if(!gpu->buffer||!gpu->transfer)return 0;
    }
    command=SDL_AcquireGPUCommandBuffer(gpu->device);if(!command)return 0;
    if(bytes) {
        void *memory=SDL_MapGPUTransferBuffer(gpu->device,gpu->transfer,true);
        if(!memory){SDL_CancelGPUCommandBuffer(command);return 0;}
        memcpy(memory,gpu->vertices,bytes);SDL_UnmapGPUTransferBuffer(gpu->device,gpu->transfer);
        copy=SDL_BeginGPUCopyPass(command);
        source.transfer_buffer=gpu->transfer;destination.buffer=gpu->buffer;destination.size=(Uint32)bytes;
        SDL_UploadToGPUBuffer(copy,&source,&destination,true);SDL_EndGPUCopyPass(copy);
    }
    color.texture=SDL_GetPointerProperty(SDL_GetTextureProperties(gpu->target),SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,NULL);
    color.clear_color=(SDL_FColor){((background>>16)&255)/255.f,((background>>8)&255)/255.f,(background&255)/255.f,1};
    color.load_op=SDL_GPU_LOADOP_CLEAR;color.store_op=SDL_GPU_STOREOP_STORE;
    depth.texture=gpu->depth;depth.clear_depth=1;depth.load_op=SDL_GPU_LOADOP_CLEAR;depth.store_op=SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;depth.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
    pass=SDL_BeginGPURenderPass(command,&color,1,&depth);
    {
        SDL_GPUViewport viewport={0,0,1280,720,0,1};
        SDL_SetGPUViewport(pass,&viewport);
    }
    binding.buffer=gpu->buffer;
    if(bytes)SDL_BindGPUVertexBuffers(pass,0,&binding,1);
    /* Opaque first, then sorted translucent faces without depth writes. */
    for(phase=0;phase<2;phase++) {
        SDL_BindGPUGraphicsPipeline(pass,phase?gpu->blend:gpu->opaque);
        for(i=0;i<gpu->draw_count;i++) {
            GameGPUDraw *draw=&gpu->draws[i];SDL_GPUTextureSamplerBinding image={draw->texture,gpu->sampler};
            if(phase && !draw->transparent)continue;
            SDL_BindGPUFragmentSamplers(pass,0,&image,1);
            SDL_DrawGPUPrimitives(pass,draw->count,1,draw->first,0);
        }
    }
    SDL_EndGPURenderPass(pass);
    if(!SDL_SubmitGPUCommandBuffer(command))return 0;
    return SDL_RenderTexture(renderer,gpu->target,NULL,NULL);
}

static int game_gpu_regression(GameGPU *gpu,SDL_Renderer *renderer) {
    ZSharpProjectedCubeFace face={0};
    SDL_FColor red={1,0,0,1},green={0,1,0,1},blue={0,0,1,.5f};
    SDL_Surface *image;Uint8 r,g,b,a;int ok;
    SDL_Texture *textured=NULL;
    Uint8 texel[4]={255,255,0,128};
    face.point_count=3;
    face.points[0][0]=100;face.points[0][1]=100;
    face.points[1][0]=400;face.points[1][1]=100;
    face.points[2][0]=100;face.points[2][1]=400;
    face.point_depth[0]=face.point_depth[1]=face.point_depth[2]=2;
    game_gpu_face(gpu,&face,red,NULL,0);
    face.point_depth[0]=face.point_depth[1]=face.point_depth[2]=4;
    game_gpu_face(gpu,&face,green,NULL,0); /* Must not cover the near red face. */
    face.point_depth[0]=face.point_depth[1]=face.point_depth[2]=1;
    game_gpu_face(gpu,&face,blue,NULL,1);
    textured=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,1,1);
    if(!textured || !game_gpu_upload(gpu,textured,texel,1,1,4)) {
        if(textured)SDL_DestroyTexture(textured);return 0;
    }
    face.points[0][0]+=500;face.points[1][0]+=500;face.points[2][0]+=500;
    game_gpu_face(gpu,&face,(SDL_FColor){1,1,1,1},textured,1);
    if(!game_gpu_present(gpu,renderer,0))return 0;
    SDL_SetRenderTarget(renderer,gpu->target);
    image=SDL_RenderReadPixels(renderer,NULL);
    SDL_SetRenderTarget(renderer,NULL);
    if(!image)return 0;
    ok=SDL_ReadSurfacePixel(image,150,150,&r,&g,&b,&a) && r>=126&&r<=129&&g==0&&b>=126&&b<=129;
    ok=ok && SDL_ReadSurfacePixel(image,650,150,&r,&g,&b,&a) && r>=126&&r<=129&&g>=126&&g<=129&&b==0;
    SDL_DestroySurface(image);
    SDL_DestroyTexture(textured);
    gpu->vertex_count=gpu->draw_count=0;
    return ok;
}
