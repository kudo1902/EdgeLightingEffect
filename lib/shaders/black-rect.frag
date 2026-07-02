precision highp float;

// Opaque-black pass drawn behind the neon when config.neon.opaque is on.
// Drawn as a fullscreen NDC quad so the fragment shader sees every pixel in the
// viewport; the silhouette comes from the analytic rounded-box SDF below, read
// from gl_FragCoord (NOT an interpolated varying). highp + gl_FragCoord keeps
// the SDF exact on mobile GLES (Mali/Tizen), where a mediump varying carrying
// pixel coordinates would lose precision and a vertex(highp)->fragment(mediump)
// varying mismatch can even fail to link.
//
//   BOTH    — black covers the whole viewport (coverage = 1 everywhere).
//   INSIDE  — black only where d <= softEdge (rect interior + feather); pixels
//             further outside stay transparent so the off-side shows through.
//   OUTSIDE — mirror of INSIDE.
//
// Boundaries are SDF-anti-aliased over ~1 px via fwidth(d).

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform vec2  uRectCenter; // rect centre in window pixels (gl_FragCoord space, y-up)
uniform int   uGlowSide;
uniform float uSoftEdge;   // matches the neon shader's softEdge

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2  localPos = gl_FragCoord.xy - uRectCenter;
    vec2  halfSize = uRectSize * 0.5;
    float d        = sdRoundBox(localPos, halfSize, uCornerRadius);

    // Feather the cut over [-edge, +edge] so the black quad fades across the
    // same soft band the neon shader uses. Floor to fwidth(d) (which gives
    // ~2 screen px of AA gradient, since the smoothstep width is 2·edge):
    // 1 px was enough for axis-aligned edges but visibly stair-stepped on
    // rounded corners. 2 px stays smooth on curves; the extra softness is
    // imperceptible against black.
    float effectiveEdge = max(uSoftEdge, fwidth(d));

    float coverage = 1.0;
    if (uGlowSide == GLOW_SIDE_INSIDE) {
        coverage = 1.0 - smoothstep(-effectiveEdge, effectiveEdge, d);
    } else if (uGlowSide == GLOW_SIDE_OUTSIDE) {
        coverage = smoothstep(-effectiveEdge, effectiveEdge, d);
    }
    if (coverage <= 0.0) discard;

    // Premultiplied black for glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA):
    // opaque black where coverage == 1, smooth AA at the INSIDE/OUTSIDE cut, and
    // the off-side leaves the background untouched.
    fragColor = vec4(0.0, 0.0, 0.0, coverage);
}
