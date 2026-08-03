#version 450

// Push uniform block (Slot 0)
layout(std140, set = 1, binding = 0) uniform TransformBlock {
    vec2 u_position; // Center or Top-Left position in Normalized Device Coordinates (-1..1)
    vec2 u_size;     // Width and Height in NDC space
} transform;

layout(location = 0) out vec2 outUV;

const vec2 unit_quad[4] = vec2[](
    vec2(-0.5, -0.5), // Bottom-Left
    vec2( 0.5, -0.5), // Bottom-Right
    vec2(-0.5,  0.5), // Top-Left
    vec2( 0.5,  0.5)  // Top-Right
);

const vec2 uvs[4] = vec2[](
    vec2(0.0, 1.0),
    vec2(1.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 0.0)
);

void main() {
    // Scale the unit quad and offset by the position
    vec2 finalPosition = (unit_quad[gl_VertexIndex] * transform.u_size) + transform.u_position;
    
    gl_Position = vec4(finalPosition, 0.0, 1.0);
    outUV = uvs[gl_VertexIndex];
}