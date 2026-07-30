#version 450

layout(set = 2, binding = 0) uniform sampler2D u_texture;

layout(set = 3, binding = 0) uniform Uniforms {
    vec2  texture_size;   // width, height of the source texture
    float lobes;          // 2.0 or 3.0 recommended
} u;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

#define PI 3.14159265359

// Lanczos kernel
float lanczos(float x, float a)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= a)   return 0.0;

    float pix = PI * x;
    return (a * sin(pix) * sin(pix / a)) / (pix * pix);
}

void main()
{
    vec2 tex_size = u.texture_size;
    vec2 inv_size = 1.0 / tex_size;

    // Pixel position in source texture
    vec2 src_pos = v_uv * tex_size;
    vec2 base    = floor(src_pos - 0.5) + 0.5;   // nearest texel center
    vec2 f       = src_pos - base;               // fractional offset

    float a = u.lobes;          // usually 2.0 or 3.0
    int   r = int(ceil(a));     // radius

    vec4 color = vec4(0.0);
    float total_weight = 0.0;

    for (int y = -r + 1; y <= r; ++y) {
        for (int x = -r + 1; x <= r; ++x) {
            vec2 offset = vec2(float(x), float(y));
            float wx = lanczos(offset.x - f.x, a);
            float wy = lanczos(offset.y - f.y, a);
            float w  = wx * wy;

            vec2 sample_uv = (base + offset) * inv_size;
            color += textureLod(u_texture, sample_uv, 0.0) * w;
            total_weight += w;
        }
    }

    color /= total_weight;
    o_color = color * v_color;
}