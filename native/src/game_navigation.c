#define _CRT_SECURE_NO_WARNINGS

#include "game_navigation.h"
#include "game_model.h"
#include "zsharp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZSHARP_MAX_NAV_POINTS 128

static int split_command(const char *path, char *storage, size_t size,
                         char **name, char **action, char **target) {
    char *first, *second, *third;
    if (path == NULL || strlen(path) + 1 > size) return 0;
    strcpy(storage, path);
    first = strchr(storage, '.');
    if (first == NULL) return 0;
    *first++ = '\0';
    second = strchr(first, '.');
    if (second == NULL) return 0;
    *second++ = '\0';
    third = strchr(second, '.');
    if (third == NULL || strchr(third + 1, '.') != NULL) return 0;
    *third++ = '\0';
    if (strcmp(first, "toPoint") != 0 ||
        (strcmp(second, "glide") != 0 &&
         strcmp(second, "teleport") != 0) ||
        *storage == '\0' || *third == '\0') return 0;
    *name = storage; *action = second; *target = third;
    return 1;
}

static ZSharpGameObject *find_ai(const ZSharpGameModel *model,
                                 const char *name) {
    size_t index;
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *object = &model->objects[index];
        if (object->is_ai && object->scene != NULL &&
            model->active_scene != NULL &&
            strcmp(object->scene, model->active_scene) == 0 &&
            strcmp(object->name, name) == 0) return object;
    }
    return NULL;
}

static int sight_query(const ZSharpGameModel *model, const char *path,
                       ZSharpGameObject **observer,
                       ZSharpGameObject **target) {
    char storage[512], *first, *second;
    size_t index;
    if (path == NULL || strlen(path) + 1 > sizeof(storage)) return 0;
    strcpy(storage, path);
    first = strchr(storage, '.');
    if (first == NULL) return 0;
    *first++ = '\0';
    second = strchr(first, '.');
    if (second == NULL || strchr(second + 1, '.') != NULL) return 0;
    *second++ = '\0';
    if (strcmp(first, "canSee") != 0) return 0;
    *observer = find_ai(model, storage);
    *target = NULL;
    if (*observer == NULL) return 0;
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *candidate = &model->objects[index];
        if (candidate->scene != NULL && model->active_scene != NULL &&
            strcmp(candidate->scene, model->active_scene) == 0 &&
            strcmp(candidate->name, second) == 0) {
            *target = candidate;
            return 1;
        }
    }
    return 0;
}

int zsharp_game_navigation_has_sight_query(const ZSharpGameModel *model,
                                            const char *path) {
    ZSharpGameObject *observer, *target;
    return sight_query(model, path, &observer, &target);
}

int zsharp_game_navigation_can_see(const ZSharpGameModel *model,
                                   const char *path) {
    ZSharpGameObject *observer, *target;
    size_t index;
    if (!sight_query(model, path, &observer, &target) ||
        !observer->visible || !target->visible) return 0;
    for (index = 0; index < model->object_count; index++) {
        const ZSharpGameObject *blocker = &model->objects[index];
        float origin[3], direction[3], center[3], half[3];
        float entry = 0.001f, leave = 0.999f;
        int axis;
        if (blocker == observer || blocker == target ||
            blocker->scene == NULL || model->active_scene == NULL ||
            strcmp(blocker->scene, model->active_scene) != 0 ||
            !blocker->visible || blocker->trigger ||
            blocker->collider == ZGAME_COLLIDER_NONE ||
            blocker->shape == ZGAME_SHAPE_NAV ||
            blocker->shape == ZGAME_SHAPE_LIGHT) continue;
        origin[0] = observer->x;
        origin[1] = observer->y;
        origin[2] = observer->z;
        direction[0] = target->x - origin[0];
        direction[1] = target->y - origin[1];
        direction[2] = target->z - origin[2];
        center[0] = blocker->x;
        center[1] = blocker->y;
        center[2] = blocker->z;
        half[0] = fabsf(blocker->width * blocker->scale_x) * 0.5f;
        half[1] = fabsf(blocker->height * blocker->scale_y) * 0.5f;
        half[2] = fabsf(blocker->depth * blocker->scale_z) * 0.5f;
        for (axis = 0; axis < 3; axis++) {
            float low = center[axis] - half[axis];
            float high = center[axis] + half[axis];
            float first, last;
            if (fabsf(direction[axis]) < 0.00001f) {
                if (origin[axis] < low || origin[axis] > high) break;
                continue;
            }
            first = (low - origin[axis]) / direction[axis];
            last = (high - origin[axis]) / direction[axis];
            if (first > last) { float swap = first; first = last; last = swap; }
            if (first > entry) entry = first;
            if (last < leave) leave = last;
            if (entry > leave) break;
        }
        if (axis == 3 && entry <= leave) return 0;
    }
    return 1;
}

