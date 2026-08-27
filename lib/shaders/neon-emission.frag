precision highp float;

// ---------------------------------------------------------------------------
// Perimeter emission pre-pass.
//
// Bakes the FRAGMENT-INVARIANT half of the neon gather loop into a small
// texture: NEON_MAX_LOOP_SAMPLES wide, 2 tall.
//
// Why this pass exists
// --------------------
// The gather loop in neon.frag / neon-optimized.frag runs, for every sample
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
// arc / segment coverages, the filament gate, and the colour-stop alpha. Those
// are read at sPos in the main shader, not at the gather samples, and baking
// them at sample resolution would reintroduce the quantisation their pointwise
// evaluation exists to avoid.
// ---------------------------------------------------------------------------

out vec4 fragColor;

uniform float uTime;
uniform float uHueRotationRate;
uniform int   uNumSamples; ///< Samples in use; 1..NEON_MAX_LOOP_SAMPLES. Texels past it are written but never read.

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

// Verbatim from the gather it replaces - the feather widths buy hue-blend
// behaviour, so changing them here changes the ring's colour hand-over. Keep
// in step with the copies in neon.frag / neon-optimized.frag.
float arcInside(float si, float start, float length, float invNumSamples) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float fHead = invNumSamples;
    float fTail = 0.25 * invNumSamples;
    float end = start + length;
    float g1a = smoothstep(start - fTail, start, si);
    float g2a = 1.0 - smoothstep(end, end + fHead, si);
    float g1b = smoothstep(start - fTail, start, si + 1.0);
    float g2b = 1.0 - smoothstep(end, end + fHead, si + 1.0);
    return max(g1a * g2a, g1b * g2b);
}

void main() {
    // gl_FragCoord.x is the sample index, .y picks the row. si is computed
    // directly rather than accumulated as the old loop's `si += dti` chain
    // did, so it carries no drift across N iterations.
    float invNumSamples = 1.0 / float(max(uNumSamples, 1));
    float si = floor(gl_FragCoord.x) * invNumSamples;
    float ti = si - uTime * uHueRotationRate;

    // --- Arc winner-take-all, moved verbatim from the gather ---------------
    float bestMask = 0.0;
    int   bestIdx  = -1;
    for (int a = 0; a < uArcCount; a++) {
        vec4  arc  = uArcs[a];
        float mask = arcInside(si, arc.x, arc.y, invNumSamples) * arc.z;
        if (mask > bestMask) {
            bestMask = mask;
            bestIdx  = a;
        }
    }
    float arcW = bestMask;

    // Winner's colour: arc-local for hasStops, perimeter space otherwise.
    // segFallback is what a stop-less segment inherits - the arc's colour
    // where an arc covers, the base gradient where none does.
    vec3 baseColI;
    vec3 segFallback;
    if (bestIdx >= 0) {
        vec4 winner = uArcs[bestIdx];
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
            // alpha read. arcInside above already covers a seam-straddling arc
            // (it tests si and si + 1.0), so an unwrapped si - start went
            // negative past the seam and CLAMP_TO_EDGE pinned the wrapped
            // remainder to the head colour while the arc stayed lit.
            float rowY = (float(bestIdx) + 0.5) / float(MAX_ARCS);
            float rel  = si - winner.x;
            rel       -= floor(rel);                          // wrap to [0, 1)
            if (rel > 0.5 * (1.0 + winner.y)) { rel -= 1.0; } // behind the start, not past the head
            float uArc = rel / max(winner.y, 1e-4);
            baseColI   = texture(uArcLUT, vec2(uArc, rowY)).rgb;
        } else {
            baseColI = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
        }
        segFallback = baseColI;
    } else {
        baseColI    = vec3(0.0);
        segFallback = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
    }

    // --- Row 0: the arc term, premultiplied by its own gather weight -------
    if (gl_FragCoord.y < 1.0) {
        fragColor = vec4(baseColI * arcW, arcW);
        return;
    }

    // --- Row 1: the segment term -------------------------------------------
    // Summed over segments here so the consumer sees one vec4 per sample
    // regardless of uSegmentCount - that sum is the whole point of the pass.
    vec3  segSum  = vec3(0.0);
    float bellSum = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg  = uSegments[s];
        float rel  = si - seg.x;
        rel       -= floor(rel + 0.5);              // wrap to [-0.5, 0.5]
        float e    = rel * seg.y;                   // normalise by invSigma
        float bell = seg.z * exp(-e * e);           // boost * gaussian
        if (bell < 0.005) continue;                 // same early-out as the old loop

        vec3 segColor;
        if (seg.w > 0.5) {
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            segColor     = texture(uSegmentLUT, vec2(tLocal, rowY)).rgb;
        } else {
            segColor = segFallback;
        }
        segSum  += segColor * bell;
        bellSum += bell;
    }
    fragColor = vec4(segSum, bellSum);
}
