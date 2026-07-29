#version 450

layout(set = 2, binding = 0) uniform sampler2D videoTexture;

layout(location = 0) in vec4 v_color;    // SDL Vertex Color
layout(location = 1) in vec2 v_texCoord; // SDL Texture Coordinate

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
    vec2 duv_dx = dFdx(v_texCoord);
    vec2 duv_dy = dFdy(v_texCoord);
    vec2 invTexSize = vec2(length(duv_dx), length(duv_dy));

    if (invTexSize.x < 1e-7 || invTexSize.y < 1e-7) {
        fragColor = texture(videoTexture, v_texCoord);
        return;
    }

    vec2 texSize = 1.0 / invTexSize;
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
            
            // Raw standard RGBA sampling - accurate colors guaranteed!
            colorSum += texture(videoTexture, sampleUV) * weight;
            weightSum += weight;
        }
    }

    fragColor = colorSum / max(weightSum, 1e-5);
}