int zsharp_game_navigation_has_command(const ZSharpGameModel *model,
                                        const char *path) {
    char storage[512], *name, *action, *target;
    return split_command(path, storage, sizeof(storage), &name, &action,
                         &target) && find_ai(model, name) != NULL;
}

static const char *resolve_nav_id(const ZSharpGameObject *ai,
                                   const char *alias) {
    size_t index;
    for (index = 0; index < ai->nav_count; index++) {
        if (strcmp(ai->nav_aliases[index], alias) == 0 ||
            strcmp(ai->nav_ids[index], alias) == 0)
            return ai->nav_ids[index];
    }
    return NULL;
}

static int same_scene(const ZSharpGameModel *model,
                      const ZSharpGameObject *object) {
    return model->active_scene != NULL && object->scene != NULL &&
           strcmp(model->active_scene, object->scene) == 0;
}

static int line_hits_box(const ZSharpGameObject *ai,
                         const ZSharpGameObject *box,
                         float start_x, float start_z,
                         float end_x, float end_z) {
    float half_x = fabsf(box->width * box->scale_x) * 0.5f +
                   fabsf(ai->width * ai->scale_x) * 0.5f;
    float half_z = fabsf(box->depth * box->scale_z) * 0.5f +
                   fabsf(ai->depth * ai->scale_z) * 0.5f;
    float half_y = fabsf(box->height * box->scale_y) * 0.5f +
                   fabsf(ai->height * ai->scale_y) * 0.5f;
    float entry = 0.001f, leave = 0.999f;
    float origin[2] = {start_x, start_z};
    float direction[2] = {end_x - start_x, end_z - start_z};
    float center[2] = {box->x, box->z};
    float half[2] = {half_x, half_z};
    int axis;
    if (fabsf(ai->y - box->y) > half_y) return 0;
    for (axis = 0; axis < 2; axis++) {
        float low = center[axis] - half[axis];
        float high = center[axis] + half[axis];
        float first, second;
        if (fabsf(direction[axis]) < 0.00001f) {
            if (origin[axis] < low || origin[axis] > high) return 0;
            continue;
        }
        first = (low - origin[axis]) / direction[axis];
        second = (high - origin[axis]) / direction[axis];
        if (first > second) { float swap = first; first = second; second = swap; }
        if (first > entry) entry = first;
        if (second < leave) leave = second;
        if (entry > leave) return 0;
    }
    return entry <= leave;
}

static int path_clear(const ZSharpGameModel *model,
                      const ZSharpGameObject *ai,
                      float ax, float az, float bx, float bz) {
    size_t index;
    for (index = 0; index < model->object_count; index++) {
        const ZSharpGameObject *box = &model->objects[index];
        if (box == ai || !same_scene(model, box) || !box->visible ||
            box->body != ZGAME_BODY_STATIC ||
            box->collider == ZGAME_COLLIDER_NONE || box->trigger ||
            box->shape == ZGAME_SHAPE_NAV ||
            box->shape == ZGAME_SHAPE_LIGHT) continue;
        if (line_hits_box(ai, box, ax, az, bx, bz)) return 0;
    }
    return 1;
}

static float distance_xz(float ax, float az, float bx, float bz) {
    float dx = bx - ax, dz = bz - az;
    return sqrtf(dx*dx + dz*dz);
}

