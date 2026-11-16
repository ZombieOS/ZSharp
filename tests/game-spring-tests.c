#include "game_model.h"
#include "game_spring.h"
#include <ufbx.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test, message) do { if (!(test)) { \
    fprintf(stderr, "spring regression: %s (line %d)\n", message, __LINE__); return 0; \
} } while (0)

typedef struct Rig {
    ZSharpGameModel model;
    ZSharpGameObject objects[2];
} Rig;

static const char *fixture_path;
static char fixture_root[4096];
static const char *fixture_name;
static ufbx_scene *fixture_scene;
static char *bone_names[] = {"ManeRoot", "ManeMid", "ManeTip"};

static ZSharpSpringChain chain_config(void) {
    ZSharpSpringChain chain = {0};
    chain.name = "Mane";
    chain.bones = bone_names;
    chain.bone_count = 3;
    chain.stiffness = 20;
    chain.damping = 4;
    chain.gravity[1] = -9.8f;
    chain.radius = .02f;
    chain.max_angle = 60;
    return chain;
}

static int rig_prepare(Rig *rig, const ZSharpSpringConfig *config, size_t count) {
    size_t index;
    char error[512] = {0};
    memset(rig, 0, sizeof(*rig));
    rig->model.objects = rig->objects;
    rig->model.object_count = count;
    rig->model.is_3d = 1;
    rig->model.active_scene = "SpringTest";
    rig->model.project_root = fixture_root;
    rig->model.scene_generation = 1;
    for (index = 0; index < count; index++) {
        ZSharpGameObject *object = &rig->objects[index];
        object->name = "Mane";
        object->display_name = index ? "Second" : "First";
        object->scene = "SpringTest";
        object->shape = ZGAME_SHAPE_MESH;
        object->mesh_path = (char *)fixture_name;
        object->width = 2;
        object->height = object->depth = .2f;
        object->scale_x = object->scale_y = object->scale_z = 1;
        object->visible = object->spring_bones_enabled = 1;
        CHECK(zsharp_game_spring_config_copy(&object->spring_config, config), "copy config");
    }
    if (!zsharp_game_springs_prepare(&rig->model, error, sizeof(error))) {
        fprintf(stderr, "spring preparation: %s\n", error);
        return 0;
    }
    zsharp_game_springs_update(&rig->model, 0);
    return 1;
}

static void rig_free(Rig *rig) {
    size_t index;
    zsharp_game_springs_free(&rig->model);
    for (index = 0; index < rig->model.object_count; index++)
        zsharp_game_spring_config_free(rig->objects[index].spring_config);
}

static double pose_amount(const ZSharpGameObject *object) {
    size_t count, index;
    double amount = 0;
    const ZSharpSpringBonePose *poses = zsharp_game_spring_poses(object->spring_state, &count);
    for (index = 0; index < count; index++) {
        amount += fabs(poses[index].rotation[0]) + fabs(poses[index].rotation[1]) +
                  fabs(poses[index].rotation[2]);
    }
    return amount;
}

static int valid_poses(const ZSharpGameObject *object, double max_degrees) {
    size_t count, index, axis;
    const ZSharpSpringBonePose *poses = zsharp_game_spring_poses(object->spring_state, &count);
    CHECK(count == 2 && poses != NULL, "one rotation per segment, excluding the tip marker");
    for (index = 0; index < count; index++) {
        double norm = 0, angle;
        CHECK(poses[index].name != NULL, "named spring rotation");
        for (axis = 0; axis < 4; axis++) {
            CHECK(isfinite(poses[index].rotation[axis]), "finite quaternion");
            norm += poses[index].rotation[axis] * poses[index].rotation[axis];
        }
        CHECK(fabs(norm - 1) < .0001, "unit quaternion");
        angle = 2 * acos(fmin(1, fabs(poses[index].rotation[3]))) * 180 / 3.141592653589793;
        CHECK(angle <= max_degrees + .05, "bend is bounded");
    }
    return 1;
}

/* Use ufbx's actual FK and weighted skinning as an independent consumer of
   the public quaternion poses, including existing authored bone offsets. */
