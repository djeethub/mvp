#version 450

// Vertex Attributes mapped from SDL_GPU Pipeline Vertex Input State
layout(location = 0) in vec2 inPosition; // Screen space/NDC coordinates
layout(location = 1) in vec2 inUV;       // Atlas UV coordinates (u, v)
layout(location = 2) in vec4 inColor;    // Per-glyph color (r, g, b, a)

// Outputs to Fragment Shader
layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    outUV = inUV;
    outColor = inColor;
}