static int build_path(ZSharpGameModel *model, ZSharpGameObject *ai,
                      size_t target_index, char *error, size_t error_size) {
    size_t nodes[ZSHARP_MAX_NAV_POINTS + 1];
    float distance[ZSHARP_MAX_NAV_POINTS + 1];
    int previous[ZSHARP_MAX_NAV_POINTS + 1];
    unsigned char visited[ZSHARP_MAX_NAV_POINTS + 1] = {0};
    size_t count = 1, target = 0, index, iteration;
    nodes[0] = (size_t)(ai - model->objects);
    for (index = 0; index < model->object_count; index++) {
        const ZSharpGameObject *point = &model->objects[index];
        if (point->shape != ZGAME_SHAPE_NAV || !same_scene(model, point)) continue;
        if (count > ZSHARP_MAX_NAV_POINTS) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size,
                         "AI navigation supports at most %d points per scene",
                         ZSHARP_MAX_NAV_POINTS);
            return 0;
        }
        nodes[count] = index;
        if (index == target_index) target = count;
        count++;
    }
    if (target == 0) return 0;
    for (index = 0; index < count; index++) {
        distance[index] = INFINITY;
        previous[index] = -1;
    }
    distance[0] = 0.0f;
    for (iteration = 0; iteration < count; iteration++) {
        size_t current = count, neighbor;
        for (index = 0; index < count; index++) {
            if (!visited[index] &&
                (current == count || distance[index] < distance[current]))
                current = index;
        }
        if (current == count || !isfinite(distance[current]) ||
            current == target) break;
        visited[current] = 1;
        for (neighbor = 1; neighbor < count; neighbor++) {
            const ZSharpGameObject *a = &model->objects[nodes[current]];
            const ZSharpGameObject *b = &model->objects[nodes[neighbor]];
            float candidate;
            if (neighbor == current || visited[neighbor] ||
                !path_clear(model, ai, a->x, a->z, b->x, b->z)) continue;
            candidate = distance[current] + distance_xz(a->x, a->z, b->x, b->z);
            if (candidate < distance[neighbor]) {
                distance[neighbor] = candidate;
                previous[neighbor] = (int)current;
            }
        }
    }
    if (!isfinite(distance[target])) return 0;
    {
        unsigned reversed[ZSHARP_MAX_NAV_POINTS];
        size_t length = 0, position;
        int cursor = (int)target;
        while (cursor > 0 && length < ZSHARP_MAX_NAV_POINTS) {
            reversed[length++] = (unsigned)nodes[cursor];
            cursor = previous[cursor];
        }
        if (cursor != 0 || length == 0) return 0;
        ai->nav_path = (float *)malloc(length * 2 * sizeof(float));
        if (ai->nav_path == NULL) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size, "out of memory building AI route");
            return 0;
        }
        ai->nav_path_count = length;
        ai->nav_path_step = 0;
        for (position = 0; position < length; position++) {
            const ZSharpGameObject *point = &model->objects[
                reversed[length - position - 1]];
            ai->nav_path[position * 2] = point->x;
            ai->nav_path[position * 2 + 1] = point->z;
        }
    }
    return 1;
}

/* Generate a bounded X/Z walk grid when authored navigation points do not
 * provide a route. This keeps the public nav:target syntax while allowing
 * simple levels to route around static geometry automatically. */
