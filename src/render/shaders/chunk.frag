#version 460

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
    vec4 origin;
    vec4 fog;
    vec4 sky;
    vec4 camera;
} pc;

layout(set = 0, binding = 0) uniform sampler2DArray block_textures;

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in uint in_layer;
layout(location = 2) flat in uint in_flags;
layout(location = 3) in vec4 in_color;
layout(location = 4) in float in_distance;
layout(location = 5) in float in_world_y;

layout(location = 0) out vec4 out_color;

void main() {
    if (pc.fog.w > 0.5 && in_world_y >= pc.fog.z - 0.001) discard; // Y slice
    vec4 texel = texture(block_textures, vec3(in_uv, float(in_layer)));
    if ((in_flags & 1u) != 0u && texel.a < 0.5) discard;            // cutout
    vec4 c = texel * in_color;
    if (pc.sky.a < 0.5) c.a = 1.0;                                  // opaque passes
    else if (c.a < 0.004) discard;
    if (pc.fog.y > 0.0 && pc.camera.w < 0.5) {
        float f = clamp((in_distance - pc.fog.x) / max(pc.fog.y - pc.fog.x, 1.0), 0.0, 1.0);
        c.rgb = mix(c.rgb, pc.sky.rgb, f);
    }
    out_color = c;
}