static ufbx_scene *evaluate_rig(const ZSharpGameObject *object, int springs) {
    ufbx_transform_override overrides[3];
    ufbx_anim_opts options = {0};
    ufbx_evaluate_opts evaluation = {0};
    ufbx_error error;
    ufbx_anim *animation;
    ufbx_scene *result;
    const ZSharpSpringBonePose *poses;
    size_t count, index, pose_index;
    poses = zsharp_game_spring_poses(object->spring_state, &count);
    for (index = 0; index < 3; index++) {
        const ufbx_node *node = ufbx_find_node(fixture_scene, bone_names[index]);
        overrides[index].node_id = node->typed_id;
        overrides[index].transform = node->local_transform;
        for (pose_index = 0; pose_index < object->part_pose_count; pose_index++) {
            const ZSharpGamePartPose *pose = &object->part_poses[pose_index];
            if (!strcmp(pose->name, bone_names[index])) {
                ufbx_vec3 angles = {0};
                angles.x = pose->rotation[0]; angles.y = pose->rotation[1]; angles.z = pose->rotation[2];
                overrides[index].transform.translation.x += pose->position[0];
                overrides[index].transform.translation.y += pose->position[1];
                overrides[index].transform.translation.z += pose->position[2];
                overrides[index].transform.rotation = ufbx_quat_mul(
                    overrides[index].transform.rotation, ufbx_euler_to_quat(angles, node->rotation_order));
            }
        }
        if (springs) for (pose_index = 0; pose_index < count; pose_index++) {
            const float *rotation = poses[pose_index].rotation;
            if (!strcmp(poses[pose_index].name, bone_names[index])) {
                ufbx_quat delta = {0};
                delta.x = rotation[0]; delta.y = rotation[1]; delta.z = rotation[2]; delta.w = rotation[3];
                overrides[index].transform.rotation = ufbx_quat_mul(overrides[index].transform.rotation, delta);
            }
        }
    }
    options.transform_overrides.data = overrides;
    options.transform_overrides.count = 3;
    animation = ufbx_create_anim(fixture_scene, &options, &error);
    if (!animation) return NULL;
    evaluation.evaluate_skinning = true;
    result = ufbx_evaluate_scene(fixture_scene, animation, 0, &evaluation, &error);
    ufbx_free_anim(animation);
    return result;
}

static ufbx_vec3 bone_position(const ufbx_scene *scene, size_t index) {
    const ufbx_node *node = ufbx_find_node(scene, bone_names[index]);
    ufbx_vec3 point = {0};
    point.x = node->node_to_world.m03; point.y = node->node_to_world.m13; point.z = node->node_to_world.m23;
    return point;
}

static double distance(ufbx_vec3 first, ufbx_vec3 second) {
    double x = first.x - second.x, y = first.y - second.y, z = first.z - second.z;
    return sqrt(x*x + y*y + z*z);
}

static int validation_tests(void) {
    ZSharpSpringChain chain = chain_config();
    ZSharpSpringConfig config = {&chain, 1, NULL, 0};
    ZSharpSpringConfig *copy = NULL;
    ZSharpSpringCollider collider = {0};
    char error[512] = {0};
    float *fields[] = {&chain.stiffness, &chain.damping, &chain.gravity[0], &chain.radius, &chain.max_angle};
    float invalid[] = {-1, -1, 10001, -1, 181};
    size_t index;
    CHECK(zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "valid authored three-bone chain");
    CHECK(zsharp_game_spring_config_copy(&copy, &config), "deep config copy");
    CHECK(copy != &config && copy->chains != &chain && copy->chains[0].bones != bone_names &&
          copy->chains[0].bones[0] != bone_names[0], "config strings and arrays have independent ownership");
    zsharp_game_spring_config_free(copy);
    for (index = 0; index < sizeof(fields)/sizeof(fields[0]); index++) {
        float previous = *fields[index];
        *fields[index] = invalid[index];
        CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "out-of-range config rejected");
        *fields[index] = NAN;
        CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "NaN config rejected");
        *fields[index] = INFINITY;
        CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "infinite config rejected");
        *fields[index] = previous;
    }
    chain.bone_count = 1;
    CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "tip marker required");
    chain.bone_count = 3;
    {
        char *bad_parent[] = {"ManeRoot", "ManeTip"};
        char *missing[] = {"ManeRoot", "UnknownBone"};
        chain.bones = bad_parent; chain.bone_count = 2;
        CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "skipped direct parent rejected");
        chain.bones = missing;
        CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "missing bone rejected");
        chain.bones = bone_names; chain.bone_count = 3;
    }
    collider.name = "Chest"; collider.bone = "UnknownBone"; collider.radius = .2f;
    config.colliders = &collider; config.collider_count = 1;
    CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "unknown collider anchor rejected");
    collider.bone = "ManeRoot"; collider.radius = 0;
    CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "zero collider radius rejected");
    collider.radius = .2f; collider.offset[1] = NAN;
    CHECK(!zsharp_game_spring_validate(&config, fixture_scene, error, sizeof(error)), "nonfinite collider offset rejected");
    return 1;
}

