#version 450
// Generate opaque SPIR-V normally and blended SPIR-V with -DTRANSPARENT=1.
layout(set=2,binding=0) uniform sampler2D image;
layout(location=0) in vec4 tint;
layout(location=1) in vec2 texcoord;
layout(location=0) out vec4 outputColor;
void main() {
 outputColor=texture(image,texcoord)*tint;
#ifdef TRANSPARENT
 if(outputColor.a<=0.0 || outputColor.a>=0.999) discard;
#else
 if(outputColor.a<0.999) discard;
#endif
}
