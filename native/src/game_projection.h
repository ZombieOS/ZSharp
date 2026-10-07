#ifndef ZSHARP_GAME_PROJECTION_H
#define ZSHARP_GAME_PROJECTION_H

#include "game_vulkan.h"
void zsharp_game_prepare_projection(ZSharpGameRenderFrame *frame);

#include <stddef.h>

/* Produces screen-space line segments for the cube's visible/clipped edges.
   Each result is {x1, y1, x2, y2}. */
size_t zsharp_game_project_cube(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object,
    float edges[12][4]);

typedef struct ZSharpProjectedCubeFace {
    float points[6][2];
    float texcoords[6][2];
    float point_depth[6];
    size_t point_count;
    float depth;
    float brightness;
    float world_center[3];
    float world_normal[3];
} ZSharpProjectedCubeFace;

/* Produces clipped, screen-space polygons for the cube's six solid faces.
   Faces are returned from farthest to nearest for painter-style rendering. */
size_t zsharp_game_project_cube_faces(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object,
    ZSharpProjectedCubeFace faces[6]);

/* Clip/project one world-space mesh triangle, preserving its UVs and the
   world-space face geometry used by the scene light pass. */
int zsharp_game_project_mesh_triangle(
    const ZSharpGameRenderFrame *frame,
    const float world[3][3], const float uv[3][2],
    ZSharpProjectedCubeFace *face);

/* Returns the object's center depth in the rotated camera coordinate system. */
int zsharp_game_cube_in_view(const ZSharpGameRenderFrame *frame,
                            const ZSharpGameRenderObject *object);

float zsharp_game_camera_depth(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object);

/* Backmost corner depth for painter ordering of large cubes. Sorting on the
   center lets a long floor suddenly paint over the player halfway across it. */
float zsharp_game_camera_far_depth(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object);

#endif