static int gravity_skinning_and_step_tests(void) {
    Rig slow, fast;
    ZSharpSpringChain chain = chain_config();
    ZSharpSpringConfig config = {&chain, 1, NULL, 0};
    ufbx_scene *posed, *baseline;
    size_t index, count1, count2;
    double largest_change = 0;
    const ZSharpSpringBonePose *first, *second;
    CHECK(rig_prepare(&slow, &config, 1) && rig_prepare(&fast, &config, 1), "prepare fixed-step pair");
    for (index = 0; index < 120; index++) zsharp_game_springs_update(&slow.model, 1.0/120.0);
    for (index = 0; index < 60; index++) zsharp_game_springs_update(&fast.model, 1.0/60.0);
    CHECK(valid_poses(&slow.objects[0], 60), "gravity rotations");
    CHECK(pose_amount(&slow.objects[0]) > .01, "world gravity bends a horizontal chain");
    first = zsharp_game_spring_poses(slow.objects[0].spring_state, &count1);
    second = zsharp_game_spring_poses(fast.objects[0].spring_state, &count2);
    CHECK(count1 == count2, "same fixed-step pose count");
    for (index = 0; index < count1*4; index++)
        CHECK(fabs(first[index/4].rotation[index%4] - second[index/4].rotation[index%4]) < .0002, "fixed-step simulation independent of caller frame subdivision");
    posed = evaluate_rig(&slow.objects[0], 1); baseline = evaluate_rig(&slow.objects[0], 0);
    CHECK(posed && baseline, "evaluate spring quaternions using real ufbx skinning");
    CHECK(bone_position(posed, 1).y < -.01, "spring bends downward");
    for (index = 0; index < 2; index++)
        CHECK(fabs(distance(bone_position(posed, index), bone_position(posed, index+1))-1) < .0001, "bone rest lengths preserved by hierarchy");
    for (index = 0; index < posed->meshes.data[0]->num_indices; index++) {
        ufbx_vec3 before = ufbx_get_vertex_vec3(&baseline->meshes.data[0]->skinned_position, (uint32_t)index);
        ufbx_vec3 after = ufbx_get_vertex_vec3(&posed->meshes.data[0]->skinned_position, (uint32_t)index);
        largest_change = fmax(largest_change, distance(before, after));
    }
    CHECK(largest_change > .01, "spring rotations deform actual weighted mesh vertices");
    ufbx_free_scene(posed); ufbx_free_scene(baseline);
    rig_free(&slow); rig_free(&fast);
    return 1;
}

