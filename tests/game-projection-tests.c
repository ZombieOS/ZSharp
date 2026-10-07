#include "game_projection.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int finite_edges(float edges[12][4], size_t count) {
    size_t edge, value;
    for (edge = 0; edge < count; edge++)
        for (value = 0; value < 4; value++)
            if (!isfinite(edges[edge][value])) return 0;
    return 1;
}

static int valid_face_uvs(const ZSharpProjectedCubeFace faces[6],
                          size_t count) {
    size_t face, vertex, axis;
    for (face = 0; face < count; face++)
        for (vertex = 0; vertex < faces[face].point_count; vertex++)
            for (axis = 0; axis < 2; axis++) {
                float uv = faces[face].texcoords[vertex][axis];
                if (!isfinite(uv) || uv < -0.001f || uv > 1.001f) return 0;
                if (!isfinite(faces[face].point_depth[vertex]) ||
                    faces[face].point_depth[vertex] <= 0.0f) return 0;
            }
    return 1;
}

int main(void) {
    ZSharpGameRenderFrame frame;
    ZSharpGameRenderObject cube;
    float edges[12][4];
    ZSharpProjectedCubeFace faces[6];
    size_t count;
    memset(&frame, 0, sizeof(frame));
    memset(&cube, 0, sizeof(cube));
    frame.camera_z = 8.0f;
    frame.camera_fov = 70.0f;
    cube.shape = ZGAME_SHAPE_CUBE;
    cube.width = cube.height = cube.depth = 1.0f;
    cube.scale_x = cube.scale_y = cube.scale_z = 1.0f;
    count = zsharp_game_project_cube(&frame, &cube, edges);
    if (count != 12 || !finite_edges(edges, count)) {
        fprintf(stderr, "default documented cube produced %zu edges\n", count);
        return 1;
    }
    count = zsharp_game_project_cube_faces(&frame, &cube, faces);
    if (count != 6 || !valid_face_uvs(faces, count)) {
        fprintf(stderr, "default documented cube produced %zu solid faces\n",
                count);
        return 1;
    }
    cube.depth = 120.0f;
    if (zsharp_game_camera_far_depth(&frame, &cube) <=
        zsharp_game_camera_depth(&frame, &cube) + 50.0f) {
        fprintf(stderr, "long cube lost its far-depth ordering extent\n");
        return 1;
    }
    cube.depth = 1.0f;

    /* A large cube intersects the near plane. It must be clipped instead of
       causing the entire object to disappear. */
    cube.width = cube.height = cube.depth = 64.0f;
    count = zsharp_game_project_cube(&frame, &cube, edges);
    if (count == 0 || !finite_edges(edges, count)) {
        fprintf(stderr, "near-plane cube was discarded\n");
        return 1;
    }
    count = zsharp_game_project_cube_faces(&frame, &cube, faces);
    if (count == 0 || !valid_face_uvs(faces, count)) {
        fprintf(stderr, "near-plane cube produced no solid faces\n");
        return 1;
    }

    cube.width = cube.height = cube.depth = 1.0f;
    cube.x = 2.0f;
    cube.y = -1.0f;
    cube.z = 3.0f;
    cube.rotation = 35.0f;
    frame.camera_x = 1.0f;
    frame.camera_y = -0.5f;
    count = zsharp_game_project_cube(&frame, &cube, edges);
    if (count != 12 || !finite_edges(edges, count)) {
        fprintf(stderr, "transformed cube projection failed\n");
        return 1;
    }

    cube.rotation_x = 20.0f;
    cube.rotation_y = 15.0f;
    cube.rotation_z = 10.0f;
    frame.camera_rotation_x = -5.0f;
    frame.camera_rotation_y = 12.0f;
    frame.camera_rotation_z = 3.0f;
    count = zsharp_game_project_cube_faces(&frame, &cube, faces);
    if (count == 0) {
        fprintf(stderr, "Euler-rotated cube or camera produced no faces\n");
        return 1;
    }
    {
        size_t face;
        for (face = 0; face < count; face++) {
            float normal_length = sqrtf(
                faces[face].world_normal[0] * faces[face].world_normal[0] +
                faces[face].world_normal[1] * faces[face].world_normal[1] +
                faces[face].world_normal[2] * faces[face].world_normal[2]);
            if (!isfinite(normal_length) || fabsf(normal_length - 1.0f) > 0.001f ||
                !isfinite(faces[face].world_center[0]) ||
                !isfinite(faces[face].world_center[1]) ||
                !isfinite(faces[face].world_center[2])) {
                fprintf(stderr, "3D light face geometry was invalid\n");
                return 1;
            }
        }
    }

    /* A +90 degree yaw looks toward world -X. */
    memset(&cube, 0, sizeof(cube));
    cube.shape = ZGAME_SHAPE_CUBE;
    cube.width = cube.height = cube.depth = 1.0f;
    cube.scale_x = cube.scale_y = cube.scale_z = 1.0f;
    cube.x = -5.0f;
    memset(&frame, 0, sizeof(frame));
    frame.camera_fov = 70.0f;
    frame.camera_rotation_y = 90.0f;
    count = zsharp_game_project_cube(&frame, &cube, edges);
    if (count != 12 || !finite_edges(edges, count)) {
        fprintf(stderr, "yawed camera did not see cube on its forward axis\n");
        return 1;
    }

    /* Combined pitch and backwards-facing yaw must remain independent. This
       point is directly along the camera's local forward axis for yaw 180 and
       pitch 30, so its tiny cube should remain centered with zero roll. */
    cube.width = cube.height = cube.depth = 0.01f;
    cube.x = 0.0f;
    cube.y = 5.0f;
    cube.z = 8.660254f;
    frame.camera_rotation_x = 30.0f;
    frame.camera_rotation_y = 180.0f;
    frame.camera_rotation_z = 0.0f;
    count = zsharp_game_project_cube(&frame, &cube, edges);
    if (count != 12 || !finite_edges(edges, count)) {
        fprintf(stderr, "backwards-facing pitched camera lost its forward object\n");
        return 1;
    }
    {
        float minimum_x = edges[0][0], maximum_x = edges[0][0];
        float minimum_y = edges[0][1], maximum_y = edges[0][1];
        size_t edge;
        for (edge = 0; edge < count; edge++) {
            size_t endpoint;
            for (endpoint = 0; endpoint < 2; endpoint++) {
                float x = edges[edge][endpoint * 2];
                float y = edges[edge][endpoint * 2 + 1];
                if (x < minimum_x) minimum_x = x;
                if (x > maximum_x) maximum_x = x;
                if (y < minimum_y) minimum_y = y;
                if (y > maximum_y) maximum_y = y;
            }
        }
        if (fabsf((minimum_x + maximum_x) * 0.5f - 640.0f) > 1.0f ||
            fabsf((minimum_y + maximum_y) * 0.5f - 360.0f) > 1.0f) {
            fprintf(stderr, "yaw 180 introduced pitch/roll drift\n");
            return 1;
        }
    }
    {
        ZSharpGameRenderFrame view = {0};
        ZSharpGameRenderObject cube = {0};
        view.camera_fov = 70;
        cube.width = cube.height = cube.depth = 2;
        cube.scale_x = cube.scale_y = cube.scale_z = 1;
        cube.z = -10;
        if (!zsharp_game_cube_in_view(&view, &cube)) return 20;
        cube.z = 10;
        if (zsharp_game_cube_in_view(&view, &cube)) return 21;
        cube.z = -10; cube.x = 100;
        if (zsharp_game_cube_in_view(&view, &cube)) return 22;
        cube.width = 300; /* Large floor crossing the view must survive. */
        if (!zsharp_game_cube_in_view(&view, &cube)) return 23;
        view.camera_rotation_y = 180; cube.width = 2; cube.x = 0; cube.z = 10;
        if (!zsharp_game_cube_in_view(&view, &cube)) return 24;
    }
    {
        ZSharpGameRenderFrame direct={0},prepared;
        ZSharpGameRenderObject cube={0};
        float a[12][4],b[12][4];
        int yaw;
        cube.width=3;cube.height=4;cube.depth=2;
        cube.scale_x=cube.scale_y=cube.scale_z=1;cube.z=-20;
        cube.rotation_x=17;cube.rotation_y=21;cube.rotation_z=5;
        direct.camera_fov=83;direct.camera_rotation_x=13;direct.camera_rotation_z=7;
        for(yaw=-360;yaw<=360;yaw+=15) {
            size_t na,nb,i,j;
            direct.camera_rotation_y=(float)yaw;
            prepared=direct;zsharp_game_prepare_projection(&prepared);
            na=zsharp_game_project_cube(&direct,&cube,a);
            nb=zsharp_game_project_cube(&prepared,&cube,b);
            if(na!=nb)return 25;
            for(i=0;i<na;i++)for(j=0;j<4;j++)
                if(fabsf(a[i][j]-b[i][j])> .01f + fabsf(a[i][j])*.0001f)return 26;
            if(zsharp_game_cube_in_view(&direct,&cube)!=zsharp_game_cube_in_view(&prepared,&cube))return 27;
        }
    }
    return 0;
}
