#version 450

struct SpriteInstance {
    vec2 position;
    vec2 size;
    vec2 uv;
    vec2 uv_size;
    vec4 color;
};

layout(std140, set = 0, binding = 0) readonly buffer SpriteBuffer {
    SpriteInstance sprites[];
};

// Outputs to Fragment Shader
layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

const vec2 unit_quad[6] = vec2[](
    vec2(0.0, -1.0), // Bottom-Left
    vec2(1.0, -1.0), // Bottom-Right
    vec2(0.0,  0.0), // Top-Left

    vec2(0.0,  0.0), // Top-Left
    vec2(1.0, -1.0), // Bottom-Right
    vec2(1.0,  0.0)  // Top-Right
);

const vec2 uvs[6] = vec2[](
    vec2(0.0, 1.0),
    vec2(1.0, 1.0),
    vec2(0.0, 0.0),

    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(1.0, 0.0)
);

void main() {
    uint sprite_id = gl_VertexIndex / 6;
    uint vertex_id = gl_VertexIndex % 6;
    SpriteInstance sprite = sprites[sprite_id];

    vec2 finalPosition = (unit_quad[vertex_id] * sprite.size) + sprite.position;
    gl_Position = vec4(finalPosition, 0.0, 1.0);
    outUV = (uvs[vertex_id] * sprite.uv_size) + sprite.uv;
    outColor = sprite.color;
}
