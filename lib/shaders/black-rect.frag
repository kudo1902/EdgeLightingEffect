precision highp float;

// Opaque background pass drawn behind the neon when
// config.neon.opaqueMode != NONE. Fills a rounded-rect band with
// `uOpaqueColor` (default black). The silhouette comes from an analytic
// rounded-box SDF read off gl_FragCoord (NOT an interpolated varying), so it
// is independent of the geometry it is drawn on - which is what lets the CPU
// bound this pass with a ring sized to the band (see
// NeonRenderer::setupFillGeometry) instead of shading the whole viewport to
// paint a 20 px band.
//
// highp + gl_FragCoord keeps the SDF exact on mobile GLES (Mali/Tizen), where
// a mediump varying carrying pixel coordinates would lose precision and a
// vertex(highp)->fragment(mediump) varying mismatch can even fail to link.
//
// Cutoffs are positive pixel distances measured from the rect edge along
// their respective sides (see NeonConfig::insideCutoff / outsideCutoff).
//
//   NONE    - never dispatched; the CPU skips the pass entirely.
//   OUTSIDE - fill 0 <= d <= outsideCutoff.
//   INSIDE  - fill -insideCutoff <= d <= 0.
//   BOTH    - fill -insideCutoff <= d <= outsideCutoff (the whole glow band).
//   ALL     - never dispatched either: coverage is 1 at every pixel, so the
//             CPU expresses it as a scissored glClear and no shader runs.
//             The branch below is kept only as a safety fallback.
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
    // Guard - the CPU already skips this pass for NONE (and handles ALL with
    // glClear), but be explicit for safety. Transparent black rather than
    // discard, for the reason given at the coverage test below: this shader
    // contains no discard at all, deliberately.
    if (uOpaqueMode == OPAQUE_MODE_NONE) { fragColor = vec4(0.0); return; }

    // ALL is coverage 1 at every pixel, so none of the shaping below can
    // change its result. It is also the one mode the CPU still hands a
    // fullscreen quad (every other mode is bounded by the band ring in
    // NeonRenderer::setupFillGeometry), so it is the mode that shades the most
    // fragments - and the SDF, the fwidth and the two smoothsteps would all be
    // dead work in every one of them. Same output, taken directly.
    if (uOpaqueMode == OPAQUE_MODE_ALL) {
        fragColor = vec4(uOpaqueColor.rgb, 1.0);
        return;
    }

    vec2  localPos = gl_FragCoord.xy - uRectCenter;
    vec2  halfSize = uRectSize * 0.5;
    float d        = sdRoundBox(localPos, halfSize, uCornerRadius);

    // Feather widths. BOTH are TOTAL widths in px, and both are floored at one
    // pixel so nothing here can stair-step.
    //   aa    = pixel-crisp AA at boundaries where no soft join is required
    //           (the geometric d=0 rect edge under OUTSIDE/INSIDE modes: the
    //           neon emission is at its brightest there and fully occludes the
    //           fill, so a wider feather would only bleed the fill sideways for
    //           no visual gain).
    //   softW = the fill's own cutoff feather (uOpaqueSoftness, independent of
    //           the neon shader's cutoffSoftness so fill and emission can taper
    //           at different rates). Applied at each -insideCutoff /
    //           +outsideCutoff boundary so the fill fades off gently instead of
    //           stamping a hard rectangle.
    //
    // Both ramps used to be written as smoothstep(-w, w, x), which spans 2w -
    // TWICE the width being asked for. Two consequences, and they are the same
    // root cause as the d == 0 bug documented below:
    //
    //   - At uOpaqueSoftness 0 the fallback to aa gave a 2 px ramp centred on
    //     each cutoff, so the outermost and innermost pixel of the band were
    //     both partially covered and the fill read ~1 px narrower per side than
    //     the cutoff asked for.
    //   - At any non-zero softness the fade ran 2x wider than the documented
    //     "feather width in pixels" (NeonConfig::opaqueSoftness).
    //
    // Halving fixes both: the ramp now spans exactly softW, centred on the
    // boundary, so a pixel wholly inside the band is fully covered and a
    // softness of S px feathers over S px. NOTE this changes the look of any
    // already-tuned non-zero opaqueSoftness - it is half as wide as before, and
    // is now what the config says it is.
    float aa       = max(fwidth(d), 1e-6);
    float softW    = max(uOpaqueSoftness, aa);
    float softHalf = 0.5 * softW;

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
    // that pixel - DebugConfig::opaqueOnly is what exposes it.
    //
    // Linear, not smoothstep, on purpose: this IS the analytic coverage, and
    // it matches the "pixel-crisp" intent stated above. The cutoff boundaries
    // keep their smoothstep even though they are now the same width, because
    // there the ramp is an artistic feather that can run tens of px wide
    // (uOpaqueSoftness), and a linear alpha ramp that wide shows Mach banding
    // at its ends. At the aa floor the two shapes differ by at most ~0.09
    // coverage on the single boundary pixel, which is not resolvable.
    float edgeIn  = clamp(0.5 - d / aa, 0.0, 1.0); // 1 inside the rect, 0 outside
    float edgeOut = 1.0 - edgeIn;                  // mirror, for OUTSIDE

    // Band boundaries against the offset rect (square corners at
    // cornerRadius 0); the d == 0 rect edge itself still uses the plain SDF.
    float dOut = bandOuterDistance(localPos, d, halfSize, uCornerRadius, uOutsideCutoff);
    float dIn  = bandInnerDistance(d, uInsideCutoff);

    float coverage = 0.0;
    if (uOpaqueMode == OPAQUE_MODE_OUTSIDE) {
        // 0 <= d <= outsideCutoff
        float rise = edgeOut;
        float fall = 1.0 - smoothstep(-softHalf, softHalf, dOut);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_INSIDE) {
        // -insideCutoff <= d <= 0
        float rise = smoothstep(-softHalf, softHalf, dIn);
        float fall = edgeIn;
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_BOTH) {
        // -insideCutoff <= d <= +outsideCutoff (the full glow band)
        float rise = smoothstep(-softHalf, softHalf, dIn);
        float fall = 1.0 - smoothstep(-softHalf, softHalf, dOut);
        coverage   = rise * fall;
    }

    // Transparent black instead of discard, and NOT as a stylistic choice.
    // Under this pass's glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) a coverage
    // of 0 gives dst = 0 + dst * (1 - 0) = dst, so the two are bit-exact - 135
    // offscreen scenes come back byte-identical either way. What differs is
    // speed: measured back to back in one process at 3840x2160, dropping the
    // discard took ~0.2-0.3 ms off the fullscreen cases (an opaque mode whose
    // cutoff is disabled, where the ring in NeonRenderer::setupFillGeometry
    // has nothing to bound). It is worth ~0 on the bounded ring, which shades
    // too few fragments for it to show.
    //
    // The effect scales with fragments shaded, so it should hold on the Mali /
    // Tizen targets too - a shader with discard cannot use forward pixel kill
    // there. That direction is UNVERIFIED here; if it ever measures worse on
    // device, restoring `if (coverage <= 0.0) discard;` is the whole revert.

    // Coverage-weighted premultiplied output for
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA): opaque uOpaqueColor.rgb
    // where coverage == 1, smooth AA at boundaries, off-side untouched.
    // uOpaqueColor.a is intentionally not applied yet - reserved for a
    // later premultiplied partial-fill pass.
    fragColor = vec4(uOpaqueColor.rgb * coverage, coverage);
}
