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

// -------------------------------------------------
// Lanczos kernel
// -------------------------------------------------
float lanczos(float x, float a) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= a) return 0.0;
    float pix = 3.14159265359 * x;
    return (a * sin(pix) * sin(pix / a)) / (pix * pix);
}

// -------------------------------------------------
// Sample with Lanczos (works on any single texture)
// -------------------------------------------------
vec4 lanczos_sample(sampler2D tex, vec2 uv, vec2 size, float lobes) {
    vec2 pos  = uv * size;
    vec2 base = floor(pos - 0.5) + 0.5;
    vec2 f    = pos - base;
    int r     = int(ceil(lobes));

    vec4 color = vec4(0.0);
    float total = 0.0;

    for (int y = -r+1; y <= r; ++y) {
        for (int x = -r+1; x <= r; ++x) {
            float wx = lanczos(float(x) - f.x, lobes);
            float wy = lanczos(float(y) - f.y, lobes);
            float w  = wx * wy;

            vec2 sample_uv = (base + vec2(float(x), float(y))) / size;
            color += textureLod(tex, sample_uv, 0.0) * w;
            total += w;
        }
    }
    return color / total;
}

// -------------------------------------------------
// YUV → RGB conversion
// -------------------------------------------------
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

// -------------------------------------------------
// Main
// -------------------------------------------------
void main() {
    if (ubo.is_rgb == 1) {
        // Already RGB – just do high-quality scaling
        o_color = lanczos_sample(u_tex_y, v_uv, ubo.tex_size, ubo.lobes) * v_color;
        return;
    }

    // ----- YUV path -----
    // Sample Y at full resolution
    float y = lanczos_sample(u_tex_y, v_uv, ubo.tex_size, ubo.lobes).r;

    // Sample UV at half resolution (for 4:2:0)
    vec2 uv_size = ubo.tex_size * 0.5;
    vec2 uv_coord = v_uv;                     // or apply chroma siting offset
    vec4 uv_sample = lanczos_sample(u_tex_uv, uv_coord, uv_size, ubo.lobes);

    float u_val = uv_sample.r;
    float v_val = uv_sample.g;

    vec3 rgb = yuv_to_rgb(y, u_val, v_val);
    o_color = vec4(rgb, 1.0) * v_color;
}