static int build_grid_path(ZSharpGameModel *model, ZSharpGameObject *ai,
                           const ZSharpGameObject *goal) {
    enum { SIDE = 64, CELLS = SIDE * SIDE };
    int previous[CELLS], queue[CELLS], reversed[CELLS];
    int columns, rows, head = 0, tail = 0, found = -1;
    float min_x = fminf(ai->x, goal->x), max_x = fmaxf(ai->x, goal->x);
    float min_z = fminf(ai->z, goal->z), max_z = fmaxf(ai->z, goal->z);
    float step = fmaxf(3.0f,
        fmaxf(fabsf(ai->width * ai->scale_x),
              fabsf(ai->depth * ai->scale_z)) * 0.75f);
    size_t object_index;
    int index, length = 0;
    for (object_index = 0; object_index < model->object_count; object_index++) {
        const ZSharpGameObject *box = &model->objects[object_index];
        float half_y = fabsf(box->height * box->scale_y) * 0.5f +
                       fabsf(ai->height * ai->scale_y) * 0.5f;
        float half_x, half_z;
        if (box == ai || !same_scene(model, box) || !box->visible ||
            box->body != ZGAME_BODY_STATIC || box->trigger ||
            box->collider == ZGAME_COLLIDER_NONE ||
            box->shape == ZGAME_SHAPE_NAV ||
            box->shape == ZGAME_SHAPE_LIGHT ||
            fabsf(ai->y - box->y) > half_y) continue;
        half_x = fabsf(box->width * box->scale_x) * 0.5f;
        half_z = fabsf(box->depth * box->scale_z) * 0.5f;
        min_x = fminf(min_x, box->x - half_x);
        max_x = fmaxf(max_x, box->x + half_x);
        min_z = fminf(min_z, box->z - half_z);
        max_z = fmaxf(max_z, box->z + half_z);
    }
    min_x -= step * 2.0f; max_x += step * 2.0f;
    min_z -= step * 2.0f; max_z += step * 2.0f;
    step = fmaxf(step, fmaxf((max_x-min_x)/63.0f,
                             (max_z-min_z)/63.0f));
    columns = (int)ceilf((max_x-min_x)/step) + 1;
    rows = (int)ceilf((max_z-min_z)/step) + 1;
    if (columns > SIDE) columns = SIDE;
    if (rows > SIDE) rows = SIDE;
    for (index = 0; index < CELLS; index++) previous[index] = -2;
    for (index = 0; index < columns * rows; index++) {
        float x = min_x + (float)(index % columns) * step;
        float z = min_z + (float)(index / columns) * step;
        if (distance_xz(ai->x, ai->z, x, z) > step * 1.8f ||
            !path_clear(model, ai, ai->x, ai->z, x, z)) continue;
        previous[index] = -1;
        queue[tail++] = index;
    }
    while (head < tail) {
        int current = queue[head++];
        int column = current % columns, row = current / columns;
        float x = min_x + (float)column * step;
        float z = min_z + (float)row * step;
        int direction;
        if (path_clear(model, ai, x, z, goal->x, goal->z)) {
            found = current;
            break;
        }
        for (direction = 0; direction < 4; direction++) {
            int next_column = column + (direction == 0) - (direction == 1);
            int next_row = row + (direction == 2) - (direction == 3);
            int next;
            float next_x, next_z;
            if (next_column < 0 || next_row < 0 ||
                next_column >= columns || next_row >= rows) continue;
            next = next_row * columns + next_column;
            if (previous[next] != -2) continue;
            next_x = min_x + (float)next_column * step;
            next_z = min_z + (float)next_row * step;
            if (!path_clear(model, ai, x, z, next_x, next_z)) continue;
            previous[next] = current;
            queue[tail++] = next;
        }
    }
    if (found < 0) return 0;
    for (index = found; index >= 0 && length < CELLS;
         index = previous[index]) reversed[length++] = index;
    ai->nav_path = (float *)malloc((size_t)(length + 1) * 2 * sizeof(float));
    if (ai->nav_path == NULL) return 0;
    ai->nav_path_count = 0;
    {
        float from_x = ai->x, from_z = ai->z;
        int cursor = length - 1;
        while (cursor >= 0) {
            int farthest = cursor;
            int candidate;
            for (candidate = 0; candidate < cursor; candidate++) {
                int cell = reversed[candidate];
                float x = min_x + (float)(cell % columns) * step;
                float z = min_z + (float)(cell / columns) * step;
                if (path_clear(model, ai, from_x, from_z, x, z)) {
                    farthest = candidate;
                    break;
                }
            }
            {
                int cell = reversed[farthest];
                from_x = min_x + (float)(cell % columns) * step;
                from_z = min_z + (float)(cell / columns) * step;
                ai->nav_path[ai->nav_path_count * 2] = from_x;
                ai->nav_path[ai->nav_path_count * 2 + 1] = from_z;
                ai->nav_path_count++;
            }
            cursor = farthest - 1;
        }
    }
    ai->nav_path[ai->nav_path_count * 2] = goal->x;
    ai->nav_path[ai->nav_path_count * 2 + 1] = goal->z;
    ai->nav_path_count++;
    ai->nav_path_step = 0;
    return 1;
}

