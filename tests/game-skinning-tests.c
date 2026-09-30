#include <ufbx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    ufbx_scene *scene, *posed;
    const ufbx_node *bone = NULL;
    const ufbx_node *mesh_node = NULL;
    ufbx_transform_override override = {0};
    ufbx_anim_opts options = {0};
    ufbx_evaluate_opts evaluate_options = {0};
    ufbx_anim *animation;
    ufbx_error error;
    ufbx_vec3 angle = {0};
    ufbx_vec3 before, after;
    size_t index, vertex;
    double largest_change = 0.0;
    if (argc != 2) return 2;
    scene = ufbx_load_file(argv[1], NULL, &error);
    if (scene == NULL) {
        fprintf(stderr, "could not load skinning fixture: %s\n", error.description.data);
        return 1;
    }
    for (index = 0; index < scene->nodes.count; index++) {
        const ufbx_node *node = scene->nodes.data[index];
        if (node->bone != NULL && node->name.length == 7 &&
            memcmp(node->name.data, "Bone002", 7) == 0) bone = node;
        if (node->mesh != NULL && node->mesh->skin_deformers.count > 0)
            mesh_node = node;
    }
    if (bone == NULL || mesh_node == NULL) {
        fprintf(stderr, "skinning fixture lacks Bone002 or a skinned mesh\n");
        ufbx_free_scene(scene);
        return 1;
    }
    override.node_id = bone->typed_id;
    override.transform = bone->local_transform;
    angle.z = 70.0;
    override.transform.rotation = ufbx_quat_mul(
        bone->local_transform.rotation,
        ufbx_euler_to_quat(angle, bone->rotation_order));
    options.transform_overrides.data = &override;
    options.transform_overrides.count = 1;
    animation = ufbx_create_anim(scene, &options, &error);
    if (animation == NULL) {
        fprintf(stderr, "could not make bone pose: %s\n", error.description.data);
        ufbx_free_scene(scene);
        return 1;
    }
    evaluate_options.evaluate_skinning = true;
    posed = ufbx_evaluate_scene(scene, animation, 0.0,
                                &evaluate_options, &error);
    ufbx_free_anim(animation);
    if (posed == NULL) {
        fprintf(stderr, "could not evaluate bone pose: %s\n", error.description.data);
        ufbx_free_scene(scene);
        return 1;
    }
    for (vertex = 0; vertex < mesh_node->mesh->num_indices; vertex++) {
        const ufbx_node *new_node = posed->nodes.data[mesh_node->typed_id];
        before = ufbx_get_vertex_vec3(&mesh_node->mesh->skinned_position,
                                       (uint32_t)vertex);
        after = ufbx_get_vertex_vec3(&new_node->mesh->skinned_position,
                                      (uint32_t)vertex);
        if (mesh_node->mesh->skinned_is_local)
            before = ufbx_transform_position(&mesh_node->geometry_to_world, before);
        if (new_node->mesh->skinned_is_local)
            after = ufbx_transform_position(&new_node->geometry_to_world, after);
        {
            double change = fabs(after.x - before.x) +
                            fabs(after.y - before.y) + fabs(after.z - before.z);
            if (change > largest_change) largest_change = change;
        }
    }
    ufbx_free_scene(posed);
    ufbx_free_scene(scene);
    if (largest_change < 0.001) {
        fprintf(stderr, "bone override did not deform the skinned mesh\n");
        return 1;
    }
    printf("skinned vertex movement: %.6f\n", largest_change);
    return 0;
}
