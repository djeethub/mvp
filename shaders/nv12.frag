#version 450

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex_y;
layout(set = 2, binding = 1) uniform sampler2D u_tex_u;

// Uniforms
layout(set = 3, binding = 0) uniform Uniforms {
    vec2  tex_size;        // full resolution (Y plane)
    int   is_full_range;   // 1 = full range, 0 = limited
    int   matrix_id;       // 0 = BT.601, 1 = BT.709, 2 = BT.2020
} uf;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 yuv_to_rgb(float y, float u, float v) {
    u -= 0.5;
    v -= 0.5;

    // Color matrix
    vec3 rgb = vec3(
        y + 1.596 * v,
        y - 0.813 * v - 0.391 * u,
        y + 2.018 * u
    );

    return clamp(rgb, 0.0, 1.0);
}

void main() {
    float y = texture(u_tex_y, v_uv).r;
    vec2 uv_sampled = texture(u_tex_u, v_uv).rg; 
    float u = uv_sampled.r;
    float v = uv_sampled.g;

    vec3 rgb = yuv_to_rgb(y, u, v);
    o_color = vec4(rgb.r, rgb.g, rgb.b, 1.0);
}
