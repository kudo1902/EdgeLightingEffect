precision highp float;

// ---------------------------------------------------------------------------
// Perimeter emission pre-pass.
//
// Bakes the FRAGMENT-INVARIANT half of the neon gather loop into a small
// texture: NEON_MAX_LOOP_SAMPLES wide, 2 tall.
//
// Why this pass exists
// --------------------
// The gather loop in neon.frag runs, for every sample
// AND every screen fragment: the arc winner-take-all search (uArcCount x
// arcInside, four smoothsteps each), the travelling-segment loop
// (uSegmentCount x exp()), and one to two filtered LUT fetches. None of that
// depends on the fragment - the sample's perimeter position si is i / N, and
// every colour / mask term is a pure function of (si, uTime, config). So a
// full-viewport draw was recomputing the identical N-entry table once per
// fragment.
//
// Here it is computed once per frame in 2N fragments, and the consumer's
// per-sample cost collapses to two texelFetches plus the distance maths that
// genuinely does vary per fragment. Per-fragment cost stops scaling with the
// number of arcs and segments.
//
// Why TWO rows and not one
// ------------------------
// The consumer keeps two independently normalised accumulators:
//
//     col       = SUM(baseColI * arcW * g) / SUM(arcW * g)      <- arc hue
//     segColHue = SUM(segColour * bell * g) / SUM(bell * g)     <- segment hue
//
// That GATED normalisation is deliberate (it makes each hue a unit-magnitude
// value carrying neither coverage nor intensity - see neon.frag), but it means
// the two colour terms ride DIFFERENT weights: arcW * g for the arc, bell * g
// for the segments. They therefore cannot be folded into a single
// premultiplied colour the way they could under one shared weight - each needs
// its own numerator AND its own denominator, which is 8 floats:
//
//   row 0 (y = 0):  .rgb = baseColI * arcW      .a = arcW
//   row 1 (y = 1):  .rgb = SUM(segColour*bell)  .a = SUM(bell)
//
// Downstream, `acc += row0.rgb * g` and `wsumLit += row0.a * g` reproduce the
// old arc accumulation exactly, and likewise row 1 for the segments.
//
// THE .a WEIGHTS ARE ALSO THE GLOW'S COVERAGE, and that decides what goes in
// them. Since V14 (docs/review-findings.md) the gather divides each piece's
// SUM(.a * g) by its SUM(g) and scales that piece's halo and bloom by the
// result, so these two channels are no longer only hue weights. Three things
// follow, each of which was a defect while they were only hue weights:
//
//   - COLOUR-STOP ALPHA IS IN THEM. The hue is normalised by the same weight it
//     is gathered with, so alpha cancels out of it and costs it nothing; the
//     coverage is exactly the thing alpha is meant to scale. Left out, an
//     alpha-0 stretch kept its full halo and bloom (V15). The LUTs store
//     STRAIGHT alpha, so it has to be multiplied in here - it is never in the
//     colour.
//   - EACH SAMPLE CARRIES ITS CELL'S MEAN, NOT ITS CENTRE'S VALUE. A sample
//     stands for the stretch of perimeter half a spacing either side of it,
//     and an arc end or a narrow bell point-sampled at the centre flips as it
//     crosses one: a rotating arc's glow ticked by ~20 levels per sample and a
//     0.005-long segment's swung from 6 to 86 (V17). The cell mean is
//     continuous in the light's position, so the sum over samples is too.
//   - A CELL NEVER CROSSES INTO ANOTHER EMITTER PIECE. The gather keeps one
//     coverage per piece, and a cell that straddled a boundary would spend a
//     neighbour's light as this piece's: a 40 px side that owns two samples
//     reads half lit if one of them picks up the arc ending round the corner.
//     So the first and last cell of each piece are cut at its ends - and
//     stretched to them, so a piece's cells tile it exactly.
//
// uIntensity is deliberately NOT folded in here (the reference implementation
// on improve_neon_by_emission_pre_pass did fold it). It cancels out of the
// gated normalisation anyway, and reaches the emission through emitCover /
// filamentGate, which are pointwise and size-invariant.
//
// Both rows can exceed 1.0 - SegmentBoost::boost is an absolute peak
// brightness and several segments can stack, and Arc::intensity is unbounded -
// so the target wants a float format. The renderers ask for RGBA16F and fall
// back to RGBA8 with a warning where the driver refuses it.
//
// What is NOT baked here
// ----------------------
// Anything that reads the FRAGMENT's own perimeter position: the continuous
// arc / segment coverages, the filament gate, the filament's colour and the
// colour-stop alpha that gates it. Those are read at sPos in the main shader,
// not at the gather samples, and baking them at sample resolution would
// reintroduce the quantisation their pointwise evaluation exists to avoid. The
// alpha above is the same alpha, sampled coarsely for the glow, which is wide
// enough not to care.
// ---------------------------------------------------------------------------