static int motion_reset_and_isolation_tests(void) {
    Rig rig;
    ZSharpSpringChain chain = chain_config();
    ZSharpSpringConfig config = {&chain, 1, NULL, 0};
    ZSharpGamePartPose authored = {"ManeRoot", {0,0,0}, {0,0,25}};
    ufbx_scene *posed, *baseline;
    size_t index;
    chain.gravity[1] = 0;
    CHECK(rig_prepare(&rig, &config, 2), "prepare independent instances");
    rig.objects[0].y = .4f;
    zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(pose_amount(&rig.objects[0]) > .001, "spring lags object translation");
    CHECK(pose_amount(&rig.objects[1]) < .00001, "another placement remains independent");
    zsharp_game_springs_reset(&rig.model);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "explicit reset clears secondary pose");
    rig.objects[0].rotation_z = 35;
    zsharp_game_springs_update(&rig.model, 1.0/120.0);
    rig.objects[0].rotation_z = 60;
    zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(valid_poses(&rig.objects[0], 180), "object rotation gives finite local poses");
    CHECK(pose_amount(&rig.objects[0]) > .001, "secondary motion lags object rotation");
    rig.objects[0].x += 100;
    zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "teleport resets secondary motion");
    rig_free(&rig);
    chain.gravity[1] = -9.8f;
    CHECK(rig_prepare(&rig, &config, 1), "prepare reset and authored-animation checks");
    for (index = 0; index < 120; index++) zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(pose_amount(&rig.objects[0]) > .01, "motion can resume after teleport");
    rig.model.scene_generation++;
    zsharp_game_springs_update(&rig.model, 0);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "scene generation invalidates previous state");
    rig.objects[0].part_poses = &authored; rig.objects[0].part_pose_count = 1;
    zsharp_game_springs_reset(&rig.model);
    for (index = 0; index < 120; index++) zsharp_game_springs_update(&rig.model, 1.0/120.0);
    posed = evaluate_rig(&rig.objects[0], 1); baseline = evaluate_rig(&rig.objects[0], 0);
    CHECK(posed && baseline, "authored pose and springs both evaluate");
    CHECK(bone_position(baseline, 1).y > .4, "authored root rotation remains in base pose");
    CHECK(distance(bone_position(posed, 2), bone_position(baseline, 2)) > .01, "spring quaternion composes with authored animation");
    ufbx_free_scene(posed); ufbx_free_scene(baseline);
    rig.objects[0].scale_x = 1.2f; rig.objects[0].scale_y = .7f; rig.objects[0].scale_z = 1.5f;
    rig.objects[0].rotation_x = 15; rig.objects[0].rotation_y = 35;
    zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "scale change seeds new state");
    for (index = 0; index < 120; index++) zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(valid_poses(&rig.objects[0], 180), "rotated nonuniform scale produces valid quaternions");
    posed = evaluate_rig(&rig.objects[0], 1);
    CHECK(posed != NULL, "evaluate hierarchy after nonuniform instance scale");
    for (index = 0; index < 2; index++)
        CHECK(fabs(distance(bone_position(posed, index), bone_position(posed, index+1))-1) < .0001, "nonuniform instance scaling does not stretch local bones");
    ufbx_free_scene(posed);
    rig.objects[0].spring_bones_enabled = 0;
    zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "disable restores authored animation only");
    rig.objects[0].spring_bones_enabled = 1;
    zsharp_game_springs_update(&rig.model, 0);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "re-enable seeds fresh state");
    for (index = 0; index < 30; index++) zsharp_game_springs_update(&rig.model, 1.0/120.0);
    zsharp_game_springs_update(&rig.model, NAN);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "nonfinite elapsed time resets safely");
    zsharp_game_springs_update(&rig.model, INFINITY);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "infinite elapsed time resets safely");
    zsharp_game_springs_update(&rig.model, -.1);
    CHECK(pose_amount(&rig.objects[0]) < .00001, "negative elapsed time resets safely");
    zsharp_game_springs_update(&rig.model, 10);
    CHECK(valid_poses(&rig.objects[0], 180), "long pause cannot explode simulation");
    rig_free(&rig);
    return 1;
}

