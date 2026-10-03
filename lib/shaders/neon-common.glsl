// ---------------------------------------------------------------------------
// What neon.frag and neon-gather.frag share
// ---------------------------------------------------------------------------
//
// Not a shader on its own. lib/CMakeLists.txt reads it and shaders.h.in
// injects it into both, after the version line and @NEON_TUNING@ and ahead of
// the file's own source - so everything here is declared before either file's
// first line, and neither declares any of it again.
//
// It is the perimeter GATHER - the colour loop that is ~95% of the neon's cost
// - and exactly what that loop reads. Two programs run it: neon.frag's direct
// path, inline, at resolutionScale 1.0; and neon-gather.frag, alone, into the
// gather buffer below it. ONE copy, so the two cannot drift: a change to the
// loop lands on both paths by construction, and tools/neon-scale-check is what
// says whether they still agree.
//
// The loop half is compiled out under NEON_READS_GATHER, the neon.frag variant
// that reads the gather's result back instead of running it - so that variant
// has no sample block, count or emission table for the renderer to bind.

precision highp float;

in vec2 vPos;

uniform vec2  uRectSize;
uniform float uCornerRadius;

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops): when .w > 0.5 the segment's colour comes from row `s` of
// uSegmentLUT (its own head-to-tail gradient); when .w == 0 it inherits the
// current base gradient sample. When uSegmentCount == 0 the whole feature is
// skipped in the gather loop.
//
// Here rather than in neon.frag because the gather branches on uSegmentCount;
// every neon program reads it, whichever file it is built from.
//
// Declared in a std140 uniform block (the DALi PunctualLightBlock pattern):
// DALi writes one element per registered property ("uSegments[0]", ...) into
// the block's UBO at the reflected std140 array stride. On desktop GL the
// block is fed from a UBO in neon-renderer.cpp.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

const float PI      = 3.141592653589793;
const float TWO_PI  = 6.283185307179586;
const float HALF_PI = 1.5707963267948966;

// Perimeter of the rounded rect, in the px uRectSize is in - SCALED px below
// resolutionScale 1.0. Sizes the gather's colour kernel, and neon.frag also
// converts the arc feathers from px to perimeter fractions with it.
float rectPerimeter() {
    float r = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    return 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;
}

#ifndef NEON_READS_GATHER
// Loop sample positions (perimeter points) as a std140 uniform block. Each
// entry is packed as a vec4 (only .xy is meaningful; std140 pads vec2 to a
// 16-byte stride anyway) so the shader reads raw float32 out of the constant
// cache in the gather loop. Fixed size at compile time - see the shared
// NEON_MAX_LOOP_SAMPLES tuning constant.
layout(std140) uniform LoopSamplesBlock
{
    vec4 uLoopSamples[NEON_MAX_LOOP_SAMPLES];
};

// How many of the block's entries are actually in use this frame, from
// NeonConfig::numSamples (clamped to 1..NEON_MAX_LOOP_SAMPLES CPU-side). The
// block is always allocated at full size; entries past this are (0,0,0,0) and
// never read, because the gather stops here.
//
// The emission pre-pass is handed the SAME count and bakes its table over
// exactly these indices, so texel i there is sample i here. The two must agree
// or the gather reads emission belonging to a different perimeter position.
uniform int uNumSamples;

// Perimeter emission table from neon-emission.frag: NEON_MAX_LOOP_SAMPLES
// wide, 2 tall, RGBA16F (RGBA8 where the driver refuses float rendering).
//   row 0: .rgb = arcColour * arcW, .a = arcW
//   row 1: .rgb = SUM(segColour * bell), .a = SUM(bell)
// Read with texelFetch at integer sample index - never filtered, since
// neighbouring texels are unrelated perimeter samples. See the gather loop
// and docs/emission-prepass.md.
uniform sampler2D uEmission;

// The gather's four results - its ONLY outputs, and all four smooth across
// the screen (Lorentzian-weighted means over the whole perimeter), which is
// what lets neon-gather.frag store them on a grid far coarser than a pixel.
struct PerimeterGather
{
    vec3  hue;      // base perimeter hue, unit magnitude
    float arcCover; // g-weighted mean arc coverage x intensity
    vec3  segHue;   // segment hue, unit magnitude
    float segCover; // g-weighted mean segment boost x bell
};

