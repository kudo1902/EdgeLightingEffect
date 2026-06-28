precision mediump float;

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

// Opaque mode (mirrors NeonRenderer): occlude the background over the lit
// region — including its dark surround — while leaving the unlit side of a
// one-sided glow transparent. Uses gl_FragCoord (full-res screen px) + the
// rect, so the half-res FBO content doesn't need to carry the mask.
uniform int   uOpaque;           // 0 = composite (premult over), 1 = opaque
uniform vec2  uCenter;           // rect centre, screen px (bottom-left origin)
uniform vec2  uRectSize;         // px
uniform float uCornerRadius;     // px
uniform int   uGlowSide;         // 0 both, 1 inside, 2 outside
uniform float uGlowSideSoftness; // px

#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

highp float sdRoundBox(highp vec2 p, highp vec2 b, float r) {
    highp vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 uv = vPos * 0.5 + 0.5;
    vec4 c = texture(uSource, uv); // premultiplied colour + coverage alpha

    if (uOpaque == 1) {
        // Discard the unlit side so the background shows through there; the lit
        // side (incl. its transparent-black surround) is written opaquely.
        highp float d = sdRoundBox(gl_FragCoord.xy - uCenter, uRectSize * 0.5, uCornerRadius);
        float softEdge = max(uGlowSideSoftness, 1e-5);
        if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
        if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

        // Premultiplied colour over black == c.rgb; write it opaque.
        fragColor = vec4(c.rgb, 1.0);
    } else {
        // Composite over the backbuffer (renderer sets GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
        fragColor = c;
    }
}
