#version 450

layout(set = 2, binding = 0) uniform sampler2D videoTexture;

// SDL3 custom fragment uniforms are bound to Set 3, Binding 0 (Slot 0)
layout(set = 3, binding = 0) uniform TextureUniforms {
    vec2 u_texSize;
} uniforms;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_texCoord;

layout(location = 0) out vec4 fragColor;

const float PI = 3.14159265358979323846;

float lanczos3(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    float piX = PI * x;
    return (3.0 * sin(piX) * sin(piX / 3.0)) / (piX * piX);
}

void main() {
    vec2 texSize = uniforms.u_texSize;
    vec2 invTexSize = 1.0 / max(texSize, vec2(1.0));

    // Calculate how many source texels fit inside ONE screen pixel
    vec2 duv_dx = dFdx(v_texCoord);
    vec2 duv_dy = dFdy(v_texCoord);
    vec2 texelsPerPixel = vec2(length(duv_dx), length(duv_dy)) * texSize;

    // --- DOWNSCALING PATH (Large image shrunk into small window) ---
    if (texelsPerPixel.x > 1.2 || texelsPerPixel.y > 1.2) {
        // Expand kernel radius based on downscaling factor to aggregate all pixels
        vec2 radius = max(texelsPerPixel * 0.5, vec2(1.0));
        vec4 colorSum = vec4(0.0);
        float totalWeight = 0.0;

        // Sample across the footprint covering this destination pixel
        for (float y = -radius.y; y <= radius.y; y += 1.0) {
            for (float x = -radius.x; x <= radius.x; x += 1.0) {
                vec2 sampleUV = v_texCoord + (vec2(x, y) / radius) * (duv_dx + duv_dy) * 0.5;
                
                // Gaussian-like weighting towards pixel center
                float w = exp(-2.0 * (x*x + y*y) / (radius.x * radius.x + radius.y * radius.y + 1e-5));
                colorSum += texture(videoTexture, sampleUV) * w;
                totalWeight += w;
            }
        }

        fragColor = colorSum / max(totalWeight, 1e-5);
    } 
    // --- UPSCALING / NEAR 1:1 PATH (Lanczos-3) ---
    else {
        vec2 pixelPos = v_texCoord * texSize - 0.5;
        vec2 icenter  = floor(pixelPos);
        vec2 f        = pixelPos - icenter;

        vec4 colorSum   = vec4(0.0);
        float weightSum = 0.0;

        for (int y = -2; y <= 3; ++y) {
            float wy = lanczos3(float(y) - f.y);
            for (int x = -2; x <= 3; ++x) {
                float wx = lanczos3(float(x) - f.x);
                float weight = wx * wy;

                vec2 sampleUV = (icenter + vec2(float(x), float(y)) + 0.5) * invTexSize;
                colorSum += texture(videoTexture, sampleUV) * weight;
                weightSum += weight;
            }
        }

        fragColor = colorSum / max(weightSum, 1e-5);
    }
}