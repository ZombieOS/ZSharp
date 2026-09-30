/* Exercise the actual collision helpers, not a second copy of the math. */
#define ZSHARP_COLLISION_TEST 1
#include "../native/src/game_model.c"

static ZSharpGameObject box(float x, float y, float z,
                            float width, float height, float depth,
                            float yaw) {
    ZSharpGameObject object;
    memset(&object, 0, sizeof(object));
    object.x = x; object.y = y; object.z = z;
    object.width = width; object.height = height; object.depth = depth;
    object.scale_x = object.scale_y = object.scale_z = 1.0f;
    object.rotation_y = yaw;
    object.collider = ZGAME_COLLIDER_BOX;
    return object;
}

int main(void) {
    ZSharpGameObject wall = box(0, 0, 0, 24, 6, 5, 45);
    ZSharpGameObject player = box(0, 0, 0, 4, 4, 4, 0);
    float normal[3], penetration;
    /* Beyond an unrotated wall's Z face, but inside its visible rotation. */
    player.x = -7; player.z = 7;
    if (!box_contact(&player, &wall, normal, &penetration) ||
        penetration <= 0.0f) return 1;
    resolve_round_collision(&player, &wall, normal, penetration);
    if (box_contact(&player, &wall, normal, &penetration)) return 2;
    player.x = 18; player.z = 0;
    if (box_contact(&player, &wall, normal, &penetration)) return 3;
    player = box(0, 4.9f, 0, 4, 4, 4, 0);
    if (!box_contact(&player, &wall, normal, &penetration) ||
        normal[1] < 0.9f) return 4;
    player.velocity_y = -1;
    resolve_round_collision(&player, &wall, normal, penetration);
    if (!player.grounded) return 5;
    player = box(-7, 0, 7, 4, 4, 4, 0);
    player.collider = ZGAME_COLLIDER_SPHERE;
    if (!round_box_contact(&player, &wall, normal, &penetration)) return 6;
    resolve_round_collision(&player, &wall, normal, penetration);
    if (round_box_contact(&player, &wall, normal, &penetration)) return 7;
    wall = box(0, 0, 0, 12, 2, 12, 0);
    wall.rotation_z = 30;
    player = box(0, 4, 0, 2, 2, 2, 0);
    if (box_contact(&player, &wall, normal, &penetration)) return 8;
    player.x = -5; player.y = -1;
    if (!box_contact(&player, &wall, normal, &penetration)) return 9;
    wall = box(0, 0, 0, 12, 2, 12, 0);
    wall.rotation_x = 30;
    player = box(0, -2, 5, 2, 2, 2, 0);
    if (!box_contact(&player, &wall, normal, &penetration)) return 10;
    wall = box(0, 0, 0, 24, 6, 5, 0);
    wall.rotation = 45;
    player = box(-7, 0, 7, 4, 4, 4, 0);
    if (!box_contact(&player, &wall, normal, &penetration)) return 11;
    return 0;
}
