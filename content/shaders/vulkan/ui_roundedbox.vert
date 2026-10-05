#version 450
layout(push_constant) uniform PC {
    mat4 projection;
    layout(offset=64) vec4 radii;
    layout(offset=80) float outlineWidth;
    layout(offset=96) vec4 outlineColour;
    layout(offset=112) vec2 halfSize;
} pc;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out vec4 vRadii;
layout(location = 3) flat out float vOutlineWidth;
layout(location = 4) flat out vec4 vOutlineColour;
layout(location = 5) flat out vec2 vHalfSize;
void main() {
    gl_Position = pc.projection * vec4(aPos, 1.0);
    vCol = aCol; vUV = aUV;
    vRadii = pc.radii; vOutlineWidth = pc.outlineWidth; vOutlineColour = pc.outlineColour;
    vHalfSize = pc.halfSize;
}