// --- Colour gather ---------------------------------------------------------
// This loop gathers COLOUR ONLY - the halo and bloom intensities are computed
// in closed form by neon.frag after it, and every per-sample colour / mask
// term is precomputed by neon-emission.frag into uEmission.
//
// Per iteration: 1 UBO read for the sample position, 1 sub, 1 dot, 1
// reciprocal, and 2 texelFetches - 1 where the config has no segments and the
// branch below takes the shorter body. What used to live here - the arc
// winner-take-all scan over uArcCount, the segment loop over uSegmentCount,
// and one to two FILTERED LUT fetches - was a pure function of
// (si, uTime, config), so it did not belong in a loop that runs once per
// fragment. Hoisting it also removed two dynamic inner loops (which blocked
// unrolling), a serial reduction (`if (mask > bestMask)`), and the
// loop-carried `si += dti` chain.
//
// See docs/emission-prepass.md for the packing and the invariant that keeps
// the split honest.
PerimeterGather gatherPerimeter(vec2 p) {
    // kc is the COLOUR gather weight, and it is the one length in the neon
    // that is NOT in pixels. The blend is a uNumSamples-sample sum over
    // colours baked from the gradient LUT, which is indexed by perimeter
    // FRACTION, so a pixel-sized kernel spans a different slice of the
    // gradient on every geometry - the same stops render washed out small and
    // crisp large. Sizing it as a fraction of the perimeter makes the gradient
    // read identically at any size, and pins the kernel at a constant 1.13
    // sample spacings so it cannot bead at any size either. Not coupled to
    // glowRadius: the gather is colour only, so a wide glow has no business
    // desaturating the ring. See neon-tuning.h for the measurements behind
    // this, and neon.frag's kernel widths for the emission widths it is split
    // from.
    float kc  = max(rectPerimeter() * COLOR_BLEND_PERIM_FRAC, EMISSION_MIN_WIDTH);
    float kc2 = kc * kc;

    vec3  acc       = vec3(0.0); // base colour x arc-gated gather weight
    vec3  segAcc    = vec3(0.0); // segment colour x bell x gather weight
    float wsumLit   = 0.0; // SUM ARC-GATED g     - normalises `hue` (see below)
    float wsumSegW  = 0.0; // SUM SEGMENT bell*g  - normalises the segment hue
    // SUM UNGATED g. One add per iteration, and it buys the GLOW's magnitude:
    // wsumLit / wsumAll and wsumSegW / wsumAll are the g-weighted MEANS of the
    // two coverages over the perimeter, which is what the halo and bloom have
    // to be scaled by. See neon.frag's glow-coverage block.
    float wsumAll   = 0.0;

    // Runtime loop bound, from NeonConfig::numSamples. The UBO behind
    // uLoopSamples is always NEON_MAX_LOOP_SAMPLES long, so this only ever
    // stops the walk EARLY - it can never run off the end. Hoisted into a
    // local because a uniform in the condition is re-read per iteration on
    // some drivers.
    int n = uNumSamples;

    // TWO LOOP BODIES, ONE UNIFORM BRANCH, and the duplication is deliberate.
    //
    // Row 1 of the emission table is the segment term, and neon-emission.frag
    // writes it as vec4(segSum, bellSum) over a loop bounded by uSegmentCount.
    // At uSegmentCount == 0 that loop does not execute, so the row is exactly
    // vec4(0.0) at every texel - and the two accumulations it feeds here are
    // provably no-ops. A config with no segments was therefore issuing one
    // texture read per sample per fragment to add zero: 128 of them, over a
    // quad that covers most of the viewport at the default glowRadius.
    //
    // The branch has to be OUTSIDE the loop, not inside it. Gating the fetch
    // per iteration (`uSegmentCount > 0 ? texelFetch(...) : vec4(0.0)`) was
    // measured SLOWER than leaving the fetch alone - 4.52 ms against a 4.23 ms
    // baseline at 1920x1080 - because the per-iteration branch costs what the
    // fetch it skips cost. Hoisting it so each body is straight-line is what
    // actually pays: 1.46x on the whole neon layer, measured as an interleaved
    // A/B over seven rounds at two resolutions.
    //
    // The uSegmentCount > 0 body below is the original loop VERBATIM, which is
    // what makes a segmented config byte-identical by construction rather than
    // by measurement. The segment-less body drops exactly the two dead
    // accumulations and the fetch that fed them; segAcc and wsumSegW keep the
    // vec3(0.0) / 0.0 they were initialised with, which is what the deleted
    // adds would have left them holding. Verified 0 of 2073600 pixels changed
    // across six scenes - no segments, a plain segment, a segment with its own
    // stops, four arcs with stops, both cutoffs enabled, and resolutionScale
    // 0.5.
    //
    // Branching on a uniform is safe here for the same reason the lens flare's
    // uSpread guard is (see lens-flare.frag): the condition is uniform across
    // the draw, so control flow stays uniform. Nothing in either body takes a
    // derivative in any case - texelFetch has no LOD to compute.
    if (uSegmentCount > 0) {
        for (int i = 0; i < n; i++) {
            vec2  dv  = p - uLoopSamples[i].xy;
            float dd  = dot(dv, dv);

            float g   = 1.0 / (dd + kc2);

            // Both rows of the emission table for this sample. Row 0 carries
            // the arc term already premultiplied by its own gather weight
            // arcW, plus arcW itself for the denominator; row 1 does the same
            // for the summed segment term. texelFetch (not texture): integer
            // sample index, no filtering, no wrap math, no LOD derivatives.
            vec4 e0 = texelFetch(uEmission, ivec2(i, 0), 0);
            vec4 e1 = texelFetch(uEmission, ivec2(i, 1), 0);

            // GATED normalisation, and it is the point. Dividing by the same
            // weight the numerator was gathered with makes `hue` a pure hue of
            // unit magnitude: it carries no coverage and no per-arc intensity,
            // both of which cancel. Those reach the emission solely through
            // emitCover / filamentGate in neon.frag, which are px-based and
            // size-invariant. segAcc / wsumSegW does the identical thing for
            // the segment hue.
            //
            // Both used to divide by an UNGATED sum over every sample, so an
            // unlit far side of the ring dragged the lit colour toward black by
            // roughly kc / rectHeight. With kc pinned to a fixed px span that
            // ratio grew as the rect shrank: a quarter-perimeter arc measured
            // 0.79 of full brightness at 200x150 against 0.97 at 1920x1080.
            // Gated normalisation is exactly 1.0 at every size.
            //
            // e0.rgb is baseColI * arcW and e0.a is arcW, so these two lines
            // are exactly the old `acc += baseColI * lg` / `wsumLit += lg` with
            // lg = g * arcW.
            acc      += e0.rgb * g;
            wsumLit  += e0.a   * g;

            // Segments are gathered with the raw proximity weight g, NOT the
            // arc-gated one, so a segment lights even on perimeter stretches no
            // arc covers. e1 holds SUM(segColour * bell) and SUM(bell) over
            // every segment, so the old inner loop collapses to one add each.
            segAcc   += e1.rgb * g;
            wsumSegW += e1.a   * g;

            wsumAll  += g;
        }
    } else {
        // No segments: row 1 is all zeros, so the fetch and the two adds it
        // feeds are dropped. Everything else is the body above, line for line.
        for (int i = 0; i < n; i++) {
            vec2  dv  = p - uLoopSamples[i].xy;
            float dd  = dot(dv, dv);

            float g   = 1.0 / (dd + kc2);

            vec4 e0 = texelFetch(uEmission, ivec2(i, 0), 0);

            acc      += e0.rgb * g;
            wsumLit  += e0.a   * g;
            wsumAll  += g;
        }
    }

    PerimeterGather result;
    // Both are pure hues of unit magnitude now; neon.frag attaches the
    // magnitudes from the pointwise coverages.
    result.hue    = acc    / max(wsumLit,  WSUM_EPSILON);
    result.segHue = segAcc / max(wsumSegW, WSUM_EPSILON);

    // The glow's two coverages: each SUM(cover * g) over SUM(g), the g-weighted
    // mean of that coverage over the perimeter. Why the glow is scaled by a
    // gathered coverage rather than the pointwise one is neon.frag's
    // glow-coverage block.
    //
    // Two divides rather than one reciprocal and two multiplies: a fully lit
    // ring rests on wsumLit / wsumAll being exactly 1.0 when the two sums are
    // equal, and x * (1.0 / x) is not. Outside the loop, so it costs one extra
    // divide per fragment, not per sample.
    float wsumDen   = max(wsumAll, WSUM_EPSILON);
    result.arcCover = wsumLit  / wsumDen; // arc coverage x intensity
    result.segCover = wsumSegW / wsumDen; // segment boost x bell
    return result;
}
#endif
