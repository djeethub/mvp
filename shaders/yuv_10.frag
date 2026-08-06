#version 450

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex_y;
layout(set = 2, binding = 1) uniform sampler2D u_tex_u;
layout(set = 2, binding = 2) uniform sampler2D u_tex_v;

// Uniforms
layout(std140, set = 3, binding = 0) uniform Uniforms {
    vec2  tex_size;        // full resolution (Y plane)
    int   color_range;   // 2 = jpeg, 1 = mpeg
    int   colorspace;    // 2 = BT.601, 1 = BT.709, 9,10 = BT.2020
} uf;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 to_rgb(vec3 yuv, int colorRange, int colorspace) {
    // 2. Adjust for Range Bias (AVColorRange)
    // AVCOL_RANGE_JPEG = 2 (Full Range [0, 255])
    // AVCOL_RANGE_MPEG = 1 (Limited/Broadcast Range [16, 235])
    if (colorRange == 1) { 
        yuv -= vec3(64.0 / 1023.0, 512.0 / 1023.0, 512.0 / 1023.0);
        yuv *= vec3(1023.0 / (940.0 - 64.0), 1023.0 / (960.0 - 64.0), 1023.0 / (960.0 - 64.0));
    } else {
        // Full range still requires shifting Chroma components to center zero
        yuv -= vec3(0.0, 512.0 / 1023.0, 512.0 / 1023.0);
    }

    vec3 rgb;

    // 3. Apply the dynamic transformation matrix based on AVColorSpace
    // Values match FFmpeg's enum definition entries natively
    if (colorspace == 1) { 
        // AVCOL_SPC_BT709 (HD Video)
        mat3 transform709 = transpose(mat3(
            1.0,  0.0,      1.5748,
            1.0, -0.1873,  -0.4681,
            1.0,  1.8556,   0.0
        ));
        rgb = transform709 * yuv;
    } 
    else if (colorspace == 9) { 
        // AVCOL_SPC_BT2020_NCL (4K / HDR Non-constant Luminance)
        mat3 transform2020 = transpose(mat3(
            1.0,  0.0,      1.4746,
            1.0, -0.1646,  -0.5714,
            1.0,  1.8814,   0.0
        ));
        rgb = transform2020 * yuv;
    } 
    else { 
        // Default / AVCOL_SPC_BT470BG or AVCOL_SPC_SMPTE170M (BT.601 SD Video)
        mat3 transform601 = transpose(mat3(
            1.0,  0.0,      1.402,
            1.0, -0.34414, -0.71414,
            1.0,  1.772,    0.0
        ));
        rgb = transform601 * yuv;
    }

    return rgb;
}

void main() {
    vec3 yuv;
    // Calculate how many source texels fit inside ONE screen pixel
    vec2 duv_dx = dFdx(v_uv);
    vec2 duv_dy = dFdy(v_uv);
    vec2 texelsPerPixel = vec2(length(duv_dx), length(duv_dy)) * uf.tex_size;

    // --- DOWNSCALING PATH (Large image shrunk into small window) ---
    if (texelsPerPixel.x > 1.2 || texelsPerPixel.y > 1.2) {
        // Expand kernel radius based on downscaling factor to aggregate all pixels
        vec2 radius = max(texelsPerPixel * 0.5, vec2(1.0));
        float totalWeight = 0.0;
        yuv = vec3(0.0);
        // Sample across the footprint covering this destination pixel
        for (float y = -radius.y; y <= radius.y; y += 1.0) {
            for (float x = -radius.x; x <= radius.x; x += 1.0) {
                vec2 sampleUV = v_uv + (vec2(x, y) / radius) * (duv_dx + duv_dy) * 0.5;
                
                // Gaussian-like weighting towards pixel center
                float w = exp(-2.0 * (x*x + y*y) / (radius.x * radius.x + radius.y * radius.y + 1e-5));
                yuv += vec3(texture(u_tex_y, sampleUV).r, texture(u_tex_u, sampleUV).r, texture(u_tex_v, sampleUV).r) * (65535.0 / 1023.0) * w;
                totalWeight += w;
            }
        }
        yuv /= max(totalWeight, 1e-5);
    } else {
        yuv = vec3(texture(u_tex_y, v_uv).r, texture(u_tex_u, v_uv).r, texture(u_tex_v, v_uv).r) * (65535.0 / 1023.0);
    }

    vec3 rgb = to_rgb(yuv, uf.color_range, uf.colorspace);
    o_color = vec4(rgb, 1.0);
}
