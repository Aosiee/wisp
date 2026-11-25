#version 450
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in vec4 vRadii;
layout(location = 3) flat in float vOutlineWidth;
layout(location = 4) flat in vec4 vOutlineColour;
layout(location = 5) flat in vec2 vHalfSize;
layout(location = 0) out vec4 outCol;

float sdRoundedBox(vec2 p, vec2 b, vec4 r) {
    r.xy = (p.x > 0.0) ? r.yw : r.xz;
    r.x  = (p.y > 0.0) ? r.x  : r.y;
    vec2 q = abs(p) - b + r.x;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r.x;
}

void main() {
    vec2 p = (vUV - 0.5) * 2.0 * vHalfSize;
    vec4 r = vRadii;
    float m = min(vHalfSize.x, vHalfSize.y);
    r = min(r, vec4(m));
    float d = sdRoundedBox(p, vHalfSize, r);

    // Fill first (AA coverage), then paint the outline ring over it.
    // The previous outline branch inverted the fill/outline mix and zeroed
    // the interior alpha, so any rounded box drawn with an outline rendered
    // ONLY its outline ring and never its fill (the "zero alpha" swatches).
    float aa = fwidth(d);
    float fillAlpha = 1.0 - smoothstep(-aa, aa, d);
    outCol = vCol;
    outCol.a *= fillAlpha;

    if (vOutlineWidth > 0.0) {
        float outer = abs(d) - vOutlineWidth;
        float outerAA = fwidth(outer);
        float ring = 1.0 - smoothstep(-outerAA, outerAA, outer);
        outCol = mix(outCol, vOutlineColour, ring);
    }
}
