#include "xr_ui_world.h"
#include <cstdio>
#include <cstring>
#include <limits>

#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} } while(0)
static ZSharpGameWorldTriangle wall(float z) {
    ZSharpGameWorldTriangle t{};
    const float points[3][3]={{-2,-2,z},{2,-2,z},{0,2,z}};
    memcpy(t.points,points,sizeof(points));t.color[3]=1;return t;
}
static void button_face(std::vector<ZSharpGameWorldTriangle> &world,
    const ZSharpGameRenderObject &button,float normal_offset,size_t owner) {
    float corners[4][3];
    if(!zsharp_xr_ui_button_corners(&button,corners))return;
    const float radians=button.rotation_y*3.14159265358979323846f/180.f;
    const float normal[3]={std::sin(radians),0,std::cos(radians)};
    const int indices[2][3]={{0,1,2},{0,2,3}};
    for(const auto &index:indices) {
        ZSharpGameWorldTriangle t{};t.color[3]=1;t.ui_button_id=owner;
        for(int corner=0;corner<3;corner++)for(int axis=0;axis<3;axis++)
            t.points[corner][axis]=corners[index[corner]][axis]+normal[axis]*normal_offset;
        world.push_back(t);
    }
}
int main() {
    ZSharpXRUIRay ray{{0,0,0},{0,0,-1},10};
    ZSharpGameRenderObject button{};button.shape=ZGAME_SHAPE_BUTTON;
    button.render_id=7;button.z=-2;button.width=button.height=1;
    button.scale_x=button.scale_y=1;button.opacity=1;button.visible=1;
    ZSharpGameRenderFrame frame{};frame.objects=&button;frame.object_count=1;
    std::vector<ZSharpGameWorldTriangle> world;float endpoint=0;
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7&&endpoint==2);
    world={wall(-1)};CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0&&endpoint==1);
    world={wall(-3)};CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7&&endpoint==2);
    world={wall(-2)};CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7);
    // A distinct surface blocks even within the old 1 mm blanket tolerance.
    world={wall(-1.9995f)};CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    // Owning metadata, not proximity, distinguishes the panel and its glyphs.
    world={wall(-1.9995f)};world[0].ui_button_id=7;
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7&&endpoint<2);
    world[0].ui_button_id=8;
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    // Match the renderer's 0.5 mm normal glyph offset at a 75 degree angle.
    // It becomes almost 2 mm along the ray: the previous tolerance rejected it.
    button.rotation_y=75;world.clear();
    button_face(world,button,0,button.render_id);
    button_face(world,button,.0005f,button.render_id);
    const float glyph_distance=XRUIWorld::nearest_surface(ray,world);
    CHECK(glyph_distance+.001f<2);
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7);
    CHECK(std::abs(endpoint-glyph_distance)<.00001f);
    // Identical geometry from another button, or standalone text, must block.
    for(auto &triangle:world)triangle.ui_button_id=8;
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    for(auto &triangle:world)triangle.ui_button_id=0;
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    // An external surface may even sit behind the button's own raised glyphs.
    // The visible endpoint remains the glyph while the actual wall blocks.
    for(auto &triangle:world)triangle.ui_button_id=7;
    world.push_back(wall(-1.9995f));
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    CHECK(std::abs(endpoint-glyph_distance)<.00001f);
    button.rotation_y=0;
    // Hover shrinking the rendered panel does not shrink its hit target.
    button.model_index=1;button.ui_base_scale_x=button.ui_base_scale_y=1;
    button.scale_x=button.scale_y=.1f;ray.origin[0]=.3f;
    world.clear();CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7);
    button.ui_base_scale_x=0;CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    button.model_index=0;button.scale_x=button.scale_y=1;ray.origin[0]=0;
    // Alpha cutouts, including the font atlas, are not invisible click blockers.
    const unsigned char pixels[]={255,255,255,0};
    world={wall(-1)};world[0].pixels=pixels;world[0].texture_width=world[0].texture_height=1;
    world[0].texture_pitch=4;CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7);
    const unsigned char opaque[]={255,255,255,255};world[0].pixels=opaque;
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    world[0].color[3]=.1f;CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7);
    world[0].color[3]=.75f;CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    world[0].points[0][0]=std::numeric_limits<float>::quiet_NaN();
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==7);
    // Backfaces block too, consistent with the stereo renderer's CullNone.
    world={wall(-1)};for(int axis=0;axis<3;axis++)std::swap(world[0].points[0][axis],world[0].points[1][axis]);
    CHECK(XRUIWorld::pick(ray,frame,world,endpoint)==0);
    // Laser meshes are finite, world-space, opaque, and end in front of the panel.
    world.clear();XRUIWorld::add_pointer(world,ray,2,0,true);CHECK(world.size()==6);
    for(const auto &t:world)for(const auto &p:t.points)for(float coordinate:p)CHECK(std::isfinite(coordinate));
    CHECK(world.back().points[0][2]>-2);
    ray.direction[1]=1;ray.direction[2]=0;world.clear();
    XRUIWorld::add_pointer(world,ray,2,1,false);CHECK(world.size()==6);
    for(const auto &t:world)for(const auto &p:t.points)for(float coordinate:p)CHECK(std::isfinite(coordinate));
    std::puts("PASS: world-space XR UI owner-aware/grazing-angle depth/alpha occlusion and pointer geometry");
    return 0;
}
