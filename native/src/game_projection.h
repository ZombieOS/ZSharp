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

#endif
