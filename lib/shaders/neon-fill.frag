precision mediump float;

in vec2 vPos;
out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform int   uGlowSide;        // 0 both, 1 inside, 2 outside
uniform float uGlowSideSoftness;

#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Opaque black fill for the neon renderer's opaque mode. Writes black over the
// region the effect is allowed to light:
//   BOTH    -> the whole viewport (no discard).
//   INSIDE  -> only inside the rect edge; the outside is left untouched so the
//              background shows through there.
//   OUTSIDE -> only outside the rect edge; the inside shows the background.
// The neon pass (neon.frag) discards the same unlit side, so the unlit region
// is written by neither pass and stays transparent.
void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d = sdRoundBox(vPos, halfSize, uCornerRadius);
    float softEdge = max(uGlowSideSoftness, 1e-5);

    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  softEdge) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -softEdge) discard;

    fragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
