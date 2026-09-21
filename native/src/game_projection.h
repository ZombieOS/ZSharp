#ifndef ZSHARP_GAME_PROJECTION_H
#define ZSHARP_GAME_PROJECTION_H

#include "game_vulkan.h"

#include <stddef.h>

/* Produces screen-space line segments for the cube's visible/clipped edges.
   Each result is {x1, y1, x2, y2}. */
size_t zsharp_game_project_cube(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object,
    float edges[12][4]);

typedef struct ZSharpProjectedCubeFace {
    float points[6][2];
    size_t point_count;
    float depth;
    float brightness;
} ZSharpProjectedCubeFace;

/* Produces clipped, screen-space polygons for the cube's six solid faces.
   Faces are returned from farthest to nearest for painter-style rendering. */
size_t zsharp_game_project_cube_faces(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object,
    ZSharpProjectedCubeFace faces[6]);

/* Returns the object's center depth in the rotated camera coordinate system. */
float zsharp_game_camera_depth(
    const ZSharpGameRenderFrame *frame,
    const ZSharpGameRenderObject *object);

#endif
