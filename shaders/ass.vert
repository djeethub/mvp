#version 450

// Attributes matching your SDL_GPUVertexAttribute setup
layout(location = 0) in vec2 inPosition; // Screen space NDC position
layout(location = 1) in vec2 inUV;       // Texture coordinates (0..1)

layout(location = 0) out vec2 outUV;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    outUV = inUV;
}