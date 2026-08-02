#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// Descriptor Set 2 is required for SDL_GPU Fragment Samplers
layout(set = 2, binding = 0) uniform sampler2D texAlpha;

// Uniform Data (passed via SDL_PushGPUFragmentUniformData, slot 0)
layout(std140, set = 3, binding = 0) uniform ColorBuffer {
    vec4 textColor; // RGBA color from ASS_Image
};

void main() {
    // Sample the single-channel R8 alpha mask output by libass
    float glyphAlpha = texture(texAlpha, inUV).r;
    
    // Combine glyph alpha mask with libass text color and opacity
    outColor = vec4(textColor.rgb, textColor.a * glyphAlpha);
}