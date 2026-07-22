precision highp float;

// Opaque background pass drawn behind the neon when
// config.neon.opaqueMode != NONE. Fills a rounded-rect band with
// `uOpaqueColor` (default black). Drawn as a fullscreen NDC quad so the
// fragment shader sees every pixel in the viewport; the silhouette comes
// from an analytic rounded-box SDF read off gl_FragCoord (NOT an
// interpolated varying). highp + gl_FragCoord keeps the SDF exact on
// mobile GLES (Mali/Tizen), where a mediump varying carrying pixel
// coordinates would lose precision and a vertex(highp)->fragment(mediump)
// varying mismatch can even fail to link.
//
// Cutoffs are positive pixel distances measured from the rect edge along
// their respective sides (see NeonConfig::insideCutoff / outsideCutoff).
//
//   NONE    - discarded before this shader is dispatched (CPU-side guard).
//   OUTSIDE - fill 0 <= d <= outsideCutoff.
//   INSIDE  - fill -insideCutoff <= d <= 0.
//   BOTH    - fill -insideCutoff <= d <= outsideCutoff (the whole glow band).
//   ALL     - fill the whole viewport (coverage = 1 everywhere).
//
// Boundaries are SDF-anti-aliased over ~1-2 px via fwidth(d).

#define OPAQUE_MODE_NONE    0
#define OPAQUE_MODE_OUTSIDE 1
#define OPAQUE_MODE_INSIDE  2
#define OPAQUE_MODE_BOTH    3
#define OPAQUE_MODE_ALL     4

out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform vec2  uRectCenter;     // rect centre in window pixels (gl_FragCoord space, y-up)
uniform int   uOpaqueMode;
uniform float uInsideCutoff;   // positive distance INSIDE the edge (d = -uInsideCutoff at boundary). Disabled sides collapse to a huge sentinel CPU-side.
uniform float uOutsideCutoff;  // positive distance OUTSIDE the edge (d = +uOutsideCutoff at boundary). Disabled sides collapse to a huge sentinel CPU-side.
uniform float uOpaqueSoftness; // feather width in px at the fill's cutoff boundaries (NeonConfig::opaqueSoftness).
uniform vec4  uOpaqueColor;    // fill colour; only .rgb used today, .a reserved for a later partial-fill pass

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    if (uOpaqueMode == OPAQUE_MODE_NONE) discard; // guard - the CPU already
                                                  // skips this pass, but be
                                                  // explicit for safety.

    vec2  localPos = gl_FragCoord.xy - uRectCenter;
    vec2  halfSize = uRectSize * 0.5;
    float d        = sdRoundBox(localPos, halfSize, uCornerRadius);

    // Feather widths.
    //   aa  = pixel-crisp AA at boundaries where no soft join is required
    //         (the geometric d=0 rect edge under OUTSIDE/INSIDE modes: the
    //         neon emission is at its brightest there and fully occludes the
    //         fill, so a wider feather would only bleed the fill sideways for
    //         no visual gain).
    //   soft = the fill's own cutoff feather (uOpaqueSoftness, independent
    //          of the neon shader's cutoffSoftness so fill and emission can
    //          taper at different rates). Applied at each -insideCutoff /
    //          +outsideCutoff boundary so the fill fades off gently instead
    //          of stamping a hard rectangle. Floored to fwidth so the fade
    //          is never sharper than 1 px on curves.
    float aa   = fwidth(d);
    float soft = max(uOpaqueSoftness, aa);

    float coverage = 0.0;
    if (uOpaqueMode == OPAQUE_MODE_ALL) {
        coverage = 1.0;
    } else if (uOpaqueMode == OPAQUE_MODE_OUTSIDE) {
        // 0 <= d <= outsideCutoff
        float rise = smoothstep(-aa, aa, d);
        float fall = 1.0 - smoothstep(uOutsideCutoff - soft, uOutsideCutoff + soft, d);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_INSIDE) {
        // -insideCutoff <= d <= 0
        float rise = smoothstep(-uInsideCutoff - soft, -uInsideCutoff + soft, d);
        float fall = 1.0 - smoothstep(-aa, aa, d);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_BOTH) {
        // -insideCutoff <= d <= +outsideCutoff (the full glow band)
        float rise = smoothstep(-uInsideCutoff - soft, -uInsideCutoff + soft, d);
        float fall = 1.0 - smoothstep(uOutsideCutoff - soft, uOutsideCutoff + soft, d);
        coverage   = rise * fall;
    }

    if (coverage <= 0.0) discard;

    // Coverage-weighted premultiplied output for
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA): opaque uOpaqueColor.rgb
    // where coverage == 1, smooth AA at boundaries, off-side untouched.
    // uOpaqueColor.a is intentionally not applied yet - reserved for a
    // later premultiplied partial-fill pass.
    fragColor = vec4(uOpaqueColor.rgb * coverage, coverage);
}
