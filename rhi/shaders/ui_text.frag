#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(push_constant) uniform PC { mat4 projection; uint texIndex; } pc;
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outCol;
layout(binding = 0) uniform sampler2D u_Textures[];
void main() { float a = texture(u_Textures[nonuniformEXT(int(pc.texIndex))], vUV).a; outCol = vCol * vec4(1.0, 1.0, 1.0, a); }
