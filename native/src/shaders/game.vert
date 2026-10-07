#version 450
// Regenerate embedded headers with glslang -V --vn zsharp_game_vertex_spirv.
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 uv;
layout(location=0) out vec4 tint;
layout(location=1) out vec2 texcoord;
void main() { gl_Position=position; tint=color; texcoord=uv; }
