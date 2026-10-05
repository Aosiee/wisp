#version 450
layout(push_constant) uniform PC { mat4 projection; } pc;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUV;
void main() { gl_Position = pc.projection * vec4(aPos, 1.0); vCol = aCol; vUV = aUV; }