static int damping_test(void) {
    Rig free_rig, damped_rig;
    ZSharpSpringChain chain = chain_config();
    ZSharpSpringConfig config = {&chain, 1, NULL, 0};
    size_t index;
    double free_motion = 0, damped_motion = 0;
    chain.gravity[1] = 0; chain.damping = 0;
    CHECK(rig_prepare(&free_rig, &config, 1), "prepare undamped chain");
    chain.damping = 12;
    CHECK(rig_prepare(&damped_rig, &config, 1), "prepare damped chain");
    free_rig.objects[0].y = damped_rig.objects[0].y = .25f;
    for (index = 0; index < 1200; index++) {
        zsharp_game_springs_update(&free_rig.model, 1.0/120.0);
        zsharp_game_springs_update(&damped_rig.model, 1.0/120.0);
        if (index >= 1080) {
            free_motion += pose_amount(&free_rig.objects[0]);
            damped_motion += pose_amount(&damped_rig.objects[0]);
        }
    }
    CHECK(free_motion > .001 && damped_motion < free_motion*.25, "damping dissipates sustained secondary motion");
    rig_free(&free_rig); rig_free(&damped_rig);
    return 1;
}

static int contact_test(int capsule) {
    Rig rig;
    ZSharpSpringChain chain = chain_config();
    ZSharpSpringCollider collider = {0};
    ZSharpSpringConfig config = {&chain, 1, &collider, 1};
    size_t step, index;
    int contact_observed = 0;
    chain.stiffness = 0; chain.damping = 8; chain.radius = .05f; chain.max_angle = 180;
    collider.name = "Chest"; collider.bone = "ManeRoot";
    collider.offset[0] = 1; collider.offset[1] = -.4f;
    collider.radius = .35f; collider.capsule = capsule;
    memcpy(collider.tail, collider.offset, sizeof(collider.tail));
    if (capsule) { collider.offset[2] = -.3f; collider.tail[2] = .3f; }
    CHECK(rig_prepare(&rig, &config, 1), "prepare spring collider");
    for (step = 0; step < 240; step++) {
        ufbx_scene *posed;
        zsharp_game_springs_update(&rig.model, 1.0/120.0);
        posed = evaluate_rig(&rig.objects[0], 1);
        CHECK(posed != NULL, "evaluate colliding bone hierarchy");
        for (index = 1; index < 3; index++) {
            ufbx_vec3 point = bone_position(posed, index);
            double z = capsule ? fmax(-.3, fmin(.3, point.z)) : 0;
            ufbx_vec3 closest = {0};
            closest.x = 1; closest.y = -.4; closest.z = z;
            double gap = distance(point, closest);
            if (gap < .405) contact_observed = 1;
            if (gap < .395) fprintf(stderr, "contact gap %.6f (%s step %zu bone %zu)\n", gap, capsule ? "capsule" : "sphere", step, index);
            CHECK(gap >= .395, "actual posed endpoints stay outside collider plus tip radius");
        }
        ufbx_free_scene(posed);
    }
    CHECK(contact_observed, "trajectory actually reaches collision boundary");
    rig_free(&rig);
    return 1;
}

static int impossible_contact_test(void) {
    Rig rig;
    ZSharpSpringChain chain = chain_config();
    ZSharpSpringCollider collider = {0};
    ZSharpSpringConfig config = {&chain, 1, &collider, 1};
    ufbx_scene *posed;
    size_t index;
    chain.max_angle = 0;
    collider.name = "EnclosingBody"; collider.bone = "ManeRoot"; collider.radius = 10;
    CHECK(rig_prepare(&rig, &config, 1), "prepare deliberately impossible collision geometry");
    for (index = 0; index < 120; index++) zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(valid_poses(&rig.objects[0], 0), "impossible contact retains finite quaternions and zero angle limit");
    posed = evaluate_rig(&rig.objects[0], 1);
    CHECK(posed != NULL, "impossible contact still renders a valid rig");
    for (index = 0; index < 2; index++)
        CHECK(fabs(distance(bone_position(posed, index), bone_position(posed, index+1))-1) < .0001, "impossible collision does not stretch bones");
    ufbx_free_scene(posed);
    rig_free(&rig);
    return 1;
}

