#version 450

// Texture bindings
layout(set = 2, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 yuv_to_rgb(float y, float u, float v) {
    u -= 0.5;
    v -= 0.5;

    vec3 rgb = vec3(
        y + 1.402 * v,
        y - 0.344136 * u - 0.714136 * v,
        y + 1.772 * u
    );

    return clamp(rgb, 0.0, 1.0);
}

void main() {
// 1. Sample Y from the top 2/3 of the texture buffer
    // Normalizing Y-coordinates to scale within [0.0, 2/3]
    vec2 y_uv = vec2(v_uv.x, v_uv.y * (2.0 / 3.0));
    float y = texture(u_tex, y_uv).r;

    // 2. Sample U from the block below Y ([2/3, 5/6] vertical range)
    vec2 u_uv = vec2(v_uv.x * 0.5, (2.0 / 3.0) + (v_uv.y * (1.0 / 6.0)));
    float u = texture(u_tex, u_uv).r;

    // 3. Sample V from the bottom block ([5/6, 1.0] vertical range)
    vec2 v_uv_coord = vec2(v_uv.x * 0.5, (5.0 / 6.0) + (v_uv.y * (1.0 / 6.0)));
    float v = texture(u_tex, v_uv_coord).r;

    vec3 rgb = yuv_to_rgb(y, u, v);
    o_color = vec4(rgb, 1.0) * v_color;
}