out vec4 fragColor;

uniform float uTime;
uniform float uHueRotationRate;
uniform int   uNumSamples; ///< Samples in use; 1..NEON_MAX_LOOP_SAMPLES. Texels past it are written but never read. Also sets each sample's cell width.

// Same three LUT atlases the gather used to sample, bound to the same units.
// Reading them here rather than re-deriving the colours on the CPU is what
// makes the result exact by construction: a CPU bake would have to reproduce
// bilinear REPEAT filtering and could drift.
uniform sampler2D uGradientLUT;
uniform sampler2D uSegmentLUT;
uniform sampler2D uArcLUT;

layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// One entry per emitter piece, packed in NeonRenderer::packLightBlockData:
// .x / .y = the piece's perimeter span [start, end) as fractions, .z = the
// index of its first loop sample, .w = how many it owns. The same block
// neon.frag walks its per-piece gather with. Position 0 is always a piece
// boundary, so a span never wraps and a run of samples never does either.
// This pass reads only uPieces; the other two arrays narrow each piece to its
// lit extent for neon.frag's field, and are declared here only so the two
// programs agree on the block's layout.
layout(std140) uniform PieceBlock
{
    vec4 uPieces[NEON_EMITTER_PIECES];
    vec4 uPieceLit[NEON_EMITTER_PIECES];
    vec4 uPieceWhole[NEON_EMITTER_PIECES];
};

// Length of perimeter [lo, hi] lies under the arc [start, start + length].
//
// Every cell lies within [0, 1] - the first and last are cut at piece ends,
// and 0 and 1 are piece ends - so the arc and its copy one ring back are the
// only two that can reach it. A full arc covers the cell whatever its start.
float arcOverlap(float lo, float hi, float start, float length) {
    if (length >= 1.0 - 1e-6) return hi - lo;
    if (length <= 1e-6)       return 0.0;
    float a0 = start - floor(start);
    return max(min(hi, a0 + length)       - max(lo, a0),       0.0) +
           max(min(hi, a0 + length - 1.0) - max(lo, a0 - 1.0), 0.0);
}

// erf, Abramowitz & Stegun 7.1.26: max absolute error 1.5e-7, which is below
// what the RGBA16F table keeps. GLSL has no erf of its own.
float erfApprox(float x) {
    float t = 1.0 / (1.0 + 0.3275911 * abs(x));
    float p = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t
                 - 0.284496736) * t + 0.254829592) * t;
    return sign(x) * (1.0 - p * exp(-x * x));
}

// Mean of exp(-(u * invSigma)^2) over u in [a, b], a cell of length `len`
// (b - a, passed in because the caller already has it). The integral is
// (sqrt(PI) / 2) / invSigma * (erf(b * invSigma) - erf(a * invSigma)).
float bellCellMean(float a, float b, float invSigma, float len) {
    return 0.8862269255 * (erfApprox(b * invSigma) - erfApprox(a * invSigma)) /
           (invSigma * len);
}

