#version 450

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex_y;   // Y or RGB
layout(set = 2, binding = 1) uniform sampler2D u_tex_uv;  // UV (optional)

// Uniforms
layout(set = 3, binding = 0) uniform Uniforms {
    vec2  tex_size;        // full resolution (Y plane)
    float lobes;           // 2.0 or 3.0
    float bit_depth;       // 8.0 or 10.0 (or 12.0)
    int   is_rgb;          // 1 = already RGB, 0 = YUV
    int   is_full_range;   // 1 = full range, 0 = limited
    int   matrix_id;       // 0 = BT.601, 1 = BT.709, 2 = BT.2020
    int   chroma_offset;   // 0 = co-sited, 1 = centered (optional)
} ubo;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 yuv_to_rgb(float y, float u, float v) {
    // Scale according to bit depth
    float max_val = exp2(ubo.bit_depth) - 1.0;
    y /= max_val;
    u /= max_val;
    v /= max_val;

    // Limited range → full range
    if (ubo.is_full_range == 0) {
        y = (y - 16.0/255.0) / (235.0/255.0 - 16.0/255.0);
        u = (u - 16.0/255.0) / (240.0/255.0 - 16.0/255.0);
        v = (v - 16.0/255.0) / (240.0/255.0 - 16.0/255.0);
    }

    u -= 0.5;
    v -= 0.5;

    // Color matrix
    vec3 rgb;
    if (ubo.matrix_id == 0) {          // BT.601
        rgb = vec3(
            y + 1.402 * v,
            y - 0.344 * u - 0.714 * v,
            y + 1.772 * u
        );
    } else if (ubo.matrix_id == 1) {   // BT.709
        rgb = vec3(
            y + 1.5748 * v,
            y - 0.1873 * u - 0.4681 * v,
            y + 1.8556 * u
        );
    } else {                         // BT.2020
        rgb = vec3(
            y + 1.4746 * v,
            y - 0.1646 * u - 0.5714 * v,
            y + 1.8814 * u
        );
    }

    return clamp(rgb, 0.0, 1.0);
}

void main() {
    vec4 r = textureLod(u_tex_y, v_uv, 1.0);
    vec4 rg = textureLod(u_tex_uv, v_uv, 1.0);
//    vec3 rgb = yuv_to_rgb(r.r, rg.r, rg.g);
    o_color = vec4(r.r, rg.r, rg.g, 1.0);
}