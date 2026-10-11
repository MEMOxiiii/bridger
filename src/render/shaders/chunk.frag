#version 460
// Placeholder chunk fragment shader (Phase 0: proves the GLSL -> SPIR-V build step).

layout(set = 0, binding = 0) uniform sampler2DArray block_textures;

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in uint in_texture_layer;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 color = texture(block_textures, vec3(in_uv, float(in_texture_layer)));
    if (color.a < 0.5) discard; // cutout; translucent blocks get their own pass
    out_color = color;
}
