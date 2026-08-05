#version 450

// Descriptor Set 2 is required for SDL_GPU Fragment Samplers
layout(set = 2, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main() {
    // 1. Sample glyph alpha mask (R channel from R8_UNORM texture atlas)
    float alphaMask = texture(uTexture, inUV).r;

    // 2. Combine glyph shape mask with incoming per-vertex RGBA color
    outColor = vec4(inColor.rgb, inColor.a * alphaMask);
}