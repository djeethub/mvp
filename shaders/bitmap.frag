#version 450

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex_y;
layout(set = 2, binding = 1) uniform sampler2D u_pal;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = texture(u_pal, vec2(texture(u_tex_y, v_uv).r, 0.5)).bgra;
}
