#version 450

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex_y;
layout(set = 2, binding = 1) uniform sampler2D u_pal;

// Uniforms
layout(std140, set = 3, binding = 0) uniform Uniforms {
    vec2  tex_size;        // full resolution (Y plane)
    int   color_range;   // 2 = jpeg, 1 = mpeg
    int   colorspace;    // 2 = BT.601, 1 = BT.709, 9,10 = BT.2020
} uf;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 rgba;
    // Calculate how many source texels fit inside ONE screen pixel
    vec2 duv_dx = dFdx(v_uv);
    vec2 duv_dy = dFdy(v_uv);
    vec2 texelsPerPixel = vec2(length(duv_dx), length(duv_dy)) * uf.tex_size;

    // --- DOWNSCALING PATH (Large image shrunk into small window) ---
    if (texelsPerPixel.x > 1.2 || texelsPerPixel.y > 1.2) {
        // Expand kernel radius based on downscaling factor to aggregate all pixels
        vec2 radius = max(texelsPerPixel * 0.5, vec2(1.0));
        float totalWeight = 0.0;
        rgba = vec4(0.0);
        // Sample across the footprint covering this destination pixel
        for (float y = -radius.y; y <= radius.y; y += 1.0) {
            for (float x = -radius.x; x <= radius.x; x += 1.0) {
                vec2 sampleUV = v_uv + (vec2(x, y) / radius) * (duv_dx + duv_dy) * 0.5;
                
                // Gaussian-like weighting towards pixel center
                float w = exp(-2.0 * (x*x + y*y) / (radius.x * radius.x + radius.y * radius.y + 1e-5));
                rgba += texture(u_pal, vec2(texture(u_tex_y, sampleUV).r, 0.5)) * w;
                totalWeight += w;
            }
        }
        o_color = rgba / max(totalWeight, 1e-5);
    } else {
        o_color = texture(u_pal, vec2(texture(u_tex_y, v_uv).r, 0.5));
    }
}
