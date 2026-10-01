#ifndef ZSHARP_GAME_INPUT_EDGES_H
#define ZSHARP_GAME_INPUT_EDGES_H
#include <stdint.h>
#include <string.h>

/* Access under the game model mutex. Each task owns its own cursor. */
typedef struct ZSharpMouseEdges {
    uint64_t count[4];
    int held[2];
} ZSharpMouseEdges;
typedef struct ZSharpMouseCursor { uint64_t seen[4]; } ZSharpMouseCursor;

static int zsharp_mouse_edge_index(const char *path) {
    static const char *names[4] = {
        "input.mouse.leftPressed", "input.mouse.leftReleased",
        "input.mouse.rightPressed", "input.mouse.rightReleased"
    };
    int i;
    if (path == NULL) return -1;
    for (i = 0; i < 4; ++i) if (strcmp(path, names[i]) == 0) return i;
    return -1;
}
static void zsharp_mouse_transition(ZSharpMouseEdges *edges, int button,
                                    int down) {
    if (edges->held[button] == down) return;
    edges->held[button] = down;
    ++edges->count[button * 2 + (down ? 0 : 1)];
}
static void zsharp_mouse_cursor_begin(ZSharpMouseCursor *cursor,
                                      const ZSharpMouseEdges *edges) {
    memcpy(cursor->seen, edges->count, sizeof(cursor->seen));
}
static int zsharp_mouse_edge_read(ZSharpMouseCursor *cursor,
                                  const ZSharpMouseEdges *edges, int edge) {
    if (cursor->seen[edge] == edges->count[edge]) return 0;
    ++cursor->seen[edge];
    return 1;
}
#endif
