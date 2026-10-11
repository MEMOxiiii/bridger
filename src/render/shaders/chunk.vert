#version 460
// Placeholder chunk vertex shader (Phase 0: proves the GLSL -> SPIR-V build step).
// The real packed-vertex layout arrives with the mesher in Phase 7.

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
    vec3 chunk_origin; // camera-relative
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_texture_layer;

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out uint out_texture_layer;

void main() {
    out_uv = in_uv;
    out_texture_layer = in_texture_layer;
    gl_Position = pc.view_projection * vec4(pc.chunk_origin + in_position, 1.0);
}