void main() {
    // gl_FragCoord.x is the sample index, .y picks the row. si is computed
    // directly rather than accumulated as the old loop's `si += dti` chain
    // did, so it carries no drift across N iterations.
    int   i             = int(floor(gl_FragCoord.x));
    float invNumSamples = 1.0 / float(max(uNumSamples, 1));
    float si            = float(i) * invNumSamples;
    float ti            = si - uTime * uHueRotationRate;

    // --- The stretch of perimeter this sample stands for ----------------
    // Half a spacing either side, except at the ends of its piece: the first
    // and last cell of a piece reach exactly to its ends, so the piece's cells
    // tile it and no cell spends a neighbouring piece's light. A piece that
    // owns no sample (a corner shorter than a spacing) is skipped - its
    // coverage is borrowed from its neighbours in neon.frag instead.
    float lo = si - 0.5 * invNumSamples;
    float hi = si + 0.5 * invNumSamples;
    for (int k = 0; k < NEON_EMITTER_PIECES; k++) {
        vec4 pc = uPieces[k];
        if (pc.w < 0.5) continue;
        int first = int(pc.z + 0.5);
        int last  = first + int(pc.w + 0.5) - 1;
        if (i == first) { lo = pc.x; }
        if (i == last)  { hi = pc.y; }
    }
    float len = max(hi - lo, 1e-7);

    // --- Arc coverage of the cell, winner-take-all ----------------------
    // The documented overlap rule is winner-take-all: where two arcs cover the
    // same perimeter, the brighter one's intensity holds, not their sum. Over a
    // cell the exact form would need the max-envelope of the arcs, so this
    // takes the SUM of the overlaps capped at the brightest arc's intensity
    // over the whole cell - exact for the two cases that matter. Arcs that tile
    // (one ends where the next starts) never overlap, so their sum is the
    // cell's coverage, including across a seam inside the cell. Arcs stacked
    // on top of each other saturate the cap and read as the brightest of them.
    float arcSum  = 0.0;
    float arcTop  = 0.0;
    float bestM   = 0.0;
    int   bestIdx = -1;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                   // dark arc: covers nothing
        float ov = arcOverlap(lo, hi, arc.x, arc.y);
        if (ov <= 0.0) continue;
        float m = ov * arc.z;
        arcSum += m;
        arcTop  = max(arcTop, arc.z);
        if (m > bestM) {
            bestM   = m;
            bestIdx = a;
        }
    }
    // Exactly 1.0 for a full arc at intensity 1 (len / len), which is what
    // keeps a fully lit ring's glow unchanged by any of this.
    float arcW = min(arcSum, arcTop * len) / len;

    // Winner's colour and alpha: arc-local for hasStops, perimeter space
    // otherwise. segFallback is what a stop-less segment inherits - the arc's
    // colour where an arc covers, the base gradient where none does.
    vec4 ring = texture(uGradientLUT, vec2(ti, 0.5));
    vec3  baseColI;
    float baseA;
    vec3  segFallback;
    float fallA;
    if (bestIdx >= 0) {
        vec4 winner = uArcs[bestIdx];
        vec4 c4;
        // .w is a bitmask (bit 0 = hasStops, bits 1-2 = abutment, used only by
        // the consumer's coverage feather) - test bit 0, not the whole value.
        if (mod(winner.w, 2.0) >= 0.5) {
            // No hue-rotation term: uArc is the arc's own head-to-tail
            // coordinate, not a perimeter position. The consumer's alpha read
            // in neon.frag must agree exactly or colour and alpha come from
            // different texels of the same row. See neon.frag for why the term
            // was wrong here in the first place.
            //
            // WRAPPED, and by the same expression the consumer uses for its
            // alpha read. The cell may reach across the seam (arcOverlap tests
            // the arc's copy one ring back), so an unwrapped si - start went
            // negative past the seam and CLAMP_TO_EDGE pinned the wrapped
            // remainder to the head colour while the arc stayed lit.
            float rowY = (float(bestIdx) + 0.5) / float(MAX_ARCS);
            float rel  = si - winner.x;
            rel       -= floor(rel);                          // wrap to [0, 1)
            if (rel > 0.5 * (1.0 + winner.y)) { rel -= 1.0; } // behind the start, not past the head
            float uArc = rel / max(winner.y, 1e-4);
            c4         = texture(uArcLUT, vec2(uArc, rowY));
        } else {
            c4 = ring;
        }
        baseColI    = c4.rgb;
        baseA       = c4.a;
        segFallback = c4.rgb;
        fallA       = c4.a;
    } else {
        baseColI    = vec3(0.0);
        baseA       = 0.0;
        segFallback = ring.rgb;
        fallA       = ring.a;
    }

    // --- Row 0: the arc term, premultiplied by its own gather weight -------
    // The weight is coverage x alpha; see the header for why alpha is here.
    if (gl_FragCoord.y < 1.0) {
        float w = arcW * baseA;
        fragColor = vec4(baseColI * w, w);
        return;
    }

    // --- Row 1: the segment term -------------------------------------------
    // Summed over segments here so the consumer sees one vec4 per sample
    // regardless of uSegmentCount - that sum is the whole point of the pass.
    // Each bell is its MEAN over the cell (bellCellMean), so a segment narrower
    // than a spacing moves smoothly between samples instead of being read at
    // its peak on one and its flank on the next. For a bell many spacings wide
    // the mean and the centre value agree to well under an 8-bit level.
    //
    // LINEAR in the bell. The segments' glow also carries a reach bound,
    // max(arc, min(segment, 1)), and it is deliberately NOT applied here per
    // sample but by neon.frag after it has averaged these weights - where it
    // shapes how the glow feathers off past a segment's ends. See the glow
    // block there.
    vec3  segSum  = vec3(0.0);
    float bellSum = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg  = uSegments[s];
        float rel  = si - seg.x;
        rel       -= floor(rel + 0.5);             // wrap to [-0.5, 0.5]
        float e    = rel * seg.y;                  // normalise by invSigma
        float bell = seg.z * bellCellMean(rel - (si - lo), rel + (hi - si), seg.y, len);
        if (bell < 0.005) continue;                 // same early-out as the old loop

        vec4 c4;
        if (seg.w > 0.5) {
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            c4           = texture(uSegmentLUT, vec2(tLocal, rowY));
        } else {
            c4 = vec4(segFallback, fallA);
        }
        bell    *= c4.a;
        segSum  += c4.rgb * bell;
        bellSum += bell;
    }
    fragColor = vec4(segSum, bellSum);
}
