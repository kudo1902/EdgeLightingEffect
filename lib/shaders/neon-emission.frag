precision highp float;

// ---------------------------------------------------------------------------
// Perimeter emission pre-pass.
//
// Bakes the FRAGMENT-INVARIANT half of the neon gather loop into a 1-D
// texture: one texel per perimeter sample, NEON_MAX_LOOP_SAMPLES wide, 1 tall.
//
// Why this pass exists
// --------------------
// neon.frag's gather loop used to run, for every sample and every screen
// fragment: the arc winner-take-all search (uArcCount x arcInside, four
// smoothsteps each), the travelling-segment loop (uSegmentCount x exp()), and
// one to two gradient-LUT fetches. None of that depends on the fragment - the
// sample's perimeter position si is i / N, and every colour/mask term is a
// pure function of (si, uTime, config). So a 1920x1080 draw was recomputing
// the identical 128-entry table millions of times.
//
// Here it is computed once per frame, in NEON_MAX_LOOP_SAMPLES fragments. The
// main shader's per-sample cost collapses to one texelFetch plus the distance
// maths that genuinely varies per fragment.
//
// Output packing (see neon.frag's gather loop for the consumer):
//   .rgb = arcColour * arcMask * uIntensity + SUM(segColour * bell)
//          i.e. the fully composed light colour at this sample, PREMULTIPLIED
//          by its own weight. Both halves accumulate against the same gather
//          weight g downstream, so folding uIntensity in here is exact and
//          saves carrying two colour accumulators through the loop.
//   .a   = cover = max(arcMask, min(SUM(bell), 1)) - drives halo + bloom.
//
// .rgb can exceed 1 (SegmentBoost::boost is an absolute peak brightness and
// several segments can stack), so the target is a float format - the renderer
// asks for RGBA16F and falls back to RGBA8 only if the driver refuses it.
//
// The filament gates are NOT baked here: they are read at the fragment's own
// continuous perimeter position in neon.frag, not at the gather samples.
// ---------------------------------------------------------------------------

out vec4 fragColor;

uniform float uTime;
uniform float uHueRotationRate;
uniform float uIntensity;
uniform float uPerimeter; ///< Current perimeter in px; converts the pixel-space arc feather to a perimeter fraction.
/// Gather sample count actually in use, 1..NEON_MAX_LOOP_SAMPLES. Sample i
/// sits at perimeter position i / uNumSamples, matching how the CPU walks
/// GetPointOnRectangle when it fills LoopSamplesBlock. NeonRenderer always
/// passes NEON_MAX_LOOP_SAMPLES; NeonOptimizedRenderer passes its slider
/// value, and the texels past it are written but never read.
uniform int uNumSamples;

// Same std140 blocks the main shader binds - see neon.frag for the layout
// rationale and the meaning of each packed component.
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

uniform sampler2D uGradientLUT;
uniform sampler2D uSegmentLUT;
uniform sampler2D uArcLUT;

// Fractional [0, 1] contribution of the sample at @p si to the lit arc.
//
// This is a BOX RESAMPLE, not a feather: the weight is the fraction of the
// sample's own cell (width @p w, CENTRED on si) that the arc [start, end]
// covers. That is the exact midpoint-rule weight for the line integral the
// gather loop performs downstream, and it is what keeps a slowly animating
// endpoint smooth. Three properties matter, and a smoothstep ramp has none of
// them:
//
//   1. Uniform speed. The emitted mass is exactly linear in the endpoint
//      position, so the halo's visible end tracks the arc end at constant
//      speed. A smoothstep ramp one cell wide makes that end lurch between
//      0.2x and 1.9x of the true speed, once per gather sample - invisible on
//      a fast sweep, obvious stepping on a slow one (a 2 s collapse crosses
//      NEON_MAX_LOOP_SAMPLES cells, a 20 s collapse crosses the same cells ten
//      times slower). The cell must be at least 2 samples wide for this:
//      at exactly 1 sample only one weight is ever in transition and the
//      partition-of-unity that makes the sum linear is lost.
//   2. Fade to nothing. An arc shorter than one cell weighs length / w, so a
//      collapsing arc dims out. Outward feathers instead floor the lit span at
//      (fHead + fTail) and hold it at full brightness until the length
//      early-out below snaps it off - a pop at the end of every collapse.
//   3. Unbiased ends. The cell is centred, so weight 0.5 lands exactly on the
//      arc endpoint and the lit span still measures `length`. Outward feathers
//      overstate it, inward feathers understate it.
//
// Wrap-aware via testing both @p si and @p si + 1: when the arc extends past
// 1.0 a sample near position 0 is physically close to the end via the
// perimeter loop. The two are SUMMED (then clamped) rather than max'd so that
// an arc closing on a full ring closes its seam continuously - with max() the
// seam sample would top out at 0.5 and a nearly-full ring would show a dark
// notch that pops shut at length == 1. Only one term is ever nonzero unless
// the arc is within one cell of a full ring, so the sum is the max elsewhere.
float arcInside(float si, float start, float length, float w) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 0.0)        return 0.0;   // empty
    float end   = start + length;
    float halfW = 0.5 * w;
    float invW  = 1.0 / max(w, 1e-6);
    float ca = (min(si       + halfW, end) - max(si       - halfW, start)) * invW;
    float cb = (min(si + 1.0 + halfW, end) - max(si + 1.0 - halfW, start)) * invW;
    return clamp(max(ca, 0.0) + max(cb, 0.0), 0.0, 1.0);
}

