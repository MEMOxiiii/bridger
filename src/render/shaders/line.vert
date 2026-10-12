#version 460
// Outline boxes (selection, report highlights): camera-relative line list with colours.

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color;
    gl_Position = pc.view_projection * vec4(in_position, 1.0);
}
