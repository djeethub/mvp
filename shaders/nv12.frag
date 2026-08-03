#version 450

#include "utils.glsl"

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex_y;
layout(set = 2, binding = 1) uniform sampler2D u_tex_u;

// Uniforms
layout(std140, set = 3, binding = 0) uniform Uniforms {
    vec2  tex_size;        // full resolution (Y plane)
    int   color_range;   // 2 = jpeg, 1 = mpeg
    int   colorspace;    // 2 = BT.601, 1 = BT.709, 9,10 = BT.2020
} uf;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    float y = texture(u_tex_y, v_uv).r;
    vec2 uv_sampled = texture(u_tex_u, v_uv).rg; 
    float u = uv_sampled.r;
    float v = uv_sampled.g;

    vec3 rgb = to_rgb(y, u, v, uf.color_range, uf.colorspace);
    o_color = vec4(rgb.r, rgb.g, rgb.b, 1.0);
}