void main() {
    float invNumSamples = 1.0 / float(max(uNumSamples, 1));

    // One fragment per sample; gl_FragCoord.x is the sample index + 0.5.
    float si = floor(gl_FragCoord.x) * invNumSamples;

    // Base-gradient coordinate. Negating the time term makes a positive
    // hueRotationRate scroll the colours WITH the winding (si advances in the
    // winding direction); the REPEAT-wrapped LUT handles the negative value.
    float ti = si - uTime * uHueRotationRate;

    // Resample cell width in perimeter fractions. Pixel-space so the arc ends
    // soften like the filament's HEAD_FEATHER_PX on any geometry, but floored
    // at TWO samples: below that the weights stop summing linearly in the
    // endpoint position and a slowly moving arc end steps (see arcInside).
    float cellW = max(HEAD_FEATHER_PX / max(uPerimeter, 1.0), 2.0 * invNumSamples);

    // --- Arc winner-take-all ---------------------------------------------
    // The arc with the largest effective mask (arcInside * intensity) at this
    // sample owns both the emission mask and the colour. Because arcInside is
    // smoothstepped at each end, adjacent arcs of different colours crossfade
    // at the seam rather than snapping.
    float bestMask = 0.0;
    int   bestIdx  = -1;
    for (int a = 0; a < uArcCount; a++) {
        vec4  arc  = uArcs[a];
        float mask = arcInside(si, arc.x, arc.y, cellW) * arc.z;
        if (mask > bestMask) {
            bestMask = mask;
            bestIdx  = a;
        }
    }
    float arcW = bestMask;

    // Winner's colour. Two cases:
    //  - hasStops: the arc has its own gradient. Sample it in ARC-LOCAL space
    //    so position 0 is the arc's start and position 1 its end. The uTime
    //    term still scrolls the LUT (REPEAT wrap) at the global rate, so hue
    //    rotation reads as the gradient marching through the arc window rather
    //    than as position offsetting.
    //  - empty stops: fall back to the base gradient IN PERIMETER SPACE so the
    //    arc stays visually continuous with the rest of the perimeter.
    // bestIdx < 0 means no arc covers this sample - it contributes no arc
    // emission, but a stop-less segment there still inherits the base gradient.
    vec3 arcCol;
    vec3 segFallback;
    if (bestIdx >= 0) {
        vec4 winner = uArcs[bestIdx];
        if (winner.w > 0.5) {
            float rowY = (float(bestIdx) + 0.5) / float(MAX_ARCS);
            float uArc = (si - winner.x) / max(winner.y, 1e-4);
            uArc      -= uTime * uHueRotationRate; // match base sign convention
            arcCol     = texture(uArcLUT, vec2(uArc, rowY)).rgb;
        } else {
            arcCol = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
        }
        segFallback = arcCol;
    } else {
        arcCol      = vec3(0.0);
        segFallback = texture(uGradientLUT, vec2(ti, 0.5)).rgb;
    }

    // --- Travelling segments (independent additive lights) ----------------
    // Composed outside uIntensity so a segment stays lit even at intensity 0 -
    // the whole point of the additive segment model. Skipped entirely when
    // uSegmentCount == 0.
    vec3  segSum  = vec3(0.0);
    float segMask = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg = uSegments[s];
        // Signed wrap-distance along the perimeter in [-0.5, 0.5]: magnitude
        // drives the bell, sign picks the spot in the segment's own gradient.
        float rel = si - seg.x;
        rel      -= floor(rel + 0.5);
        float e    = rel * seg.y;             // normalise by invSigma
        float bell = seg.z * exp(-e * e);     // boost * gaussian
        if (bell < 0.005) continue;           // distant segment: contributes nothing

        vec3 segColor;
        if (seg.w > 0.5) {
            // tLocal: 0 at the segment head, 1 at its tail. e already
            // normalises rel by invSigma, so just clamp/rescale.
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            segColor     = texture(uSegmentLUT, vec2(tLocal, rowY)).rgb;
        } else {
            segColor = segFallback;
        }
        segSum  += segColor * bell;
        segMask += bell;
    }

    // Shared coverage: arc mask OR segment coverage (clamped so stacked
    // segments can't push the halo/bloom past a single light's reach).
    float cover = max(arcW, min(segMask, 1.0));

    fragColor = vec4(arcCol * (arcW * uIntensity) + segSum, cover);
}
