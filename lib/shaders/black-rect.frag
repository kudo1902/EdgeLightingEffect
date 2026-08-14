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
// The d == 0 rect edge gets exact 1 px box-filter coverage from fwidth(d);
// the cutoff boundaries get a uOpaqueSoftness feather. See main().

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

// --- Band boundary distances -------------------------------------------
// The band's two boundaries, expressed as signed distances: dIn >= 0 means
// "past the inside cutoff", dOut <= 0 means "within the outside cutoff".
//
// INNER: plain Euclidean (d + cut). Inside the shape the rounded-box SDF is
// already per-axis, so the inner boundary is square at cornerRadius 0 and a
// correct parallel curve (radius r - cut) above it. Nothing to fix.
//
// OUTER: Euclidean too whenever cornerRadius > 0, where it is exactly d - cut.
// That is the parallel curve, so the band keeps a uniform width and the opaque
// fill covers precisely as far as the light reaches - no black bulging past
// the glow at the corners.
//
// The one exception is cornerRadius == 0: the parallel curve of a SHARP corner
// is an arc of radius `cut`, so a rect the designer asked to be square comes
// out with rounded outer corners. There, and only there, offset the box
// per-axis instead to keep the corner square. The band is then ~1.41x wider
// measured diagonally across that corner, which is unavoidable - a uniform
// width and a square outer corner cannot both hold at a sharp corner.
//
// Disabled cutoffs arrive as a huge sentinel and still no-op: dIn goes hugely
// positive, dOut hugely negative, so both masks evaluate to 1.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}
float bandInnerDistance(float d, float cut) {
    return d + cut;
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
    float aa      = max(fwidth(d), 1e-6);
    float soft = max(uOpaqueSoftness, aa);

    // The d == 0 edge is covered by an EXACT box filter, not a smoothstep.
    //
    // For a straight edge with |grad d| = 1, a pixel whose centre sits at
    // signed distance d is covered by exactly clamp(0.5 - d/aa, 0, 1) - a ramp
    // ONE pixel wide, spanning d in [-aa/2, +aa/2]. smoothstep(-aa, aa, d)
    // spans TWO, so it shaded pixels that are wholly inside the shape.
    //
    // That showed up as "the outermost 1 px is not black" whenever the
    // geometry ends on a pixel boundary - a 1920x1080 rect filling a 1920x1080
    // surface, i.e. the on-device fullscreen case. The outer pixel's centre is
    // at gl_FragCoord 0.5, so d = -0.5: half a pixel inside, hence fully
    // covered, but the doubled ramp returned 1 - smoothstep(-1, 1, -0.5)
    // = 0.844 and let 15.6% of the background through all the way round.
    // OUTSIDE had the mirror bug, tinting that same ring at 0.156 when it
    // should not touch the interior at all.
    //
    // Normally invisible because the neon filament peaks at d = 0 and covers
    // that pixel - NeonConfig::opaqueOnly is what exposes it.
    //
    // Linear, not smoothstep, on purpose: this IS the analytic coverage, and
    // it matches the "pixel-crisp" intent stated above. The cutoff boundaries
    // keep their smoothstep - there the ramp is an artistic feather
    // (uOpaqueSoftness), not an estimate of pixel coverage.
    float edgeIn  = clamp(0.5 - d / aa, 0.0, 1.0); // 1 inside the rect, 0 outside
    float edgeOut = 1.0 - edgeIn;                  // mirror, for OUTSIDE

    // Band boundaries against the offset rect (square corners at
    // cornerRadius 0); the d == 0 rect edge itself still uses the plain SDF.
    float dOut = bandOuterDistance(localPos, d, halfSize, uCornerRadius, uOutsideCutoff);
    float dIn  = bandInnerDistance(d, uInsideCutoff);

    float coverage = 0.0;
    if (uOpaqueMode == OPAQUE_MODE_ALL) {
        coverage = 1.0;
    } else if (uOpaqueMode == OPAQUE_MODE_OUTSIDE) {
        // 0 <= d <= outsideCutoff
        float rise = edgeOut;
        float fall = 1.0 - smoothstep(-soft, soft, dOut);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_INSIDE) {
        // -insideCutoff <= d <= 0
        float rise = smoothstep(-soft, soft, dIn);
        float fall = edgeIn;
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_BOTH) {
        // -insideCutoff <= d <= +outsideCutoff (the full glow band)
        float rise = smoothstep(-soft, soft, dIn);
        float fall = 1.0 - smoothstep(-soft, soft, dOut);
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
