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
//   ALL     - rarely dispatched: coverage is 1 at every pixel, so the CPU
//             normally expresses it as a scissored glClear and no shader runs.
//
// BOTH reaches that same clear whenever NEITHER cutoff is enabled, which is
// the default state of both. Disabled cutoffs arrive as a huge sentinel, so
// dIn saturates positive and dOut negative and the arm below returns coverage
// 1 at every fragment - identical output for the price of shading the whole
// viewport. NeonRenderer::FillsWholeViewport is what catches it.
//
// "Rarely", not "never": a clear ignores the depth and stencil tests, so when
// the host has either enabled the CPU keeps the draw (NeonRenderer::
// ClearClipsLikeDraw) and dispatches this shader over a fullscreen quad. Both
// coverage-1 paths below are therefore live code on that route, not dead
// arms - keep them correct.
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
    // ONE straight-line path to a single write at the bottom: no early return
    // above the derivative, and no discard anywhere. Both of those are load
    // bearing, and each cost a separate measurement to learn.
    //
    // NO EARLY RETURN. The NONE and ALL guards used to sit here as
    // `fragColor = ...; return;`, ahead of the fwidth(d) below. They read as
    // free - they branch on a uniform, so every lane in the draw takes the
    // same side - but the compiler does not get to assume that, and a
    // derivative downstream of a return it cannot prove uniform lands the
    // whole shader on a slower path. Measured at 3840x2160 on a fill covering
    // the surface: 0.29 ms before the returns were added, 0.41 ms with them,
    // 0.26 ms once they moved back into the chain below. That is a ~35%
    // penalty paid by EVERY fragment in EVERY mode, to skip work in two modes
    // the CPU never dispatches (NONE is guarded caller-side, ALL is a
    // scissored glClear). Keep the guards where they are - folded into the
    // coverage chain, below the derivative - and if a new one is ever needed,
    // it goes there too, not up here.
    //
    // NONE is not tested at all: it matches no arm of that chain, so it falls
    // through at coverage 0 and writes transparent black, which is the correct
    // no-op for this pass's blend.

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

    // The band boundaries are computed INSIDE the arms that read them, not
    // once above the chain. Each arm uses at most one of them - OUTSIDE never
    // looks at the inner boundary, INSIDE never at the outer - so hoisting
    // them made every OUTSIDE and INSIDE fragment pay for a distance it then
    // discarded. bandOuterDistance is the one that matters: at cornerRadius 0
    // it is a whole second sdRoundBox, length() and all, and cornerRadius 0 is
    // not an exotic setting.
    //
    // MEASURED NOTHING, and that is worth writing down so nobody re-measures
    // it hoping otherwise. INSIDE at cornerRadius 0, cutoffs off, fill
    // isolated with debug.opaqueOnly, 3600x2126, min of 200 frames around
    // glFinish, three runs a side: 0.79 / 0.84 / 0.82 ms hoisted against
    // 0.81 / 0.74 / 0.87 ms sunk. That is one distribution, not two - the
    // Apple GLSL compiler was already sinking the pure computation into the
    // arm that consumes it, exactly as a compiler is free to do.
    //
    // Kept anyway, on two grounds and neither of them speed on this driver:
    // the source now says what it means, and this project's real target is a
    // Mali / Tizen compiler nobody here has measured. Also because this
    // shader has already been bitten once by trusting a compiler to do the
    // obvious thing with its control flow - see the 35% note at the top of
    // main(), which is why the guards below are arms rather than early
    // returns. Being explicit is free; assuming was not.
    //
    // Note what did NOT move: `aa`, `softW`, `softHalf`, `edgeIn` and
    // `edgeOut` all stay above the chain, because `aa` comes from fwidth(d).
    // A derivative must not end up downstream of control flow the compiler
    // cannot prove uniform - that IS the 35% mistake documented at the top.
    // Only branch-free ALU moves down here; the derivative stays put.
    float coverage = 0.0;
    if (uOpaqueMode == OPAQUE_MODE_ALL) {
        // Coverage 1 everywhere - none of the shaping above changes it.
        // Reached only when the CPU could not substitute a clear, i.e. with
        // the depth or stencil test enabled (see the mode table at the top of
        // this file), so it is rare rather than dead. See the note at the top
        // of main() for why it is an arm of this chain rather than an early
        // return.
        coverage = 1.0;
    } else if (uOpaqueMode == OPAQUE_MODE_OUTSIDE) {
        // 0 <= d <= outsideCutoff. No inner boundary here.
        float dOut = bandOuterDistance(localPos, d, halfSize, uCornerRadius, uOutsideCutoff);
        float rise = edgeOut;
        float fall = 1.0 - smoothstep(-softHalf, softHalf, dOut);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_INSIDE) {
        // -insideCutoff <= d <= 0. No outer boundary here - this is the arm
        // that was paying for a second sdRoundBox it never read.
        float dIn  = bandInnerDistance(d, uInsideCutoff);
        float rise = smoothstep(-softHalf, softHalf, dIn);
        float fall = edgeIn;
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_BOTH) {
        // -insideCutoff <= d <= +outsideCutoff (the full glow band). The one
        // arm that genuinely needs both boundaries.
        float dIn  = bandInnerDistance(d, uInsideCutoff);
        float dOut = bandOuterDistance(localPos, d, halfSize, uCornerRadius, uOutsideCutoff);
        float rise = smoothstep(-softHalf, softHalf, dIn);
        float fall = 1.0 - smoothstep(-softHalf, softHalf, dOut);
        coverage   = rise * fall;
    }

    // No `if (coverage <= 0.0) discard;` here, and it is safe to leave out:
    // under this pass's glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) a coverage
    // of 0 gives dst = 0 + dst * (1 - 0) = dst, so writing transparent black
    // is bit-exact with discarding - 333 offscreen scenes come back
    // byte-identical either way.
    //
    // It buys nothing measurable HERE: restoring the discard and re-running
    // the 3840x2160 cases came back inside noise, because the ring in
    // NeonRenderer::setupFillGeometry already keeps most zero-coverage
    // fragments from being rasterised at all, and the cases it cannot bound
    // are the ones where coverage is 1 and the discard never fires anyway.
    // It is left out because a shader with no discard is the friendlier shape
    // for the Mali / Tizen targets, where discard defeats forward pixel kill -
    // UNVERIFIED on device, but it costs nothing to be on the right side of.

    // Coverage-weighted premultiplied output for
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA): opaque uOpaqueColor.rgb
    // where coverage == 1, smooth AA at boundaries, off-side untouched.
    // uOpaqueColor.a is intentionally not applied yet - reserved for a
    // later premultiplied partial-fill pass.
    fragColor = vec4(uOpaqueColor.rgb * coverage, coverage);
}
