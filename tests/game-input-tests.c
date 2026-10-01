#include "game_input_edges.h"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    ZSharpMouseEdges edges = {0};
    ZSharpMouseCursor a = {0}, b = {0}, late = {0};
    int i;
    zsharp_mouse_cursor_begin(&a, &edges);
    zsharp_mouse_cursor_begin(&b, &edges);
    /* Both transitions occur before either script gets CPU time. */
    zsharp_mouse_transition(&edges, 0, 1);
    zsharp_mouse_transition(&edges, 0, 0);
    CHECK(!edges.held[0]);
    CHECK(zsharp_mouse_edge_read(&a, &edges, 0));
    CHECK(!zsharp_mouse_edge_read(&a, &edges, 0));
    CHECK(zsharp_mouse_edge_read(&a, &edges, 1));
    CHECK(!zsharp_mouse_edge_read(&a, &edges, 1));
    /* First task cannot steal events from the second. */
    CHECK(zsharp_mouse_edge_read(&b, &edges, 0));
    CHECK(zsharp_mouse_edge_read(&b, &edges, 1));
    for (i = 0; i < 10000; ++i) {
        zsharp_mouse_transition(&edges, 1, 1);
        zsharp_mouse_transition(&edges, 1, 1); /* duplicate down */
        zsharp_mouse_transition(&edges, 1, 0);
    }
    CHECK(edges.count[2] == 10000 && edges.count[3] == 10000);
    for (i = 0; i < 10000; ++i) {
        CHECK(zsharp_mouse_edge_read(&a, &edges, 2));
        CHECK(zsharp_mouse_edge_read(&a, &edges, 3));
    }
    CHECK(!zsharp_mouse_edge_read(&a, &edges, 2));
    CHECK(!zsharp_mouse_edge_read(&a, &edges, 3));
    CHECK(zsharp_mouse_edge_read(&b, &edges, 2));
    zsharp_mouse_cursor_begin(&late, &edges);
    CHECK(!zsharp_mouse_edge_read(&late, &edges, 2));
    zsharp_mouse_transition(&edges, 1, 1);
    CHECK(edges.held[1]);
    CHECK(zsharp_mouse_edge_read(&late, &edges, 2));
    zsharp_mouse_transition(&edges, 1, 0); /* focus-loss release */
    CHECK(zsharp_mouse_edge_read(&late, &edges, 3));
    CHECK(zsharp_mouse_edge_index("input.mouse.leftPressed") == 0);
    CHECK(zsharp_mouse_edge_index("input.mouse.rightReleased") == 3);
    CHECK(zsharp_mouse_edge_index("input.mouse.left") == -1);
    return 0;
}
