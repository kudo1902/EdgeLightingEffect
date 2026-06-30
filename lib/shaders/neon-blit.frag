precision highp float;

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

// Opaque mode (mirrors the base renderer's black-rect): occlude the background
// over the lit region while leaving the unlit side of a one-sided glow
// transparent. highp + gl_FragCoord keeps the SDF exact on Mali/Tizen.
uniform int   uOpaque;       // 0 = composite over, 1 = opaque
uniform vec2  uRectCenter;   // rect centre in window px (gl_FragCoord space, y-up)
uniform vec2  uRectSize;     // px
uniform float uCornerRadius; // px
uniform int   uGlowSide;     // 0 both, 1 inside, 2 outside
uniform float uSoftEdge;     // px

#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    vec4 c = texture(uSource, uv); // premultiplied colour + coverage alpha

    if (uOpaque == 1) {
        // Discard the unlit side of a one-sided glow so the background shows
        // there; the lit side (incl. its transparent-black surround) is written
        // opaquely. The Pass-1 neon discards the same side, so the FBO is empty
        // there too.
        float d = sdRoundBox(gl_FragCoord.xy - uRectCenter, uRectSize * 0.5, uCornerRadius);
        if (uGlowSide == GLOW_SIDE_INSIDE  && d >  uSoftEdge) discard;
        if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -uSoftEdge) discard;

        // Premultiplied colour over black == c.rgb; write it opaque (blend off).
        fragColor = vec4(c.rgb, 1.0);
    } else {
        // Composite over the backbuffer (renderer sets GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
        fragColor = c;
    }
}
