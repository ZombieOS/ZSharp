#include "window_layout.h"
#include "paint.h"
#include "style_transition.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"ZSS check failed at line %d\n",__LINE__); return 1; } } while(0)
#define NEAR(a,b) (fabs((a)-(b)) < 0.001)
int main(void) {
    uint32_t rgb; unsigned char alpha;
    ZSharpUIProperty root[] = {{0}}, a[] = {{0}}, b[] = {{0}};
    ZSharpUIElement elements[3]; ZSharpWindow window; ZSharpLayoutRect rects[3];
    ZSharpPaint paint;
    char error[256];
    {
        double duration, target[6]={0}, output[6];int easing;
        ZSharpStyleTween tween;memset(&tween,0,sizeof(tween));
        CHECK(zsharp_style_transition_parse("all 200ms ease",&duration,&easing) && NEAR(duration,.2));
        CHECK(!zsharp_style_transition_parse("all -1s linear",&duration,&easing));
        CHECK(!zsharp_style_transition_parse("all 1s unknown",&duration,&easing));
        CHECK(NEAR(zsharp_style_ease(.5,0),.5));
        zsharp_style_tween(&tween,target,0,1,0,output);target[0]=100;
        zsharp_style_tween(&tween,target,0,1,0,output);
        zsharp_style_tween(&tween,target,.5,1,0,output);CHECK(NEAR(output[0],50));
        target[0]=0;zsharp_style_tween(&tween,target,.5,1,0,output);CHECK(NEAR(output[0],50));
        zsharp_style_tween(&tween,target,1,1,0,output);CHECK(NEAR(output[0],25));
    }
    CHECK(zsharp_color_parse("transparent",&rgb,&alpha) && alpha == 0);
    CHECK(zsharp_color_parse("#123",&rgb,&alpha) && rgb == 0x112233 && alpha == 255);
    CHECK(zsharp_color_parse("#1238",&rgb,&alpha) && rgb == 0x112233 && alpha == 136);
    CHECK(zsharp_color_parse("#11223380",&rgb,&alpha) && rgb == 0x112233 && alpha == 128);
    CHECK(zsharp_color_parse("rgba(255, 0, 128, 0.5)",&rgb,&alpha) && rgb == 0xFF0080 && alpha == 128);
    CHECK(zsharp_color_parse("rgb(100%, 0%, 0%)",&rgb,&alpha) && rgb == 0xFF0000);
    CHECK(!zsharp_color_parse("rgba(0,0,0,2)",&rgb,&alpha));
    CHECK(!zsharp_color_parse("rgba(0,0,0,nan)",&rgb,&alpha));
    CHECK(!zsharp_color_parse("#12345",&rgb,&alpha));
    CHECK(zsharp_color_over(0xFFFFFF,0,0x123456) == 0x123456);
    CHECK(zsharp_color_over(0xFFFFFF,128,0) == 0x808080);
    memset(&paint,0,sizeof(paint));
    CHECK(zsharp_paint_parse("transparent",&paint,error,sizeof(error)));
    CHECK(zsharp_paint_alpha_sample(&paint,0) == 0); zsharp_paint_free(&paint);
    CHECK(zsharp_paint_parse("linear-gradient(90:#FFFFFF:#000000)",&paint,error,sizeof(error)));
    CHECK(zsharp_paint_alpha_sample(&paint,0.5) == 255); zsharp_paint_free(&paint);
    CHECK(NEAR(zsharp_css_length("25%",800,800,600,1,0),200));
    CHECK(NEAR(zsharp_css_length("10vw",0,800,600,1,0),80));
    CHECK(NEAR(zsharp_css_length("10vh",0,800,600,1,0),60));
    CHECK(NEAR(zsharp_css_length("2zu",0,800,600,2,0),16));
    memset(elements,0,sizeof(elements)); memset(&window,0,sizeof(window));
    memset(rects,0,sizeof(rects));
    elements[0].type=ZUI_DESIGN; elements[1].type=elements[2].type=ZUI_BUTTON;
    window.elements=elements; window.element_count=3;
    rects[1].x=17;rects[1].width=rects[2].width=100;rects[1].height=rects[2].height=32;
    CHECK(zsharp_window_layout(&window,800,600,1,rects)); CHECK(rects[1].x == 17);
    root[0].name="display"; root[0].text_value="flex"; elements[0].properties=root;elements[0].property_count=1;
    a[0].name="flexGrow";a[0].text_value="1";b[0]=a[0];
    elements[1].properties=a;elements[1].property_count=1;elements[2].properties=b;elements[2].property_count=1;
    CHECK(zsharp_window_layout(&window,800,600,1,rects));
    CHECK(NEAR(rects[1].width,400) && NEAR(rects[2].x,400));
    root[0].text_value="grid";
    elements[1].property_count=elements[2].property_count=0;
    CHECK(zsharp_window_layout(&window,800,600,1,rects));
    CHECK(NEAR(rects[1].width,800) && rects[2].y >= rects[1].height);
    a[0].name="display";a[0].text_value="none";elements[1].property_count=1;
    CHECK(zsharp_window_layout(&window,800,600,1,rects));CHECK(rects[1].hidden && NEAR(rects[2].y,0));
    a[0].name="media/0/600/0/1e12/display";a[0].text_value="none";
    CHECK(zsharp_window_layout(&window,800,600,1,rects));CHECK(!rects[1].hidden);
    CHECK(zsharp_window_layout(&window,400,600,1,rects));CHECK(rects[1].hidden);
    /* Layout edges must not replace or interpret button click callbacks. */
    {
        ZSharpUIProperty edges[4] = {{0}};
        memset(elements, 0, sizeof(elements)); memset(rects, 0, sizeof(rects));
        elements[0].type = ZUI_DESIGN;
        elements[1].type = ZUI_CONTAINER;
        elements[2].type = ZUI_BUTTON;
        rects[1].x = 16; rects[1].y = 300;
        rects[1].width = 646; rects[1].height = 88;
        rects[2].width = rects[2].height = 32;
        edges[0].name = "__parent"; edges[0].text_value = "2";
        edges[1].name = "right"; edges[1].type = ZUI_PROPERTY_CALLBACK;
        edges[2].name = "cssRight"; edges[2].text_value = "12px";
        edges[3].name = "bottom"; edges[3].text_value = "12px";
        elements[2].properties = edges; elements[2].property_count = 4;
        CHECK(zsharp_window_layout(&window, 678, 472, 1, rects));
        CHECK(NEAR(rects[2].x, 618) && NEAR(rects[2].y, 344));
        CHECK(edges[1].type == ZUI_PROPERTY_CALLBACK);
    }
    puts("ZSS color and layout checks passed"); return 0;
}
