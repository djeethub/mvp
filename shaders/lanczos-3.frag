#version 450

// Input video texture and texture coordinate
layout(binding = 0) uniform sampler2D videoTexture;
layout(location = 0) in vec2 v_texCoord; // Normalized UV [0.0, 1.0]

layout(location = 0) out vec4 fragColor;

const float PI = 3.14159265358979323846;

// 1D Lanczos kernel with filter radius a = 3
float lanczos3(float x) {
    x = abs(x);
    if (x < 1e-5) {
        return 1.0;
    }
    if (x >= 3.0) {
        return 0.0;
    }
    float piX = PI * x;
    return (3.0 * sin(piX) * sin(piX / 3.0)) / (piX * piX);
}

// 2D Lanczos-3 Texture Sampler (36 Taps)
vec4 textureLanczos3(sampler2D tex, vec2 uv) {
    vec2 texSize = textureSize(tex, 0);
    vec2 invTexSize = 1.0 / texSize;

    // Convert UV coordinate to fractional pixel location
    vec2 pixelPos = uv * texSize - 0.5;
    vec2 icenter  = floor(pixelPos);
    vec2 f        = pixelPos - icenter;

    vec4 colorSum  = vec4(0.0);
    float weightSum = 0.0;

    // Iterate across a 6x6 pixel grid centered on the sample point (-2 to +3)
    for (int y = -2; y <= 3; ++y) {
        float wy = lanczos3(float(y) - f.y);

        for (int x = -2; x <= 3; ++x) {
            float wx = lanczos3(float(x) - f.x);
            float weight = wx * wy;

            // Compute exact texel position
            vec2 sampleUV = (icenter + vec2(float(x), float(y)) + 0.5) * invTexSize;

            colorSum += texture(tex, sampleUV) * weight;
            weightSum += weight;
        }
    }

    // Normalize result by total weight sum to prevent brightness shifts
    return colorSum / weightSum;
}

void main() {
    fragColor = textureLanczos3(videoTexture, v_texCoord);
}