static int coincident_contact_test(int capsule) {
    Rig rig;
    ZSharpSpringChain chain = chain_config();
    ZSharpSpringCollider collider = {0};
    ZSharpSpringConfig config = {&chain, 1, &collider, 1};
    ufbx_scene *posed;
    ufbx_vec3 point, closest = {0};
    size_t index;
    double gap;
    chain.gravity[1] = 0; chain.stiffness = 0;
    collider.name = "CoincidentBody"; collider.bone = "ManeRoot";
    collider.radius = .2f; collider.offset[0] = 1; collider.capsule = capsule;
    memcpy(collider.tail, collider.offset, sizeof(collider.tail));
    if (capsule) { collider.offset[2] = -.3f; collider.tail[2] = .3f; }
    CHECK(rig_prepare(&rig, &config, 1), "prepare collider exactly centered on initial joint");
    for (index = 0; index < 8; index++) zsharp_game_springs_update(&rig.model, 1.0/120.0);
    CHECK(valid_poses(&rig.objects[0], 60), "coincident contact has finite bounded quaternions");
    posed = evaluate_rig(&rig.objects[0], 1);
    CHECK(posed != NULL, "evaluate exact-coincidence contact through actual hierarchy");
    point = bone_position(posed, 1);
    closest.x = 1;
    closest.z = capsule ? fmax(-.3, fmin(.3, point.z)) : 0;
    gap = distance(point, closest);
    if (gap < .215) fprintf(stderr, "coincident %s contact gap %.6f\n", capsule ? "capsule" : "sphere", gap);
    CHECK(gap >= .215, "a feasible sideways bend escapes a collider centered on the initial tip");
    for (index = 0; index < 2; index++)
        CHECK(fabs(distance(bone_position(posed, index), bone_position(posed, index+1))-1) < .0001, "coincident collision preserves actual bone lengths");
    ufbx_free_scene(posed);
    rig_free(&rig);
    return 1;
}

int main(int argc, char **argv) {
    ufbx_error error;
    ufbx_evaluate_opts options = {0};
    ufbx_scene *animated;
    const char *separator, *backslash;
    size_t root_size;
    if (argc != 2) { fputs("usage: game-spring-tests Chain.fbx\n", stderr); return 2; }
    fixture_path = argv[1];
    separator = strrchr(fixture_path, '/');
    backslash = strrchr(fixture_path, '\\');
    if (backslash && (!separator || backslash > separator)) separator = backslash;
    root_size = separator ? (size_t)(separator-fixture_path) : 0;
    if (root_size >= sizeof(fixture_root)) return 2;
    if (separator) {
        memcpy(fixture_root, fixture_path, root_size);
        fixture_root[root_size] = '\0';
        if (!root_size) strcpy(fixture_root, "/");
        fixture_name = separator+1;
    } else {
        strcpy(fixture_root, "."); fixture_name = fixture_path;
    }
    fixture_scene = ufbx_load_file(fixture_path, NULL, &error);
    if (!fixture_scene) { fprintf(stderr, "FBX fixture: %s\n", error.description.data); return 1; }
    if (fixture_scene->bones.count != 3 || fixture_scene->meshes.count != 1 ||
        fixture_scene->meshes.data[0]->skin_deformers.count != 1 || fixture_scene->anim_stacks.count != 1) {
        fputs("fixture must contain a real animated, weighted three-bone mesh\n", stderr); return 1;
    }
    options.evaluate_skinning = true;
    animated = ufbx_evaluate_scene(fixture_scene, fixture_scene->anim, 1, &options, &error);
    if (!animated || bone_position(animated, 2).y < .5) {
        fputs("fixture's authored animation did not rotate the weighted chain\n", stderr); return 1;
    }
    ufbx_free_scene(animated);
    if (!validation_tests() || !gravity_skinning_and_step_tests() ||
        !motion_reset_and_isolation_tests() || !damping_test() ||
        !contact_test(0) || !contact_test(1) || !impossible_contact_test() ||
        !coincident_contact_test(0) || !coincident_contact_test(1)) return 1;
    ufbx_free_scene(fixture_scene);
    puts("Spring bones: config, fixed-step motion, real skinning, damping, reset, isolation and contacts passed");
    return 0;
}
