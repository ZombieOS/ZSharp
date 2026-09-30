#define _CRT_SECURE_NO_WARNINGS

#include "game_animation.h"
#include "game_model.h"
#include "zsharp.h"

#include <quickjs.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void animation_error(char *error, size_t size, const char *message) {
    if (error != NULL && size > 0) snprintf(error, size, "%s", message);
}

static char *copy_js_text(JSContext *ctx, JSValueConst value) {
    const char *source = JS_ToCString(ctx, value);
    char *copy;
    if (source == NULL) return NULL;
    copy = zsharp_copy_text(source, strlen(source));
    JS_FreeCString(ctx, source);
    return copy;
}

static int number_property(JSContext *ctx, JSValueConst object,
                           const char *name, double *result, int required) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    int ok = !JS_IsException(value) &&
             (!JS_IsUndefined(value) || !required);
    if (ok && !JS_IsUndefined(value)) {
        ok = JS_IsNumber(value) && JS_ToFloat64(ctx, result, value) == 0 &&
             isfinite(*result);
    }
    JS_FreeValue(ctx, value);
    return ok;
}

static int status_property(JSContext *ctx, JSValueConst object,
                           const char *name, int *result, int fallback) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    int ok = !JS_IsException(value);
    if (ok && JS_IsUndefined(value)) *result = fallback;
    else if (ok && JS_IsBool(value)) *result = JS_ToBool(ctx, value);
    else ok = 0;
    JS_FreeValue(ctx, value);
    return ok;
}

static int array_length(JSContext *ctx, JSValueConst array, uint32_t *length) {
    JSValue value;
    int ok;
    if (!JS_IsArray(array)) return 0;
    value = JS_GetPropertyStr(ctx, array, "length");
    ok = !JS_IsException(value) && JS_ToUint32(ctx, length, value) == 0;
    JS_FreeValue(ctx, value);
    return ok;
}