int zsharp_game_navigation_command(ZSharpGameModel *model,
                                    const char *path, const char *value,
                                    char *error, size_t error_size) {
    char storage[512], *name, *action, *alias;
    ZSharpGameObject *ai, *point = NULL;
    const char *id;
    size_t index;
    float speed = 0.0f;
    if (!split_command(path, storage, sizeof(storage), &name, &action,
                       &alias) || (ai = find_ai(model, name)) == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "unknown AI navigation command");
        return 0;
    }
    id = resolve_nav_id(ai, alias);
    if (id == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size,
                     "AI '%s' has no Navigation alias '%s'", name, alias);
        return 0;
    }
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *candidate = &model->objects[index];
        if (candidate->shape == ZGAME_SHAPE_NAV && same_scene(model, candidate) &&
            strncmp(candidate->name, "nav:", 4) == 0 &&
            strcmp(candidate->name + 4, id) == 0) {
            point = candidate;
            break;
        }
    }
    if (point == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size,
                     "Navigation point 'nav:%s' is not in the active scene", id);
        return 0;
    }
    free(ai->nav_path);
    ai->nav_path = NULL;
    ai->nav_path_count = ai->nav_path_step = 0;
    free(ai->nav_target);
    ai->nav_target = zsharp_copy_text(id, strlen(id));
    if (ai->nav_target == NULL) return 0;
    ai->nav_moving = 0;
    ai->nav_reachable = 1;
    if (strcmp(action, "teleport") == 0) {
        ai->x = point->x; ai->y = point->y; ai->z = point->z;
        ai->velocity_x = ai->velocity_y = ai->velocity_z = 0.0f;
        return 1;
    }
    if (value != NULL) {
        char *end = NULL;
        double parsed = strtod(value, &end);
        if (end == value || end == NULL || *end != '\0' ||
            !isfinite(parsed) || parsed <= 0.0 || parsed > 1000000.0) {
            if (error != NULL && error_size > 0)
                snprintf(error, error_size,
                         "AI glide speed must be a positive number");
            return 0;
        }
        speed = (float)parsed;
    }
    ai->nav_speed = speed;
    ai->nav_reachable = build_path(model, ai, index, error, error_size);
    if (!ai->nav_reachable)
        ai->nav_reachable = build_grid_path(model, ai, point);
    ai->nav_moving = ai->nav_reachable;
    return 1;
}

void zsharp_game_navigation_update(ZSharpGameModel *model,
                                    double delta_seconds) {
    size_t index;
    for (index = 0; index < model->object_count; index++) {
        ZSharpGameObject *ai = &model->objects[index];
        float dx, dz, distance;
        if (!ai->is_ai || !ai->nav_moving || !same_scene(model, ai)) continue;
        if (ai->nav_path_step >= ai->nav_path_count) {
            ai->nav_moving = 0;
            ai->velocity_x = ai->velocity_z = 0.0f;
            continue;
        }
        dx = ai->nav_path[ai->nav_path_step * 2] - ai->x;
        dz = ai->nav_path[ai->nav_path_step * 2 + 1] - ai->z;
        distance = sqrtf(dx*dx + dz*dz);
        if (distance <= ai->nav_speed * (float)delta_seconds + 0.25f) {
            ai->x = ai->nav_path[ai->nav_path_step * 2];
            ai->z = ai->nav_path[ai->nav_path_step * 2 + 1];
            ai->nav_path_step++;
            ai->velocity_x = ai->velocity_z = 0.0f;
            if (ai->nav_path_step == ai->nav_path_count)
                ai->nav_moving = 0;
            continue;
        }
        ai->velocity_x = ai->nav_speed * dx / distance;
        ai->velocity_z = ai->nav_speed * dz / distance;
    }
}
