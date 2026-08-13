#version 450

layout(std140, set = 1, binding = 0) uniform TransformBlock {
    vec2 position;
    vec2 size;
    vec2 uv_size;
} transform;

// Outputs to Fragment Shader
layout(location = 0) out vec2 outUV;

const vec2 unit_quad[4] = vec2[](
    vec2(0.0, -1.0), // Bottom-Left
    vec2(1.0, -1.0), // Bottom-Right
    vec2(0.0,  0.0), // Top-Left
    vec2(1.0,  0.0)  // Top-Right
);

const vec2 uvs[4] = vec2[](
    vec2(0.0, 1.0),
    vec2(1.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 0.0)
);

void main() {
    vec2 finalPosition = (unit_quad[gl_VertexIndex] * transform.size) + transform.position;
    
    gl_Position = vec4(finalPosition, 0.0, 1.0);
    outUV = uvs[gl_VertexIndex] * transform.uv_size;
}