static int parse_key(JSContext *ctx, JSValueConst value,
                     ZSharpAnimationKey *key, size_t target_count) {
    static const char *position[] = {"posX", "posY", "posZ"};
    static const char *rotation[] = {"rotX", "rotY", "rotZ"};
    JSValue mask;
    double number = 0.0;
    int axis;
    if (!JS_IsObject(value) ||
        !number_property(ctx, value, "frame", &key->frame, 1) ||
        key->frame < 0.0) return 0;
    key->target_mask = (unsigned char *)malloc(target_count);
    if (key->target_mask == NULL) return 0;
    memset(key->target_mask, 1, target_count);
    for (axis = 0; axis < 3; axis++) {
        number = 0.0;
        if (!number_property(ctx, value, position[axis], &number, 0)) return 0;
        key->position[axis] = (float)number;
        number = 0.0;
        if (!number_property(ctx, value, rotation[axis], &number, 0)) return 0;
        key->rotation[axis] = (float)number;
        if (!isfinite(key->position[axis]) || !isfinite(key->rotation[axis]))
            return 0;
    }
    mask = JS_GetPropertyStr(ctx, value, "models");
    if (JS_IsException(mask)) return 0;
    if (!JS_IsUndefined(mask)) {
        JSPropertyEnum *properties = NULL;
        uint32_t count = 0, index;
        if (!JS_IsObject(mask) || JS_IsArray(mask) ||
            JS_GetOwnPropertyNames(ctx, &properties, &count, mask,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreeValue(ctx, mask);
            return 0;
        }
        memset(key->target_mask, 0, target_count);
        for (index = 0; index < count; index++) {
            const char *name = JS_AtomToCString(ctx, properties[index].atom);
            char *end = NULL;
            unsigned long target = name == NULL ? 0 : strtoul(name, &end, 10);
            JSValue selected = JS_GetProperty(ctx, mask, properties[index].atom);
            int valid = name != NULL && end != name && *end == '\0' &&
                        target >= 1 && target <= target_count && JS_IsBool(selected);
            if (valid) key->target_mask[target - 1] = (unsigned char)JS_ToBool(ctx, selected);
            JS_FreeValue(ctx, selected);
            if (name != NULL) JS_FreeCString(ctx, name);
            JS_FreeAtom(ctx, properties[index].atom);
            if (!valid) {
                uint32_t rest;
                for (rest = index + 1; rest < count; rest++)
                    JS_FreeAtom(ctx, properties[rest].atom);
                js_free(ctx, properties);
                JS_FreeValue(ctx, mask);
                return 0;
            }
        }
        js_free(ctx, properties);
    }
    JS_FreeValue(ctx, mask);
    return 1;
}

static int parse_clip(JSContext *ctx, JSValueConst value,
                      ZSharpAnimationClip *clip) {
    JSValue targets = JS_UNDEFINED, type = JS_UNDEFINED, parts = JS_UNDEFINED;
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0, index;
    int ok = 0;
    if (!JS_IsObject(value)) goto done;
    targets = JS_GetPropertyStr(ctx, value, "models");
    if (!array_length(ctx, targets, &count) || count == 0 || count > 256) goto done;
    clip->targets = (char **)calloc(count, sizeof(char *));
    if (clip->targets == NULL) goto done;
    clip->target_count = count;
    for (index = 0; index < count; index++) {
        JSValue target = JS_GetPropertyUint32(ctx, targets, index);
        if (!JS_IsString(target)) { JS_FreeValue(ctx, target); goto done; }
        clip->targets[index] = copy_js_text(ctx, target);
        JS_FreeValue(ctx, target);
        if (clip->targets[index] == NULL || clip->targets[index][0] == '\0') goto done;
    }
    if (!number_property(ctx, value, "fps", &clip->fps, 1) ||
        clip->fps <= 0.0 || clip->fps > 1000000.0 ||
        !status_property(ctx, value, "loop", &clip->loop, 0)) goto done;
    type = JS_GetPropertyStr(ctx, value, "type");
    if (!JS_IsString(type)) goto done;
    {
        char *kind = copy_js_text(ctx, type);
        if (kind == NULL) goto done;
        clip->tween = strcmp(kind, "tween") == 0;
        ok = clip->tween || strcmp(kind, "static") == 0;
        free(kind);
        if (!ok) goto done;
    }
    parts = JS_GetPropertyStr(ctx, value, "parts");
    if (!JS_IsObject(parts) || JS_IsArray(parts) ||
        JS_GetOwnPropertyNames(ctx, &properties, &count, parts,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0 ||
        count == 0 || count > 256) goto done;
    clip->parts = (ZSharpAnimationPart *)calloc(count, sizeof(*clip->parts));
    if (clip->parts == NULL) goto done;
    clip->part_count = count;
    for (index = 0; index < count; index++) {
        ZSharpAnimationPart *part = &clip->parts[index];
        JSValue frames = JS_GetProperty(ctx, parts, properties[index].atom);
        uint32_t frame_count = 0, frame_index;
        const char *name = JS_AtomToCString(ctx, properties[index].atom);
        part->name = name == NULL ? NULL : zsharp_copy_text(name, strlen(name));
        if (name != NULL) JS_FreeCString(ctx, name);
        if (part->name == NULL || !array_length(ctx, frames, &frame_count) ||
            frame_count == 0 || frame_count > 100000) {
            JS_FreeValue(ctx, frames);
            goto done;
        }
        part->keys = (ZSharpAnimationKey *)calloc(frame_count, sizeof(*part->keys));
        if (part->keys == NULL) { JS_FreeValue(ctx, frames); goto done; }
        part->key_count = frame_count;
        for (frame_index = 0; frame_index < frame_count; frame_index++) {
            JSValue key = JS_GetPropertyUint32(ctx, frames, frame_index);
            int parsed = parse_key(ctx, key, &part->keys[frame_index],
                                   clip->target_count);
            JS_FreeValue(ctx, key);
            if (!parsed || (frame_index > 0 &&
                part->keys[frame_index].frame <=
                part->keys[frame_index - 1].frame)) {
                JS_FreeValue(ctx, frames);
                goto done;
            }
            if (part->keys[frame_index].frame > clip->last_frame)
                clip->last_frame = part->keys[frame_index].frame;
        }
        JS_FreeValue(ctx, frames);
    }
    ok = 1;
done:
    if (properties != NULL) {
        for (index = 0; index < count; index++)
            JS_FreeAtom(ctx, properties[index].atom);
        js_free(ctx, properties);
    }
    JS_FreeValue(ctx, targets);
    JS_FreeValue(ctx, type);
    JS_FreeValue(ctx, parts);
    return ok;
}

static void free_clip(ZSharpAnimationClip *clip) {
    size_t index, key;
    free(clip->name);
    for (index = 0; index < clip->target_count; index++) free(clip->targets[index]);
    free(clip->targets);
    for (index = 0; index < clip->part_count; index++) {
        free(clip->parts[index].name);
        for (key = 0; key < clip->parts[index].key_count; key++)
            free(clip->parts[index].keys[key].target_mask);
        free(clip->parts[index].keys);
    }
    free(clip->parts);
    free(clip->selection);
    free(clip->base_transforms);
}

void zsharp_game_animation_free(ZSharpGameModel *model) {
    size_t file, clip;
    for (file = 0; file < model->animation_count; file++) {
        for (clip = 0; clip < model->animations[file].clip_count; clip++)
            free_clip(&model->animations[file].clips[clip]);
        free(model->animations[file].clips);
        free(model->animations[file].name);
    }
    free(model->animations);
    model->animations = NULL;
    model->animation_count = 0;
}

int zsharp_game_animation_load(ZSharpGameModel *model,
                               const char *path, char *error,
                               size_t error_size) {
    FILE *input = NULL;
    char *data = NULL;
    long length;
    JSRuntime *runtime = NULL;
    JSContext *ctx = NULL;
    JSValue root = JS_UNDEFINED, clips = JS_UNDEFINED;
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0, index;
    ZSharpAnimationFile *file;
    const char *stem = path, *cursor;
    char *name = NULL;
    int ok = 0;
    for (cursor = path; *cursor; cursor++)
        if (*cursor == '/' || *cursor == '\\') stem = cursor + 1;
    cursor = strrchr(stem, '.');
    if (cursor == NULL) goto done;
    name = zsharp_copy_text(stem, (size_t)(cursor - stem));
    if (name == NULL) goto done;
    input = fopen(path, "rb");
    if (input == NULL || fseek(input, 0, SEEK_END) != 0 ||
        (length = ftell(input)) < 0 || length > 16 * 1024 * 1024 ||
        fseek(input, 0, SEEK_SET) != 0) goto done;
    data = (char *)malloc((size_t)length + 1);
    if (data == NULL || fread(data, 1, (size_t)length, input) != (size_t)length)
        goto done;
    data[length] = '\0';
    runtime = JS_NewRuntime();
    if (runtime == NULL) goto done;
    JS_SetMemoryLimit(runtime, 64 * 1024 * 1024);
    ctx = JS_NewContext(runtime);
    if (ctx == NULL) goto done;
    root = JS_ParseJSON(ctx, data, (size_t)length, path);
    if (JS_IsException(root)) goto done;
    clips = JS_GetPropertyStr(ctx, root, "clips");
    if (!JS_IsObject(clips) || JS_IsArray(clips) ||
        JS_GetOwnPropertyNames(ctx, &properties, &count, clips,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0 ||
        count == 0 || count > 256) goto done;
    {
        ZSharpAnimationFile *resized = (ZSharpAnimationFile *)realloc(
            model->animations, (model->animation_count + 1) * sizeof(*resized));
        if (resized == NULL) goto done;
        model->animations = resized;
        file = &resized[model->animation_count++];
        memset(file, 0, sizeof(*file));
        file->name = name;
        name = NULL;
        file->clips = (ZSharpAnimationClip *)calloc(count, sizeof(*file->clips));
        if (file->clips == NULL) goto done;
        file->clip_count = count;
    }
    for (index = 0; index < count; index++) {
        JSValue value = JS_GetProperty(ctx, clips, properties[index].atom);
        const char *clip_name = JS_AtomToCString(ctx, properties[index].atom);
        ZSharpAnimationClip *clip = &file->clips[index];
        clip->name = clip_name == NULL ? NULL :
            zsharp_copy_text(clip_name, strlen(clip_name));
        if (clip_name != NULL) JS_FreeCString(ctx, clip_name);
        ok = clip->name != NULL && parse_clip(ctx, value, clip);
        JS_FreeValue(ctx, value);
        if (!ok) goto done;
    }
    ok = 1;
done:
    if (properties != NULL && ctx != NULL) {
        for (index = 0; index < count; index++)
            JS_FreeAtom(ctx, properties[index].atom);
        js_free(ctx, properties);
    }
    if (ctx != NULL) {
        JS_FreeValue(ctx, clips);
        JS_FreeValue(ctx, root);
        JS_FreeContext(ctx);
    }
    if (runtime != NULL) JS_FreeRuntime(runtime);
    if (input != NULL) fclose(input);
    free(data);
    free(name);
    if (!ok) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size,
                     "%s: invalid .zanimation JSON or clip schema", path);
    }
    return ok;
}

static ZSharpAnimationClip *find_clip(const ZSharpGameModel *model,
                                      const char *file_name,
                                      const char *clip_name) {
    size_t file, clip;
    for (file = 0; file < model->animation_count; file++) {
        if (strcmp(model->animations[file].name, file_name) != 0) continue;
        for (clip = 0; clip < model->animations[file].clip_count; clip++)
            if (strcmp(model->animations[file].clips[clip].name, clip_name) == 0)
                return &model->animations[file].clips[clip];
    }
    return NULL;
}

static int split_command(const char *path, char *buffer, size_t size,
                         char **file, char **action, char **clip) {
    char *first, *second;
    if (path == NULL || strlen(path) + 1 > size) return 0;
    strcpy(buffer, path);
    first = strchr(buffer, '.');
    if (first == NULL) return 0;
    *first++ = '\0';
    second = strchr(first, '.');
    if (second == NULL || strchr(second + 1, '.') != NULL) return 0;
    *second++ = '\0';
    if (*buffer == '\0' || *second == '\0' ||
        (strcmp(first, "playClip") != 0 &&
         strcmp(first, "pauseClip") != 0 &&
         strcmp(first, "stopClip") != 0)) return 0;
    *file = buffer; *action = first; *clip = second;
    return 1;
}

int zsharp_game_animation_has_command(const ZSharpGameModel *model,
                                      const char *path) {
    char buffer[512], *file, *action, *clip;
    return split_command(path, buffer, sizeof(buffer), &file, &action, &clip) &&
           find_clip(model, file, clip) != NULL;
}

static int source_matches(const char *reference, const ZSharpGameObject *object) {
    const char *stem = reference, *cursor, *dot;
    size_t length;
    for (cursor = reference; *cursor; cursor++)
        if (*cursor == '/' || *cursor == '\\') stem = cursor + 1;
    dot = strrchr(stem, '.');
    length = dot == NULL ? strlen(stem) : (size_t)(dot - stem);
    return object->source_file != NULL &&
           strlen(object->source_file) == length &&
           strncmp(stem, object->source_file, length) == 0;
}

static int selected_instance(const ZSharpAnimationClip *clip,
                             unsigned instance) {
    size_t index;
    if (clip->selection_count == 0) return 1;
    for (index = 0; index < clip->selection_count; index++)
        if (clip->selection[index] == instance) return 1;
    return 0;
}

static int matches_placed_instance(const ZSharpGameModel *model,
                                   const ZSharpAnimationClip *clip,
                                   size_t object_index) {
    const ZSharpGameObject *object = &model->objects[object_index];
    size_t target;
    if (object->scene == NULL || model->active_scene == NULL ||
        strcmp(object->scene, model->active_scene) != 0) return 0;
    for (target = 0; target < clip->target_count; target++) {
        size_t previous;
        unsigned instance = 0;
        if (!source_matches(clip->targets[target], object)) continue;
        for (previous = 0; previous <= object_index; previous++)
            if (source_matches(clip->targets[target], &model->objects[previous]) &&
                model->objects[previous].scene != NULL &&
                strcmp(model->objects[previous].scene, object->scene) == 0)
                instance++;
        return selected_instance(clip, instance);
    }
    return 0;
}

static void restore_object(ZSharpGameModel *model, ZSharpAnimationClip *clip,
                           size_t index) {
    float *base;
    if (clip->base_transforms == NULL) return;
    base = &clip->base_transforms[index * 6];
    if (!isfinite(base[0])) return;
    model->objects[index].x = base[0];
    model->objects[index].y = base[1];
    model->objects[index].z = base[2];
    model->objects[index].rotation_x = base[3];
    model->objects[index].rotation_y = base[4];
    model->objects[index].rotation_z = base[5];
    free(model->objects[index].part_poses);
    model->objects[index].part_poses = NULL;
    model->objects[index].part_pose_count = 0;
    base[0] = NAN;
}

static void restore_base(ZSharpGameModel *model, ZSharpAnimationClip *clip) {
    size_t index;
    if (clip->base_transforms == NULL) return;
    for (index = 0; index < model->object_count; index++)
        restore_object(model, clip, index);
    free(clip->base_transforms);
    clip->base_transforms = NULL;
}

int zsharp_game_animation_command(ZSharpGameModel *model,
                                  const char *path, const char *selection,
                                  char *error, size_t error_size) {
    char buffer[512], *file, *action, *name;
    ZSharpAnimationClip *clip;
    if (!split_command(path, buffer, sizeof(buffer), &file, &action, &name) ||
        (clip = find_clip(model, file, name)) == NULL) {
        animation_error(error, error_size, "unknown animation clip command");
        return 0;
    }
    if (strcmp(action, "pauseClip") == 0) {
        if (clip->state == 1) clip->state = 2;
        return 1;
    }
    if (strcmp(action, "stopClip") == 0) {
        restore_base(model, clip);
        clip->elapsed = 0.0;
        clip->state = 0;
        return 1;
    }
    if (selection != NULL && *selection != '\0') {
        char *copy = zsharp_copy_text(selection, strlen(selection));
        char *part, *context = NULL;
        unsigned *indices = NULL;
        size_t count = 0;
        if (copy == NULL) goto memory_error;
#ifdef _WIN32
        part = strtok_s(copy, ",", &context);
#else
        part = strtok_r(copy, ",", &context);
#endif
        while (part != NULL) {
            char *end = NULL;
            unsigned long value = strtoul(part, &end, 10);
            unsigned *resized;
            if (end == part || *end != '\0' || value == 0 || value > 1000000) {
                free(indices); free(copy);
                animation_error(error, error_size, "animation instances must be positive integers");
                return 0;
            }
            resized = (unsigned *)realloc(indices, (count + 1) * sizeof(*resized));
            if (resized == NULL) { free(indices); free(copy); goto memory_error; }
            indices = resized;
            indices[count++] = (unsigned)value;
#ifdef _WIN32
            part = strtok_s(NULL, ",", &context);
#else
            part = strtok_r(NULL, ",", &context);
#endif
        }
        free(copy);
        free(clip->selection);
        clip->selection = indices;
        clip->selection_count = count;
    } else {
        free(clip->selection);
        clip->selection = NULL;
        clip->selection_count = 0;
    }
    /* One placed instance has one pose at a time. Stop only the instances
       taken over by this clip, leaving other instances animating normally. */
    {
        size_t index, file_index, clip_index;
        for (index = 0; index < model->object_count; index++) {
            if (!matches_placed_instance(model, clip, index)) continue;
            for (file_index = 0; file_index < model->animation_count;
                 file_index++) {
                for (clip_index = 0;
                     clip_index < model->animations[file_index].clip_count;
                     clip_index++) {
                    ZSharpAnimationClip *other =
                        &model->animations[file_index].clips[clip_index];
                    if (other != clip) restore_object(model, other, index);
                }
            }
        }
    }
    if (clip->state == 2 && clip->base_transforms != NULL) {
        size_t index;
        int can_resume = 1;
        for (index = 0; index < model->object_count; index++)
            if (matches_placed_instance(model, clip, index) &&
                !isfinite(clip->base_transforms[index * 6]))
                can_resume = 0;
        if (can_resume) { clip->state = 1; return 1; }
    }
    restore_base(model, clip);
    clip->base_transforms = (float *)malloc(model->object_count * 6 * sizeof(float));
    if (clip->base_transforms == NULL) goto memory_error;
    {
        size_t index;
        for (index = 0; index < model->object_count; index++)
            clip->base_transforms[index * 6] = NAN;
    }
    {
        size_t index;
        for (index = 0; index < model->object_count; index++) {
            ZSharpGameObject *object = &model->objects[index];
            float *base = &clip->base_transforms[index * 6];
            int matches = matches_placed_instance(model, clip, index);
            base[0] = matches ? object->x : NAN;
            base[1] = object->y; base[2] = object->z;
            base[3] = object->rotation_x;
            base[4] = object->rotation_y;
            base[5] = object->rotation_z;
            if (matches && object->shape == ZGAME_SHAPE_MESH &&
                clip->part_count > 0) {
                size_t part;
                object->part_poses = (ZSharpGamePartPose *)calloc(
                    clip->part_count, sizeof(*object->part_poses));
                if (object->part_poses == NULL) goto memory_error;
                object->part_pose_count = clip->part_count;
                for (part = 0; part < clip->part_count; part++)
                    object->part_poses[part].name = clip->parts[part].name;
            }
        }
    }
    clip->elapsed = 0.0;
    clip->state = 1;
    return 1;
memory_error:
    restore_base(model, clip);
    animation_error(error, error_size, "out of memory preparing animation");
    return 0;
}

static void sample_part(const ZSharpAnimationPart *part, double frame,
                        int tween, size_t target, float pose[6]) {
    const ZSharpAnimationKey *first = &part->keys[0], *last = first;
    size_t index, axis;
    float amount = 0.0f;
    for (index = 1; index < part->key_count; index++) {
        last = &part->keys[index];
        if (last->frame >= frame) break;
        first = last;
    }
    if (tween && last != first && last->frame > first->frame)
        amount = (float)((frame - first->frame) / (last->frame - first->frame));
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    if (target < 256 && !first->target_mask[target]) return;
    for (axis = 0; axis < 3; axis++) {
        pose[axis] = first->position[axis] +
            (last->position[axis] - first->position[axis]) * amount;
        pose[axis + 3] = first->rotation[axis] +
            (last->rotation[axis] - first->rotation[axis]) * amount;
    }
}

void zsharp_game_animation_update(ZSharpGameModel *model,
                                  double delta_seconds) {
    size_t file, clip_index, object_index;
    for (file = 0; file < model->animation_count; file++) {
        for (clip_index = 0; clip_index < model->animations[file].clip_count;
             clip_index++) {
            ZSharpAnimationClip *clip = &model->animations[file].clips[clip_index];
            double frame;
            if (clip->state != 1 || clip->base_transforms == NULL) continue;
            clip->elapsed += delta_seconds;
            frame = clip->elapsed * clip->fps;
            if (clip->last_frame > 0.0 && frame > clip->last_frame) {
                if (clip->loop) {
                    clip->elapsed = fmod(clip->elapsed,
                                         clip->last_frame / clip->fps);
                    frame = clip->elapsed * clip->fps;
                } else {
                    frame = clip->last_frame;
                    clip->state = 2;
                }
            }
            for (object_index = 0; object_index < model->object_count;
                 object_index++) {
                ZSharpGameObject *object = &model->objects[object_index];
                float *base = &clip->base_transforms[object_index * 6];
                size_t target, part;
                float pose[6] = {0};
                if (!isfinite(base[0]) || object->scene == NULL ||
                    model->active_scene == NULL ||
                    strcmp(object->scene, model->active_scene) != 0) continue;
                for (target = 0; target < clip->target_count; target++)
                    if (source_matches(clip->targets[target], object)) break;
                if (target == clip->target_count) continue;
                for (part = 0; part < clip->part_count; part++) {
                    const ZSharpAnimationPart *track = &clip->parts[part];
                    int root = strcmp(track->name, "root") == 0 ||
                               strcmp(track->name, "part_1") == 0;
                    memset(pose, 0, sizeof(pose));
                    sample_part(track, frame, clip->tween, target, pose);
                    if (root) {
                        object->x = base[0] + pose[0];
                        object->y = base[1] + pose[1];
                        object->z = base[2] + pose[2];
                        object->rotation_x = base[3] + pose[3];
                        object->rotation_y = base[4] + pose[4];
                        object->rotation_z = base[5] + pose[5];
                    } else if (object->shape == ZGAME_SHAPE_MESH &&
                               part < object->part_pose_count) {
                        ZSharpGamePartPose *target_pose =
                            &object->part_poses[part];
                        memcpy(target_pose->position, pose, sizeof(pose[0]) * 3);
                        memcpy(target_pose->rotation, pose + 3,
                               sizeof(pose[0]) * 3);
                    }
                }
            }
        }
    }
}
