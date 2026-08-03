#version 450

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex_y;

// Uniforms
layout(std140, set = 3, binding = 0) uniform Uniforms {
    vec2  tex_size;        // full resolution (Y plane)
    int   color_range;   // 2 = jpeg, 1 = mpeg
    int   colorspace;    // 2 = BT.601, 1 = BT.709, 9,10 = BT.2020
} uf;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = texture(u_tex_y, v_uv);
}
