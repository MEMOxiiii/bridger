#version 460
// Section vertices (mesher.hpp Vertex, 24 bytes). Positions are section-local; the push constant
// carries the section origin relative to the camera, so precision holds far from 0,0.

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
    vec4 origin;      // xyz: section origin - camera position; w: section origin y (world)
    vec4 fog;         // x: start, y: end (0 = off), z: slice y (world), w: 1 = slice on
    vec4 sky;         // rgb: sky colour, a: 1 = translucent pass
    vec4 camera;      // xyz: camera world position (y used for the slice), w: map mode
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in uint in_layer_flags; // layer | flags << 16
layout(location = 3) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out uint out_layer;
layout(location = 2) flat out uint out_flags;
layout(location = 3) out vec4 out_color;
layout(location = 4) out float out_distance;
layout(location = 5) out float out_world_y;

void main() {
    vec3 rel = pc.origin.xyz + in_position;
    out_uv = in_uv;
    out_layer = in_layer_flags & 0xFFFFu;
    out_flags = in_layer_flags >> 16;
    out_color = in_color;
    out_distance = length(rel.xz);
    out_world_y = pc.origin.w + in_position.y;
    gl_Position = pc.view_projection * vec4(rel, 1.0);
}
