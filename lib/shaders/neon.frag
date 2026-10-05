// precision, vPos, uRectSize / uCornerRadius, the segment block, PI and the
// perimeter gather itself (gatherPerimeter) are in neon-common.glsl, which is
// injected ahead of this file - neon-gather.frag runs the same loop.

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

// All other tuning constants (FILAMENT_*, HALO_*, BLOOM_*, grading, epsilons)
// are injected from lib/include/renderer/neon-tuning.h via @NEON_TUNING@ in
// shaders.h.in - single source of truth shared with the C++ renderer.
//
// (Far culling lives on the CPU: the draw quad is sized to rect + glowReach,
//  so there's no per-fragment discard here. See neon-renderer.cpp.)

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

// TWO VARIANTS OF THIS FILE. NeonRenderer splices a #define in after the
// version line (WithDefine) for one of them; the direct path compiles it
// without.
//
//   (none)            - resolutionScale 1.0: gather and shade, onto the target.
//   NEON_READS_GATHER - below 1.0, everything EXCEPT the gather: reads the
//                       four results neon-gather.frag stored back with a
//                       bilinear fetch. Built into TWO program objects, one
//                       per target: pass 1 (at resolutionScale, into the
//                       reduced buffer) and the edge ring (at full resolution,
//                       onto the target) - see NeonRenderer::ensurePathPrograms.
//
// The scaled path's third program, the gather pass, is neon-gather.frag: the
// same gatherPerimeter, alone, into the gather buffer.
out vec4 fragColor;

uniform float uLineWidth;
uniform float uFilamentFalloff; ///< Generalized-Gaussian exponent (N = value * 2); 1.0 = pure Gaussian, lower = smoother (Laplace-like), higher = flatter top.
uniform float uIntensity;
uniform float uTime;
uniform float uHueRotationRate;
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;          ///< Cutoff::size of the inside cutoff: positive px distance INSIDE the rect edge where its fade STARTS. Disabled sides collapse to a huge sentinel CPU-side so this branch no-ops.
uniform float uInsideCutoffSoftness;  ///< Feather width in px, running on from uInsideCutoff toward the centre.
uniform float uOutsideCutoff;         ///< Cutoff::size of the outside cutoff: positive px distance OUTSIDE the rect edge where its fade STARTS. Disabled sides collapse to a huge sentinel CPU-side.
uniform float uOutsideCutoffSoftness; ///< Feather width in px, running on from uOutsideCutoff away from the rect.
uniform int   uWinding;               ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE (matches Winding enum).

// Ratio between the buffer this pass is drawing into and the viewport the
// caller sees: 1.0 when the gather runs at full resolution straight onto the
// caller's framebuffer, NeonConfig::resolutionScale when it runs into the
// scaled offscreen buffer that gets blitted back.
//
// Every pixel-valued uniform above ALREADY arrives pre-multiplied by it - the
// renderer scales them once on the CPU. This exists for the other direction:
// the px constants baked in from neon-tuning.h (FILAMENT_MIN_HALF_WIDTH,
// HEAD/TAIL_FEATHER_PX, GLOW_GATE_FADE_PX) are written in full-res px, so each
// use below converts it into the space everything else is in. Three sites, all
// marked; forgetting one makes a feather or a gate change width with the
// resolution scale, which reads as the scaled path "looking different" rather
// than as a bug.
uniform float uResolutionScale;

// The segment block (uSegmentCount, uSegments) is in neon-common.glsl - the
// gather branches on it too.

// Per-segment gradient atlas (RGBA8, CLAMP-wrapped both axes). One row per
// segment, laid out head-to-tail across the segment's visible span. Sampled
// only when the segment's hasStops flag is set (see uSegments.w above).
uniform sampler2D uSegmentLUT;

// Arc gating - up to MAX_ARCS independent perimeter slices, each with its own
// start, length, intensity, and optional colour stops. Each vec4 is
// (start, length, intensity, hasStops). Overlap resolves winner-take-all:
// per sample, the arc with the largest effective mask (arcInside * intensity)
// contributes its colour and its mask to the emission. Because arcInside is
// smoothstepped 1-sample-wide at each end, adjacent arcs of different colours
// crossfade at the seam rather than snapping.
//
// When uArcCount == 0 the entire perimeter is dark; the default config seeds
// one full-perimeter arc so this only happens if the host wipes the vector.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// Per-arc gradient atlas (RGBA8, CLAMP-wrapped both axes). One row per arc,
// same layout convention as uSegmentLUT: a head-to-tail SPAN, baked with
// ColorUtils::SampleSpan, so the end colours hold rather than wrapping round.
//
// Read in TWO places, and the second is easy to miss. The pre-pass samples
// only the WINNING arc's row, for the gather hue. But the pointwise emitCover
// loop in main() samples the row of EVERY arc that covers this fragment, for
// its colour-stop alpha - so a row is live whenever its arc has stops, and the
// non-winning rows cannot be left stale. SpanAtlasLUT::Bake zero-fills the
// whole atlas on every bake (mAtlas.assign, not resize, for exactly this
// reason), which is what currently keeps that true.
uniform sampler2D uArcLUT;

// 1-row 2D LUT (REPEAT-wrapped) holding the precomputed colour ring.
// Replaces the in-shader sampleStops loop + HSV blend on the hot path.
// GLES 3.0 does not support sampler1D, so we use a 1-row 2D texture.
uniform sampler2D uGradientLUT;

// The glow coverage table (neon-glow-cover.frag, baked once per config change):
// for each piece of the emitter and each fragment position round it, how lit
// that piece is as the halo and the bloom see it, encoded c / (1 + c). Read by
// glowCoverAt, one linear fetch per piece; laid out in neon-pieces.glsl.
uniform sampler2D uGlowCover;

// How each band of that table shares its columns between its straight and its
// corner - the same numbers the bake was given. See glowCoverInner.
uniform vec2 uGlowCoverSplit;

// The gather's own inputs - the sample block, uNumSamples and the emission
// table uEmission - are in neon-common.glsl, and do not exist in the
// NEON_READS_GATHER variant: it reads the gather's RESULT instead of running
// it, so it has no loop to feed.

#ifdef NEON_READS_GATHER
// Below resolutionScale 1.0 the gather - the loop that is ~95% of this
// shader's cost - runs ONCE, in its own pass, at its own coarse scale
// (neon-gather.frag, into the gather buffer), and this variant draws
// everything else from its stored result: pass 1 at resolutionScale, and the
// edge ring at FULL resolution, where it redraws everything the reduced pass
// gets wrong near the line - the filament, which a reduced buffer cannot
// sample, and every hard edge. The gather's four results are smooth across the
// screen - Lorentzian-weighted means over the whole perimeter, whose kernel is
// never narrower than kc - so they survive a bilinear read from a grid a
// fraction of kc apart (GetGatherScale in neon-renderer.cpp). Everything else
// below is computed exactly as the direct path computes it, at whatever
// uResolutionScale the pass uploads. See docs/neon-resolution-scale-plan.md
// step 5 and section 13.
//
// uGather / uGatherSeg are the gather buffer's attachments 0 and 1. uGatherSeg
// is bound only when there are segments, and read only then. The buffer covers
// a box around the rect, clipped to the viewport (GetBufferRegion), so vPos
// maps onto its uv through an affine map - the same form neon-blit.frag
// samples the reduced buffer with - set for whichever space this pass's vPos
// is in.
uniform sampler2D uGather;
uniform sampler2D uGatherSeg;
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;
#endif

// Distance (in pixels, from the rect edge) to the draw quad's edge. The whole
// emission is faded to zero just before this, so the bloom never shows a hard
// rectangular cutoff where the quad clips it - independent of bloom strength.
uniform float uQuadMargin;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// PI, TWO_PI and HALF_PI are in neon-common.glsl.

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// --- Finite-segment halo / bloom ---------------------------------------
// The halo and bloom kernels integrated along a STRAIGHT SEGMENT rather than
// an infinite line: `a` is the perpendicular distance from the fragment to the
// segment's line, and t1/t2 are the segment's endpoints measured along it from
// the fragment's foot of perpendicular. Both integrands have elementary
// antiderivatives, and as t1 -> -inf, t2 -> +inf these reduce to exactly
// 2k^2/(a^2+k^2) and PI*k/sqrt(a^2+k^2) - the infinite-line limits the
// HALO_NORM_FACTOR / BLOOM_NORM_FACTOR calibration was set against. Summing
// them over the rect's four edges is therefore a strict generalisation of the
// single nearest-edge term this replaced, not a retune.
//
// This is what removes the interior medial-axis creases: a fragment on a
// corner diagonal has two edges equally near, and a nearest-distance profile
// counted one of them. See neon-tuning.h's halo block.
float haloSegment(float a, float t1, float t2, float k) {
    float c2 = a * a + k * k;
    return k * k / c2 * (t2 / sqrt(c2 + t2 * t2) - t1 / sqrt(c2 + t1 * t1));
}
float bloomSegment(float a, float t1, float t2, float k) {
    float c = sqrt(a * a + k * k);
    return k / c * (atan(t2 / c) - atan(t1 / c));
}

// The bloom's 1/a tail is heavy enough that it has to be pedestal-subtracted to
// reach zero at the draw quad's edge (see the block that calls this). Each edge
// carries its OWN pedestal - the same segment evaluated at `reach` - so that
// edge's contribution lands on zero at `reach` from it, whatever the other
// three are doing.
//
// One shared pedestal taken from the infinite-line form does not work here,
// which is what the nearest-edge version this replaced used. A finite segment
// is always dimmer than the infinite line it is cut from, so that pedestal
// over-subtracts and clamps the sum to zero early: measured at glowRadius 5,
// the exterior tail ended 300 px out instead of running the full 420+ to the
// quad edge.
float bloomSegmentPedestalled(float a, float t1, float t2, float k, float reach) {
    return max(bloomSegment(a, t1, t2, k) - bloomSegment(reach, t1, t2, k), 0.0);
}

// --- Corner arcs, developed onto their tangent -------------------------
// arcTangentSegment is in neon-pieces.glsl, injected ahead of this file: the
// glow coverage bake (neon-glow-cover.frag) lays each corner's coverage along
// the same line this file integrates the corner's halo and bloom along, so the
// two share one copy.

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

// Exact per-fragment perimeter position: maps this fragment's local-space point
// back to its arc-length parameter t in [0, 1), matching the CPU's
// GeometryUtils::GetPointOnRectangle for BOTH windings (uWinding = 0/1 for
// CLOCKWISE / COUNTER_CLOCKWISE). It replaces the proximity-weighted circular
// mean of the sample angles: near a corner the corner samples' phases wrap
// through 2*pi right into the arc's start, so the mean smears the whole corner
// curve to ~0 and the filament gate lights a corner that an arc starting at 0
// should leave dark. The geometric inverse reads the nearest perimeter point
// directly, so corner and edge fragments get their true positions.
float perimeterPosition(vec2 p) {
    float halfW  = uRectSize.x * 0.5;
    float halfH  = uRectSize.y * 0.5;
    float r      = clamp(uCornerRadius, 0.0, min(halfW, halfH));
    float halfWs = halfW - r;
    float halfHs = halfH - r;
    float ws     = uRectSize.x - 2.0 * r;
    float hs     = uRectSize.y - 2.0 * r;
    float arcLen = PI * r * 0.5;
    float peri   = 2.0 * ws + 2.0 * hs + 4.0 * arcLen;

    // Closest point on the rounded-rect perimeter (inverse rounded-box SDF).
    vec2  b  = vec2(halfWs, halfHs);
    vec2  c  = clamp(p, -b, b);
    vec2  d  = p - c;
    float dl = length(d);
    vec2  cp;
    if (dl > 1e-6)
    {
        cp = c + d * (r / dl);
    }
    else
    {
        // Inside the inner box: project straight along the dominant axis to
        // the nearest edge.
        vec2  e  = b - abs(p);
        float sx = (p.x >= 0.0) ? 1.0 : -1.0;
        float sy = (p.y >= 0.0) ? 1.0 : -1.0;
        cp = (e.x < e.y) ? vec2(sx * halfW, p.y) : vec2(p.x, sy * halfH);
    }

    float ax = abs(cp.x);
    float ay = abs(cp.y);

    // Canonical segment id (0..7 in CW order: top, TR, right, BR, bottom, BL,
    // left, TL) and the traversal progress u in [0, 1] measured in the CW
    // direction. CCW runs the same geometric core with mirrored progress
    // (1 - u) and a CCW segment layout, so both windings stay exact.
    int   seg;
    float u;
    if (ax > halfWs && ay > halfHs)
    {
        // Corner arc. The angle of the offset from the corner centre (radius r)
        // gives the fraction across the quarter-arc.
        float sx = (cp.x >= 0.0) ? 1.0 : -1.0;
        float sy = (cp.y >= 0.0) ? 1.0 : -1.0;
        float th = atan(cp.y - sy * halfHs, cp.x - sx * halfWs);
        if (sx > 0.0 && sy > 0.0)
        {
            seg = 1;                                     // top-right: theta 0..pi/2
            u   = (HALF_PI - th) / HALF_PI;
        }
        else if (sx > 0.0)
        {
            seg = 3;                                     // bottom-right: theta -pi/2..0
            u   = -th / HALF_PI;
        }
        else if (sy < 0.0)
        {
            seg = 5;                                     // bottom-left: theta -pi/2..-pi
            if (th > 0.0) th -= TWO_PI;                  // atan2 hands the left tangency back as +pi
            u = (-HALF_PI - th) / HALF_PI;
        }
        else
        {
            seg = 7;                                     // top-left: theta pi/2..pi
            u   = (PI - th) / HALF_PI;
        }
    }
    else if (ay >= halfHs)
    {
        if (cp.y > 0.0)
        {
            seg = 0;                                     // top edge: left to right
            u   = (cp.x + halfWs) / ws;
        }
        else
        {
            seg = 4;                                     // bottom edge: right to left
            u   = (halfWs - cp.x) / ws;
        }
    }
    else if (ax >= halfWs)
    {
        if (cp.x > 0.0)
        {
            seg = 2;                                     // right edge: top to bottom
            u   = (halfHs - cp.y) / hs;
        }
        else
        {
            seg = 6;                                     // left edge: bottom to top
            u   = (cp.y + halfHs) / hs;
        }
    }
    else
    {
        seg = 0;                                         // degenerate - never hit for r > 0
        u   = 0.0;
    }

    float base;
    float len;
    if (uWinding == 0)
    {
        // Segment starts (cumulative) in CW order: top, TR, right, BR, bottom,
        // BL, left, TL.
        switch (seg)
        {
        case 0: base = 0.0;                                   len = ws;     break;
        case 1: base = ws;                                    len = arcLen; break;
        case 2: base = ws + arcLen;                           len = hs;     break;
        case 3: base = ws + arcLen + hs;                      len = arcLen; break;
        case 4: base = ws + 2.0 * arcLen + hs;                len = ws;     break;
        case 5: base = ws + 2.0 * arcLen + hs + ws;           len = arcLen; break;
        case 6: base = ws + 3.0 * arcLen + hs + ws;           len = hs;     break;
        default: base = peri - arcLen;                        len = arcLen; break;
        }
        return (base + len * u) / peri;
    }

    // Segment starts in CCW order: left, BL, bottom, BR, right, TR, top, TL.
    switch (seg)
    {
    case 0: base = 2.0 * hs + 3.0 * arcLen + ws;              len = ws;     break;
    case 1: base = 2.0 * hs + 2.0 * arcLen + ws;              len = arcLen; break;
    case 2: base = hs + 2.0 * arcLen + ws;                    len = hs;     break;
    case 3: base = hs + arcLen + ws;                          len = arcLen; break;
    case 4: base = hs + arcLen;                               len = ws;     break;
    case 5: base = hs;                                        len = arcLen; break;
    case 6: base = 0.0;                                       len = hs;     break;
    default: base = 2.0 * hs + 3.0 * arcLen + 2.0 * ws;       len = arcLen; break;
    }
    return (base + len * (1.0 - u)) / peri;
}

// NOTE: arcInside() used to live here. It shaped the colour gather only -
// picking the winner-take-all arc per sample and weighting that sample in
// the hue average - so it was a pure function of (si, config) and moved to
// neon-emission.frag with the rest of the per-sample work. The visible
// extent of an arc (filament, halo, bloom) never came from it; that is
// arcCoverContinuous below, whose feather is INWARD and in pixels.

// uArcs[].w is a BITMASK, not a bool - packed CPU-side in packLightBlocks:
//   bit 0 (1) - the arc has its own colour stops (read row `a` of uArcLUT)
//   bit 1 (2) - another arc covers the perimeter immediately BEFORE its start
//   bit 2 (4) - another arc covers the perimeter immediately AFTER its end
// The abutment bits pick each endpoint's feather direction in
// arcCoverContinuous. They are a pure function of the arc set, so they are
// resolved once per frame on the CPU rather than rediscovered per fragment by
// an O(arcs^2) scan. Values are 0..7, all exact in a float.
bool arcHasStops(float flags)  { return mod(flags, 2.0) >= 0.5; }
bool arcTailAbuts(float flags) { return mod(floor(flags * 0.5), 2.0) >= 0.5; }
bool arcHeadAbuts(float flags) { return mod(floor(flags * 0.25), 2.0) >= 0.5; }

// Continuous [0,1] coverage of a fragment for the arc [start, start+length],
// used to gate the sharp SDF filament.
//
// The feather direction is decided PER ENDPOINT, from whether another arc
// takes over there (the @c tailAbuts / @c headAbuts flags, computed on the CPU
// and packed into uArcs[].w - see packLightBlocks):
//
//   FREE endpoint     -> ramp INWARD. Coverage is exactly 0 at the endpoint, so
//                        nothing outside the arc's own span is ever lit.
//   ABUTTING endpoint -> ramp OUTWARD, past the endpoint into the neighbour's
//                        span. Coverage is 1 at the endpoint, so max() over the
//                        two arcs hands over at full brightness with no notch,
//                        and the overlap makes the handover smooth rather than
//                        a hard step when the two carry different intensities.
//
// Both halves are needed, and a straddling feather (0.5 at every endpoint) is
// NOT an acceptable shortcut for either:
//
//   - Feathering inward everywhere is what produced the dark seam notch: two
//     arcs tiling the ring ({0, 0.5} and {0.5, 0.5}) both evaluated to exactly
//     0 at their shared seam, and no combining operator recovers a signal from
//     (0, 0). The notch spanned fTail + fHead = 28 px and was punched radially
//     outward through the halo and bloom, which emitCover also scales.
//
//   - Feathering outward everywhere - including at free endpoints - breaks
//     worse, and specifically at cornerRadius 0. The inverse-SDF map is
//     DEGENERATE at a sharp corner: the entire 90-degree exterior wedge has the
//     corner as its nearest perimeter point, so every fragment in that quadrant
//     shares ONE sPos. Coverage is a function of sPos alone, so any non-zero
//     value at a corner is painted across the whole quadrant. An arc starting
//     at a corner (start = 0 on a square rect, the default corner) lit its
//     entire top-left exterior quadrant at half brightness, bounded by two hard
//     edges where the neighbouring fragments mapped to uncovered perimeter.
//     A 7 px bleed in perimeter space is not a 7 px bleed on screen.
//
// Deciding per endpoint keeps the outward ramp exactly where it is safe: an
// abutting endpoint's "bleed" lands inside a neighbour that is already lit, so
// it cannot reach unlit geometry however degenerate the map is there.
//
// Feather widths are perimeter fractions (pixel-space widths / current perimeter,
// converted at the call site).
float arcCoverContinuous(float sPos, float start, float length, float fHead, float fTail,
                         bool tailAbuts, bool headAbuts) {
    if (length >= 1.0 - 1e-6) return 1.0;   // full coverage
    if (length <= 1e-6)       return 0.0;   // empty
    float rel = sPos - start;
    rel -= floor(rel);                       // wrap to [0, 1): distance past start
    // An outward tail ramp reaches BEHIND the start, so those fragments need a
    // small NEGATIVE rel rather than one wrapped up to near 1. Split the gap
    // between the arc's head and its own start down the middle: past that
    // midpoint a fragment is approaching the start, not trailing the head.
    // Only for an outward tail - an inward one never reads rel < 0.
    if (tailAbuts && rel > 0.5 * (1.0 + length)) { rel -= 1.0; }
    // Cap each feather at a share of the arc's own length. The widths arrive
    // as a fixed pixel span but `length` is a perimeter FRACTION, so on a small
    // rect a short arc can be narrower than the two ramps combined - they then
    // overlap and clip the peak, making the same arc config dimmer on a smaller
    // rect (0.21 vs 1.00 for L = 0.02 at 200x150 vs 800x600). Capping keeps the
    // peak at 1.0 for any length at any size, and leaves long arcs untouched.
    float cap    = length * ARC_FEATHER_MAX_SHARE;
    float fH     = min(fHead, cap);
    float fT     = min(fTail, cap);
    // Tail: inward ramps 0 -> 1 over [start, start + fT]; outward ramps over
    // [start - fT, start], reaching 1 AT the start.
    float tailIn = tailAbuts ? smoothstep(-fT, 0.0, rel)
                             : smoothstep(0.0, fT, rel);
    // Head: inward falls 1 -> 0 over [end - fH, end]; outward holds 1 to the
    // end and falls over [end, end + fH].
    float headIn = headAbuts ? 1.0 - smoothstep(length, length + fH, rel)
                             : 1.0 - smoothstep(length - fH, length, rel);
    return tailIn * headIn;
}

// A piece whose own halo plus bloom at this fragment is below this, in linear
// light before the grade, keeps the gathered coverage rather than its own: the
// difference moves the output by well under one 8-bit level (the grade maps
// 1e-3 to about 1/255). Most pieces are this faint at most fragments - the far
// side of the rect, the bloom past `reach`.
const float GLOW_PIECE_MIN = 1e-4;

// The segment's glow magnitude from one piece's (arc, segment) coverage pair:
// its coverage times the soft gate the segment glow has always taken, max with
// the arc so a segment on a lit arc keeps the arc's glow reach.
float segmentGlow(vec2 cover) {
    return cover.y * max(cover.x, min(cover.y, 1.0));
}

// How lit one piece of the outline is as its own halo and bloom see it: the
// arcs' coverage x intensity (.r halo, .g bloom) and the segments' boost x bell
// (.b halo, .a bloom) over that piece's own extent, each weighted by the
// layer's kernel at the fragment - one filtered fetch of the table
// neon-glow-cover.frag bakes once per config change, at the texture coordinate
// glowCoverStraightUV / glowCoverCornerUV (neon-pieces.glsl) give for the
// piece.
vec4 glowCoverAt(vec2 uv) {
    vec4 e = textureLod(uGlowCover, uv, 0.0);
    return e / max(1.0 - e, vec4(1.0 / 1024.0));
}

// V19, V20 and V21: ONE PIECE's halo and bloom scaled by that piece's OWN
// coverage rather than the gathered mean.
//
// The exact term is INTEGRAL cover(s) * K(|p - P(s)|) ds over the piece: the
// coverage along the piece, weighted by the layer's kernel around the
// fragment's foot of perpendicular. The gathered mean V14 scales the glow by is
// a stand-in for it that is wrong in two ways. Its kernel is a Lorentzian of
// width kc, where the halo's falls as 1/t^3 along the line, so a dark stretch
// kept light from lit ones hundreds of px away and a narrow halo drew a thin
// line along it (V19). And it is gathered around the FRAGMENT, so it is
// dominated by whichever piece is nearest: on a dark line it drops, and the
// lit far edges, scaled by the same mean, dropped with it - a groove along the
// line (V20).
//
// So each piece reads its own, from the table (glowCoverAt): the ratio of
// INTEGRAL cover * K to INTEGRAL K over the piece, which times the piece's own
// halo or bloom - that piece's INTEGRAL K - is the exact term. Exact for
// feathered arcs on every piece, every arc end summed so nothing switches as
// the fragment moves, and the segments' bells integrated to ~1e-3 of their
// boost. V20's table convolved along a straight line through the foot, past
// the piece's ends, so light spilled round each corner (V21); each piece's
// table now stops at its own ends, and a corner's runs along the line
// arcTangentSegment develops it onto.
//
// Per piece rather than once for the fragment, because each piece's table is
// continuous in the fragment's position, so nothing switches at the medial axis
// where the NEAREST piece changes - the crease V14 removed by gathering stays
// gone.
//
// Applied as a CORRECTION: the glow is first scaled by the gathered mean
// exactly as before V19, and each piece adds only the difference its own
// coverage makes, times its halo `h` and bloom `b`, to `fix` = (halo arc, halo
// segment, bloom arc, bloom segment). A piece that is skipped therefore costs
// one compare and nothing else - and the table's coordinates, which take a log
// and for a corner an atan, are worked out only past it.
//
// Measured against the closed form evaluated here per piece instead of read
// from the table: the same picture, and 2.0x / 3.3x the pre-V19 cost on a
// partly lit ring at scale 1.0 / 0.5. The four earlier attempts at an exact
// halo in this function cost 1.2-1.7x on EVERY scene, fully lit ones included,
// presumably through the program's register use: keep what runs here small.
//
// `haloWeight` / `bloomWeight` are the piece's halo and bloom as they reach the
// output, for the GLOW_PIECE_MIN test; the caller zeroes both on a ring lit
// uniformly, where every piece's coverage IS the gathered one and the
// correction is exactly 0. `gatheredSeg` is segmentGlow(gathered), the segment
// magnitude the plain sums are scaled by.
void addPieceGlowFix(inout vec4 fix, vec2 uv, vec2 gathered, float gatheredSeg, float h, float b) {
    vec4 cover  = glowCoverAt(uv);
    vec2 coverH = cover.rb;
    vec2 coverB = cover.ga;
    fix += vec4((coverH.x - gathered.x) * h, (segmentGlow(coverH) - gatheredSeg) * h,
                (coverB.x - gathered.x) * b, (segmentGlow(coverB) - gatheredSeg) * b);
}

// A straight: `band` which one (GLOW_COVER_BAND_*), `x` the fragment's
// projection along it from its t1 end, unclamped, `len` its length, `a` its
// distance from the line.
void addStraightGlowFix(inout vec4 fix, int band, float x, float len, float a, float kh,
                        vec2 gathered, float gatheredSeg, float h, float b, float haloWeight, float bloomWeight) {
    if (haloWeight + bloomWeight <= GLOW_PIECE_MIN) {
        return;
    }
    addPieceGlowFix(fix, glowCoverStraightUV(band, x, len, a, kh, glowCoverInner(band, uGlowCoverSplit)),
                    gathered, gatheredSeg, h, b);
}

// A corner: `signs` its quadrant in vPos, `w` the fragment's offset from its
// centre in arcTangentSegment's frame.
void addCornerGlowFix(inout vec4 fix, vec2 signs, vec2 w, float kh, vec2 gathered, float gatheredSeg,
                      float h, float b, float haloWeight, float bloomWeight) {
    if (haloWeight + bloomWeight <= GLOW_PIECE_MIN) {
        return;
    }
    int block = glowCoverCornerBlock(signs);
    addPieceGlowFix(fix, glowCoverCornerUV(block, w, uCornerRadius, kh, glowCoverInner(block, uGlowCoverSplit)),
                    gathered, gatheredSeg, h, b);
}

// One corner arc, start to finish: developed onto its tangent
// (arcTangentSegment, whose .w is the measure the development rate cost), its
// halo and its bloom - the bloom against the arcs' shared `pedestal`, WEIGHTED,
// since that is what has to reach zero at `reach` - added to `sum` (halo,
// bloom), and its correction from its own coverage (addCornerGlowFix).
//
// ONE ARC AT A TIME, AND THAT IS LOAD-BEARING. This block used to develop all
// four arcs first, then take four halos, four blooms and four coverage reads,
// so sixteen floats of developed segment were live at once alongside
// everything the rest of the shader still holds - and at scale 1.0 this
// program also carries the gather loop, whose speed follows the register
// count of the whole program, code a frame never runs included. Measured on
// an AMD Radeon Pro 5300M at 1280 x 720 and scale 1.0, against the build
// before V21: in that order, the per-piece coverage read V21 added made the
// whole neon pass 1.07-1.13x slower - on fully lit rings, which skip every
// read, and on a SHARP-cornered one, which never enters this block - however
// the read's arithmetic was trimmed; one arc at a time, 0.95x on the same
// scenes and 1.00x across neon-scale-check's ten fully lit ones. Keep the four
// calls whole.
void addCornerPiece(inout vec2 sum, inout vec4 fix, vec2 signs, vec2 w, float kh, float bw, float pedestal,
                    vec2 gathered, float gatheredSeg, float haloW, float bloomW) {
    vec4  c = arcTangentSegment(w, uCornerRadius);
    float h = haloSegment(c.x, c.y, c.z, kh) * c.w;
    float b = max(bloomSegment(c.x, c.y, c.z, bw) * c.w - pedestal, 0.0);
    sum += vec2(h, b);
    addCornerGlowFix(fix, signs, w, kh, gathered, gatheredSeg, h, b, haloW * h, bloomW * b);
}



// ---------------------------------------------------------------------------

void main() {
    vec2  halfSize = uRectSize * 0.5;
    float d  = sdRoundBox(vPos, halfSize, uCornerRadius);
    float ad = abs(d);

    // Note: the far exterior is culled on the CPU - the draw quad is
    // sized to rect + glowReach in NeonRenderer::setupGeometry, so geometry culls
    // the far region instead of a per-fragment discard (tiler-friendly).
    //
    // The cuts below are BACKED BY GEOMETRY TOO now, and the discards are what
    // is left over rather than the whole story. setupGeometry caps the quad's
    // margin under GlowSide::INSIDE, whose lit region is exactly a quad, and
    // cuts a hole in it for GlowSide::OUTSIDE and for an enabled insideCutoff,
    // whose lit regions are annuli - the same ring construction the opaque fill
    // has always used to bound itself. These discards still have to be here:
    // the geometry is a conservative bound rounded outward by a few px, and it
    // is the discards that place the edge exactly.
    //
    // That makes glowSide and insideCutoff inputs to the QUAD, which they were
    // not before, so both are in setupGeometry's dirty gate. Leaving them out
    // does not under-draw, it draws the previous config's bound - see the note
    // on geometryDirty in NeonRenderer::OnConfigChanged.
    // The one-sided cut's antialiasing width: one DESTINATION pixel, expressed
    // in this shader's units. |grad d| == 1 for an SDF, so fwidth(d) is one
    // BUFFER pixel, and uResolutionScale converts that to the pixel the blit
    // finally lands on - exactly the conversion the full-res px constants out
    // of neon-tuning.h take, and a no-op at scale 1.0.
    //
    // Destination pixels, not buffer pixels, because the thing this edge has
    // to register with is the opaque fill, and the fill is ALWAYS full-res on
    // the caller's framebuffer. A buffer-pixel floor here is 1/scale times too
    // wide: at scale 0.25 it put a 4 px ramp into the buffer, which the
    // bilinear blit then smeared wider still. Below scale 1.0 this floor is
    // sub-buffer-pixel and so is a step in the buffer, which is right - at a
    // reduced scale the blit owns the edge's softness and nothing in here can
    // sharpen it. The inside/outside cutoffs floor at this same pixel too, on
    // both paths - see softFloor below.
    //
    // Computed HERE, above every discard in this function, because a
    // derivative downstream of control flow the compiler cannot prove uniform
    // is undefined, and these discards are per-fragment rather than uniform.
    // Same rule black-rect.frag's main() documents at length. This is the only
    // derivative in this shader - keep it at the top if a second one is ever
    // needed.
    float sideAA = max(fwidth(d) * uResolutionScale, 1e-6);

    // TOTAL feather width of the one-sided cut, floored at that pixel. The
    // floor is what antialiases the cut: uGlowSideSoftness 0 used to leave
    // softEdge at a near-zero floor, which is a hard step, so the glow's edge
    // stair-stepped along every rounded corner while the opaque fill's own
    // d == 0 edge - box-filtered through fwidth in black-rect.frag - stayed
    // clean right beside it.
    //
    // The floor only does that work because the ramp it sizes is applied BELOW
    // the tone map, as coverage. Above it, a 1 px ramp on a filament core is
    // compressed to near nothing and the cut stair-steps anyway, floor or no
    // floor - the measurements are at the application site, after the grade.
    float sideSoft = max(uGlowSideSoftness, sideAA);

    // How far the ramp may reach back across the line, and the ONLY part of it
    // that lands on the side an OpaqueMode fill does not cover. Half a pixel
    // at full res, because that is exactly how far the fill's own box filter
    // reaches back too (black-rect.frag's edgeIn / edgeOut span d in
    // [-sideAA/2, +sideAA/2]) - so the two share one ramp and neither shows
    // past the other.
    //
    // DIRECT PATH ONLY. This shader owns the cut at resolutionScale 1.0 and
    // nowhere else, because the reasoning above does not survive the blit: a
    // buffer texel whose CENTRE is lit gets bilinear-smeared over 1/scale
    // destination pixels in BOTH directions, so wherever the cut is put inside
    // the buffer, the reconstruction washes some of it across the line. Zeroing
    // the back-reach was an attempt to hold that down and it only halved it -
    // measured at scale 0.5, glowSide OUTSIDE, softness 0: the destination
    // pixel at d = -0.5, on the dark side, still came back at 59/255, over
    // backdrop an OpaqueMode fill never reaches.
    //
    // A buffer-resolution signal cannot carry a destination-resolution edge, so
    // the cut moved to where the destination pixels are: neon-blit.frag applies
    // it, with this same anchor and floor recomputed from ITS fwidth. What is
    // left here is the CULL, which runs BLIT_SIDE_GUARD_PX past the cut so the
    // blit has lit texels to rebuild the boundary from - see neon-tuning.h.
    //
    // The cut is ANCHORED at the line, not centred on it: the feather runs
    // from sideBack INTO the lit side, so the glow's support is the fill's
    // support extended forward and never reaches back past it.
    //
    // It used to span [-softEdge, +softEdge]: a feather centred on d == 0, so
    // HALF of it landed on the side OpaqueMode does not fill. With
    // glowSide = OUTSIDE and opaqueMode = OUTSIDE the glow washed
    // uGlowSideSoftness/2 px over the bare backdrop INSIDE the rect, with no
    // fill under it, and was still climbing out of its ramp where the fill had
    // already gone solid - the blurred, unmatched seam that pair of settings
    // exists to not have. Measured at softness 4 on a 200x150 rect: red glow
    // at 232, 206, 127, 18 on the four pixels inside the edge, over a backdrop
    // the fill never touched. GlowSide says "restrict the glow to one side of
    // the line"; it now does.
    // Uniform branch (uResolutionScale is a uniform), and below every
    // derivative in this function, so it is free and legal both.
    bool  blitOwnsCut = (uResolutionScale < 1.0);
    float sideBack = 0.5 * sideAA;
    float sideCull = blitOwnsCut ? BLIT_SIDE_GUARD_PX : sideBack;
    // Deliberately NOT mirrored in neon-gather.frag - see the note at its top.
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  sideCull) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -sideCull) discard;

    // Hard geometric cutoffs. The band is [-uInsideCutoff, +uOutsideCutoff],
    // and each side's feather STARTS at that boundary and runs its softness
    // beyond it - the emission is untouched inside the band and gone by
    // cutoff + softness. Anything past the feather is culled here so
    // bloom/halo can't leak beyond the artist's stated reach even if
    // uGlowRadius says otherwise. Where the fade sits is worked out below,
    // at inMid / outMid. Softness is
    // decoupled from uGlowSideSoftness so the one-sided cut at d=0 can stay
    // pixel-tight while the cutoff joins fade smoothly, and per-side so the
    // interior and exterior can taper at different rates. Disabled sides
    // arrive with size = a huge sentinel, so these branches no-op.
    //
    // THE FLOOR IS AN ANTIALIASING FLOOR, and it applies at every resolution.
    // It used to be a near-zero floor at scale 1.0 - a hard step - on the
    // grounds that the direct path rasterises at the destination rate and so
    // places a hard cutoff exactly. It does place it exactly, and then draws it
    // with no coverage at all. Walking the boundary across one pixel in 1/8 px
    // steps, reading the pixel that straddles it, the whole sweep read
    //
    //     0, 0, 0, 0, 0, 132, 132, 132
    //
    // which is a binary edge: invisible on the axis-aligned straights, a
    // staircase everywhere the boundary curves, which is every rounded corner
    // and all four corners of a cornerRadius-0 band (bandOuterDistance makes
    // those square, not the boundary smooth). The one-sided cut got its floor
    // for exactly this reason; this is the same edge with the same defect.
    // The after-and-in-between rows are at the mask itself, below the grade.
    //
    // One DESTINATION pixel, so it reuses sideAA rather than taking a second
    // derivative - dIn and dOut are unit-gradient like d, so a pixel is a pixel
    // in all three. See the note at sideAA, which asks any second derivative to
    // be hoisted to the top of main() instead of added here.
    //
    // ONE DESTINATION PIXEL ON BOTH PATHS. The scaled path used to floor at
    // CUTOFF_SOFT_FLOOR_PX instead, a feather in BUFFER px that let the texel
    // nearest a hard boundary carry a fractional value for the blit to place
    // sub-texel. That was the best a mask drawn INTO the reduced buffer could
    // do, and it still left the boundary softened across a buffer texel and
    // snapped toward the buffer grid. Below scale 1.0 the masks are not drawn
    // here any more: neon-blit.frag applies them at destination resolution,
    // exactly as it applies the one-sided cut (see blitOwnsCut, the discards
    // below and the masks after the grade). On that path this floor only places
    // the discards, and they have to agree with the blit's ramp - which is
    // floored at one destination pixel, i.e. sideAA in this shader's units.
    float softFloor = sideAA;
    float inSoft  = max(uInsideCutoffSoftness,  softFloor);
    float outSoft = max(uOutsideCutoffSoftness, softFloor);

    // TOTAL widths, halved here because each ramp is centred on the midpoint
    // of its fade (inMid / outMid, below).
    //
    // smoothstep(-w, w, x) spans 2w, so a softness of S px used to feather over
    // 2S - the identical bug black-rect.frag found in its own two ramps and
    // documents at length, still live here afterwards. Two consequences, and
    // they are its two as well: at the floor the ramp covered a pixel either
    // side of the boundary, so the outermost and innermost pixel of the band
    // were both partially lit and the band read ~1 px narrow per side; and at
    // any stated softness the emission faded over twice the documented width,
    // while an OpaqueMode fill sharing that boundary faded over exactly it.
    // Those two are drawn on top of each other, which is what made the
    // disagreement visible.
    float inHalf  = 0.5 * inSoft;
    float outHalf = 0.5 * outSoft;

    // WHERE each fade sits: it starts at the cutoff and runs the requested
    // softness beyond it, so its MIDPOINT is softness/2 past the cutoff. The
    // distances below are measured from that midpoint and the ramps built
    // symmetrically about it, over the FLOORED width. For softness at or
    // above the floor that is exactly cutoff -> cutoff + softness. Below the
    // floor the one-pixel antialiasing ramp stays centred on the requested
    // fade rather than starting at the cutoff, and that is deliberate: at
    // softness 0 the edge's 50% point is the cutoff itself at every
    // resolution scale. Anchoring the floored ramp at the cutoff instead
    // would put that point half a FLOOR out - 0.5 px at full res, but 2 px at
    // scale 0.25, where the floor is one buffer pixel - so the edge would move
    // with resolutionScale.
    //
    // Disabled sides arrive as the huge sentinel and a neutralised side is
    // overridden below, so neither needs a branch here.
    float inMid  = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
    float outMid = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);

    // Band boundaries measured against the offset rect, so a cornerRadius-0
    // band keeps square corners instead of being rounded by the cut distance.
    //
    // A CUTOFF ON THE SIDE glowSide ALREADY CULLS IS NEUTRALISED, by handing it
    // the same huge distance a disabled cutoff arrives with, so every use below
    // no-ops through the arithmetic rather than through a branch.
    //
    // It can never be the binding constraint there. GlowSide::OUTSIDE keeps
    // d >= -sideBack, about half a pixel; an inside cutoff of size S keeps
    // d >= -(S + softness/2 + inHalf), i.e. -(inMid + inHalf), which is looser
    // for every S >= 0. Mirror argument for
    // INSIDE and the outside cutoff. So this removes a constraint that was
    // already doing nothing to the silhouette - but it was NOT doing nothing:
    //
    //   - On the scaled path it ATE THE GUARD BAND. neon-blit.frag reconstructs
    //     the cut from BLIT_SIDE_GUARD_PX of lit buffer texels past it; an
    //     inside cutoff of size 0 discards from half a buffer pixel in, which
    //     wipes that band out and hands the blit black to filter. Measured on a
    //     1920x1080 rect at 3840x2160, glowSide OUTSIDE, softness 0, toggling
    //     insideCutoff between off and {on, size 0, softness 0} - two configs
    //     that describe the same silhouette: the first lit pixel fell from 238
    //     to 178 at scale 0.5 and from 228 to 142 at scale 0.25, the exact
    //     dark seam the guard band exists to prevent. Only SMALL cutoffs reach
    //     the band (S * scale + inHalf < guard), which is what made this hide.
    //   - At full res it applied a second coverage ramp over the first, so the
    //     shared boundary came out squared - 8 px different, worst 6, all on
    //     the corners.
    //
    // NeonRenderer::setupGeometry mirrors this: the same redundant cutoff must
    // not shrink the quad's hole into the guard band either.
    float dOut;
    float dIn;
    if (uGlowSide == GLOW_SIDE_INSIDE)
    {
        dOut = -CUTOFF_NEUTRALISED;
    }
    else
    {
        dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, outMid);
    }
    if (uGlowSide == GLOW_SIDE_OUTSIDE)
    {
        dIn = CUTOFF_NEUTRALISED;
    }
    else
    {
        dIn = bandInnerDistance(d, inMid);
    }
    // On the scaled path the blit owns the masks as well as the cut, and
    // rebuilds each boundary from the buffer texels around it - so the cull
    // runs BLIT_CUTOFF_GUARD_PX past the end of the ramp rather than at it, for
    // the reason sideCull does. The guard is an exact 0 at scale 1.0, where
    // the direct path keeps culling exactly where its own masks end.
    float cutGuard = blitOwnsCut ? BLIT_CUTOFF_GUARD_PX : 0.0;
    if (dOut >  outHalf + cutGuard ) discard;
    if (dIn  < -(inHalf + cutGuard)) discard;

    // --- Filament -----------------------------------------------------
    // Generalized-Gaussian profile with exponentially smooth falloff:
    //
    //   core(ad) = exp(-ln(2) * (ad / sigma)^N)
    //
    // sigma = half-brightness radius (core = 0.5 at ad = sigma).
    // N = 2 * uFilamentFalloff controls the shape:
    //   uFilamentFalloff = 0.5 -> N = 1   (Laplace - heavy tails, smooth peak)
    //   uFilamentFalloff = 1.0 -> N = 2   (Gaussian - pure smooth falloff; default)
    //   uFilamentFalloff = 2.0 -> N = 4   (platykurtic - flatter top, sharper shoulder)
    //   uFilamentFalloff = 5.0 -> N = 10  (near-rectangular)
    //
    // The Gaussian has no power-law tail (unlike the old super-Lorentzian),
    // so the filament reads as a clean thin line with a naturally smooth
    // roll-off - no heavy glow bleed far from the line axis.
    //
    // Peak at ad = 0 is always exactly 1.0.
    //
    // lineGate fades the filament from 0 at lineWidth = 0 up to full at
    // lineWidth = FILAMENT_MIN_HALF_WIDTH * 2, so lineWidth = 0 means "no
    // line" instead of a single-pixel bright dot.
    //
    // FILAMENT_MIN_HALF_WIDTH is a full-res px constant and uLineWidth arrives
    // scaled, so the constant is converted into the same space before either
    // is used. At uResolutionScale = 1.0 minHalf IS the constant and both lines
    // below reduce to their full-res form; the 1e-3 floors only guard a scale
    // driven pathologically close to zero.
    float minHalf   = FILAMENT_MIN_HALF_WIDTH * uResolutionScale;
    float halfWidth = uLineWidth * 0.5;
    // Generalized-Gaussian exponent, needed before the sampling floor below
    // because that floor depends on the profile's SHAPE, not only its width.
    float N         = 2.0 * max(uFilamentFalloff, 1e-3);
    // Two floors, and they are in different spaces on purpose.
    //
    // minHalf is the STATED-width floor, converted like every other full-res
    // constant here; it is what pairs with lineGate below to make lineWidth 0
    // mean "no line".
    //
    // `nyquist` is the SAMPLING floor and is in BUFFER px, so it is NOT
    // converted: it exists because below resolutionScale 1.0 this shader
    // rasterises into a reduced buffer that neon-blit.frag then bilinearly
    // upsamples, and a filament the buffer cannot sample does not survive the
    // round trip - its peak lands wherever the rect edge happens to fall
    // between buffer texel centres. Converting it as well is what made a 1 px
    // line at scale 0.5 look wrong: sigma floored at 0.25 buffer px, and the
    // peak swung with the rect's sub-pixel position against a 1.0 reference
    // that does not move.
    //
    // IT IS NOT A FIXED HALF WIDTH. It used to be, and a fixed half width
    // asks the wrong question. What survives the blit is not decided by how
    // wide the profile is at half maximum - it is decided by how much signal
    // the NEIGHBOURING buffer texel still carries, because that is what the
    // bilinear filter reconstructs the peak from. So the floor is stated that
    // way instead: sigma must be large enough that the profile is still at
    // FILAMENT_NYQUIST_MIN_SHARE of its peak FILAMENT_NYQUIST_SAMPLE_PX out.
    // Inverting core() for sigma gives the expression below.
    //
    // At the default falloff (N = 2) it evaluates to exactly 0.5 buffer px,
    // which is the constant it replaces - so nothing at or near the default
    // moves. It matters at a SOFT falloff, where the two questions diverge
    // hard: at filamentFalloff 0.27 (N = 0.54) reachSigmas clamps at
    // FILAMENT_REACH_MAX_SIGMAS, so sigma multiplies a 64-sigma tail, and a
    // fixed 0.5 floor stretched a 31 px filament to 61 px at scale 0.5 and
    // past 120 px at 0.25 - to buy 5 levels of peak accuracy on a profile the
    // buffer was already sampling perfectly well. The share form asks for
    // 0.082 buffer px there, under what the caller asked for, so it does not
    // engage at all. See docs/corner-crease-and-filament-nyquist.md section 2.8.
    //
    // Gated to the scaled path, like softFloor above and for the same reason:
    // at scale 1.0 the gather already runs at the destination rate, there is
    // no blit to survive, and the direct path has to stay bit-identical to the
    // full-res renderer it replaced. The old constant was a no-op at 1.0 by
    // arithmetic coincidence; this one would not be above N = 2, so it is a
    // gate now rather than a coincidence. NeonRenderer::setupGeometry mirrors
    // both the expression and the gate when it sizes the quad.
    float nyquist   = (uResolutionScale < 1.0)
                        ? FILAMENT_NYQUIST_SAMPLE_PX /
                          pow(log2(1.0 / FILAMENT_NYQUIST_MIN_SHARE), 1.0 / N)
                        : 0.0;
    float sigma     = max(max(halfWidth, max(minHalf, 1e-3)), nyquist);
    float core      = exp2(-pow(ad / sigma, N));

    // Filament reach, in sigmas, for THIS falloff - see neon-tuning.h. Also
    // sizes the draw quad CPU-side; the two must stay in step.
    float reachSigmas = clamp(pow(log2(FILAMENT_GAIN / FILAMENT_CUTOFF), 1.0 / N),
                              FILAMENT_REACH_MIN_SIGMAS, FILAMENT_REACH_MAX_SIGMAS);
    // Pedestal-subtract the core so it reaches exactly zero at that reach,
    // renormalised to keep the ad = 0 peak at 1.0. Where the reach formula is
    // honoured the pedestal is ~1.7e-4 and invisible; where MAX_SIGMAS clamps
    // it (soft falloff, whose tail would otherwise run for hundreds of sigmas)
    // this is what makes the glow end smoothly and, crucially, SYMMETRICALLY.
    // Without it the interior kept the full tail while the exterior was cut at
    // the quad edge.
    float corePed   = exp2(-pow(reachSigmas, N));
    core            = max(core - corePed, 0.0) / max(1.0 - corePed, 1e-6);
    float lineGate  = clamp(uLineWidth / max(minHalf * 2.0, 1e-3), 0.0, 1.0);

    // Perimeter of the rounded rect, in px - converts the arc feathers below
    // from px to perimeter fractions. The gather sizes its colour kernel from
    // the same function.
    float peri = rectPerimeter();

    // --- Kernel widths ------------------------------------------------
    // Two separate kernels, and the split is the point:
    //
    //  - kc is the COLOUR gather weight, sized inside gatherPerimeter
    //    (neon-common.glsl) as a fraction of the PERIMETER rather than in
    //    pixels, so the gradient reads identically at any size and cannot
    //    bead. Not coupled to glowRadius: the gather is colour only, so a wide
    //    glow has no business desaturating the ring.
    //
    //  - kh / bw are the EMISSION widths: raw glowRadius, no floor. The halo
    //    and bloom are evaluated analytically from the SDF distance further
    //    down, and a closed form cannot bead however far apart the gather
    //    samples are, so glowRadius is proportional across its entire range.
    float kh  = max(uGlowRadius,                       EMISSION_MIN_WIDTH);
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, EMISSION_MIN_WIDTH);

    // --- Colour gather -----------------------------------------------------
    // The gather's four results: the base and segment hues, each of unit
    // magnitude, and the two g-weighted mean coverages that scale the glow
    // (see the glow-coverage block below). The direct path runs the loop here;
    // the NEON_READS_GATHER variant reads back what neon-gather.frag stored.
    // Either way it is COLOUR ONLY - the halo and bloom intensities are
    // computed in closed form after it.
#ifdef NEON_READS_GATHER
    // The gather's four results, read back from the reduced pass instead of
    // recomputed. The loop is most of this shader's cost - about three
    // quarters of a full-res frame on the reference scenes - and the ring's
    // whole point is to pay for everything EXCEPT it at full resolution.
    //
    // textureLod at level 0, not texture(): the discards above make control
    // flow non-uniform here, where an implicit derivative is undefined (see the
    // rule at sideAA). The attachments have no mip levels, so level 0 is what
    // texture() would have read anyway.
    //
    // The coverages are stored as e = c / (1 + c) - see the write in
    // neon-gather.frag - and decoded after the bilinear read, which is the
    // filtering the emulation behind the plan measured. The floor keeps a
    // saturated texel (e = 1) finite: it decodes to 254, far above any
    // coverage a config produces.
    vec2  gatherUV          = vPos * uGatherUVScale + uGatherUVOffset;
    vec4  gather0           = textureLod(uGather, gatherUV, 0.0);
    vec3  col               = gather0.rgb;
    float emitCoverGathered = gather0.a / max(1.0 - gather0.a, 1.0 / 255.0);
    vec3  segColHue         = vec3(0.0);
    float segCoverGathered  = 0.0;
    if (uSegmentCount > 0) {
        vec4 gather1     = textureLod(uGatherSeg, gatherUV, 0.0);
        segColHue        = gather1.rgb;
        segCoverGathered = gather1.a / max(1.0 - gather1.a, 1.0 / 255.0);
    }
#else
    PerimeterGather gather  = gatherPerimeter(vPos);
    vec3  col               = gather.hue;      // base perimeter hue
    float emitCoverGathered = gather.arcCover; // arc coverage x intensity
    vec3  segColHue         = gather.segHue;   // segment hue
    float segCoverGathered  = gather.segCover; // segment boost x bell
#endif

    // --- Continuous coverage, read at this fragment's own position -------
    // Recover the fragment's OWN continuous perimeter position GEOMETRICALLY
    // from vPos (inverse of the CPU's GetPointOnRectangle) and read each arc
    // directly there. Far-from-line fragments get a valid position too, but
    // their filament core ~= 0 so it never shows. The geometric inverse is
    // exact even at corners, unlike the old proximity-weighted circular mean
    // of the sample phases, which smeared the whole corner curve to ~0 and
    // lit it for any arc starting at position 0.
    float sPos = perimeterPosition(vPos);
    // Inward feathers: convert pixel widths to perimeter fractions at the
    // current geometry (`peri` is computed above the gather). `peri` is derived
    // from uRectSize and so is in SCALED px, while the two constants are
    // full-res - hence the conversion, which is identity at scale 1.0. Getting
    // it wrong changes the feather's width in perimeter fractions, i.e. the arc
    // ends soften over a different length at a different resolution scale.
    float headF  = HEAD_FEATHER_PX * uResolutionScale / peri;
    float tailF  = TAIL_FEATHER_PX * uResolutionScale / peri;
    // ONE arc coverage, folding per-arc intensity in, and it drives the
    // filament as well as the halo and bloom. `col` is gated-normalised above,
    // so intensity cancels out of it and can no longer reach the filament that
    // way - emitCover is what carries it. The scaling stays linear in
    // intensity, exactly as it was when it rode on `col`, and both layers are
    // now shaped by the same px-based (size-invariant) feathers.
    // Colour-stop ALPHA rides here, on the magnitude, for two reasons.
    //
    // It cannot ride on `col`: that sum is divided by the same weight it was
    // gathered with, so any scale folded into it cancels exactly.
    //
    // And it is read POINTWISE, at this fragment's own perimeter position,
    // rather than gathered like the hue - for the same reason emitCover is.
    // The gather weight 1/(dd + kc2) is a Lorentzian with 1/d^2 tails, so a
    // gathered alpha is a ring-wide weighted mean: a half-perimeter faded to
    // 0 still measured 0.44 of full brightness at its own midpoint, dragged
    // up by the opaque far side, while the opaque half was dragged down. The
    // pointwise read is exact at every position and needs no normalisation.
    //
    // Alpha 0 therefore kills the filament, halo and bloom together at that
    // position, and the premultiplied output alpha (peak channel, bottom of
    // main) follows for free, so the background shows through rather than
    // being occluded by a black tube.
    float baseAlphaPt = texture(uGradientLUT,
                                vec2(sPos - uTime * uHueRotationRate, 0.5)).a;
    // Winner-take-all across arcs, as documented for overlap. This can be a
    // plain max() again because arcCoverContinuous now reaches a FULL 1.0 at an
    // abutting endpoint rather than 0 (inward) or 0.5 (straddling), so two arcs
    // tiling the ring hand over at max(w1, w2) with no notch - and because
    // their ramps overlap, the handover stays smooth even when w1 != w2.
    float emitCover = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                       // dark arc: no filament
        float c = arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF,
                                     arcTailAbuts(arc.w), arcHeadAbuts(arc.w));
        if (c <= 0.0) continue;                           // does not reach here
        // Each arc's own alpha, from the same LUT its colour came from and in
        // the same coordinate space the gather used - arc-local for hasStops,
        // perimeter space otherwise.
        float aA;
        if (arcHasStops(arc.w)) {
            // NO hue-rotation term here, unlike the base-gradient path above.
            // uArc is the arc's OWN head-to-tail coordinate, not a position on
            // the perimeter ring, so there is nothing for a rotation to rotate:
            // subtracting uTime * rate just slid the gradient off one end, and
            // the atlas wrap brought the tail colour back round to butt against
            // the head mid-edge with no geometric feature to hide the seam.
            // Segments never had the term, and this is what makes arcs match
            // them. An arc's gradient moves by moving the arc (Arc::start) or
            // by animating its stops.
            //
            // WRAPPED, exactly as arcCoverContinuous wraps its own rel. An arc
            // may straddle the seam (start 0.8 + length 0.4 is legal - see
            // Arc::start), and coverage already handles that, so a plain
            // sPos - start would go NEGATIVE past the seam and CLAMP_TO_EDGE
            // would pin the whole wrapped remainder to the head colour. The
            // arc stayed lit and lost its gradient.
            //
            // The midpoint split is the other half: an outward tail feather
            // reaches BEHIND the start, and those fragments want a small
            // negative rel (clamping to the head) rather than one wrapped up
            // to near 1 (clamping to the tail). Same threshold as the coverage
            // feather so the two cannot disagree.
            float rowY = (float(a) + 0.5) / float(MAX_ARCS);
            float rel  = sPos - arc.x;
            rel       -= floor(rel);                       // wrap to [0, 1)
            if (rel > 0.5 * (1.0 + arc.y)) { rel -= 1.0; } // behind the start, not past the head
            float uArc = rel / max(arc.y, 1e-4);
            aA         = texture(uArcLUT, vec2(uArc, rowY)).a;
        } else {
            aA = baseAlphaPt;
        }
        emitCover = max(emitCover, c * arc.z * aA);
    }

    // Segment coverage at this fragment's own perimeter position. This is the
    // segments' whole magnitude now: boost * bell, straight off the analytic
    // gaussian, so it cannot inherit either the gather's sample stepping or
    // the far-side dilution that used to make a segment dimmer on a small
    // rect. Segments emit where no arc covers, so they carry their own
    // filament/halo/bloom.
    float segCoverPt = 0.0;
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg = uSegments[s];
        float rel = sPos - seg.x;
        rel      -= floor(rel + 0.5);                     // wrap to [-0.5, 0.5]
        float e   = rel * seg.y;
        // Per-segment alpha, pointwise - see emitCover above. Stop-less
        // segments inherit the base gradient's alpha, mirroring how their
        // colour falls back to segFallback in the gather.
        float sA;
        if (seg.w > 0.5) {
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            sA           = texture(uSegmentLUT, vec2(tLocal, rowY)).a;
        } else {
            sA = baseAlphaPt;
        }
        segCoverPt += seg.z * exp(-e * e) * sA;
    }

    // Attach the segments' magnitude to their hue. Unclamped on purpose: boost
    // above 1 must still brighten, as it did when the gather's `bell` carried
    // the magnitude. (segmentGlow's min(.., 1.0) only bounds the shared
    // halo/bloom reach - it is not the segment's brightness.)
    vec3 segCol = segColHue * segCoverPt;

    // --- GLOW coverage: the same two magnitudes, GATHERED ------------------
    // The halo and bloom are integrals over the WHOLE emitter (see the block
    // below), so what scales them has to be the emitter's coverage AVERAGED
    // over that same integral - not the coverage at this fragment's nearest
    // perimeter point.
    //
    // The pointwise pair above is a function of sPos, and sPos is a
    // NEAREST-POINT map: it JUMPS across the medial axis, where the nearest
    // edge changes. Scaling a smooth field by a jumping scalar hands the glow
    // the medial axis as a visible boundary - hard 45-degree creases in from
    // every corner and a flat cut along the half-min extent, with the glow at
    // full strength on one side and zero on the other. Measured on a
    // 1920x1080 rect with one boosted segment on the top edge: 41 at the
    // half-height line, 8 (background) one pixel below it. A half-ring arc
    // renders its glow as a hard-edged polygon for the same reason.
    //
    // This is V4 one level up. That fix made the halo/bloom SHAPE a sum over
    // the emitter's pieces so the FIELD had no crease; the MAGNITUDE scaling
    // it stayed a nearest-point read, so the crease came back through it
    // whenever part of the perimeter was dark.
    //
    // ratio, not a second gather: the loop already accumulates SUM(cover * g);
    // dividing by SUM(g) - which gatherPerimeter does, so the pair arrives
    // above as emitCoverGathered / segCoverGathered on both paths - turns each
    // into the g-weighted mean of that coverage over the perimeter, which is
    // exactly the coverage term of
    //
    //     INTEGRAL cover(s) * K(|p - P(s)|) ds  ~=  cover_mean(p) * INTEGRAL K ds
    //
    // the closed forms below evaluate with cover == 1. Smooth by construction:
    // every sample contributes at every fragment, so nothing switches.
    //
    // A FULLY LIT RING IS UNCHANGED: every sample then carries arcW = 1, so
    // wsumLit and wsumAll are the same sum term for term and the ratio is
    // exactly the 1.0 the pointwise read returns. The two only diverge where
    // the perimeter is partly dark, which is the case this fixes. Measured
    // over five full-ring scenes (sharp, rounded, small rect, inside cutoff,
    // resolutionScale 0.5): at most 8 pixels of 2,073,600 move, all by 1/255
    // in one channel, which is the compiler re-associating the surrounding
    // expression rather than this ratio.
    //
    // Colour-stop ALPHA is NOT in this pair: neon-emission.frag's two alpha
    // channels carry arcW and bellSum without it, and adding it would need a
    // third row and so a third texelFetch in the hottest loop in the pipeline.
    // The pointwise alpha still gates the filament exactly. The glow does NOT
    // see it at all: the emission colour is the LUT's straight RGB, so neither
    // factor of emitGlow carries alpha, and an alpha-0 stretch keeps its full
    // halo and bloom (measured; V18 in docs/review-findings.md).
    //
    // V19, one level further: the gathered mean is right far from the line and
    // wrong ON it. Its Lorentzian has width kc, the halo's kernel kh, and on a
    // stretch no arc covers the mean still reaches lit perimeter hundreds of
    // pixels away, so a narrow halo drew a thin line along the dark stretch.
    // The pair below still scales the glow as it always did, and the halo
    // block then corrects that, per piece, toward the coverage at the piece's
    // foot of perpendicular - see addPieceGlowFix.
    vec2 gatheredCover = vec2(emitCoverGathered, segCoverGathered);

    // Sharp gate for the SDF-derived filament, from the same two pointwise
    // coverages. Both are exact at this fragment's perimeter position, so
    // neither can quantise a slow tracer's head to the gather points nor light
    // the corner preceding an arc's tail - the two bugs the old
    // circular-mean/sample-based gates had.
    float filamentGate = max(smoothstep(0.5, 1.0, min(segCoverPt, 1.0)), emitCover);

    // --- Analytic halo + bloom --------------------------------------------
    // Closed forms of the sums this shader used to run over the perimeter
    // samples, evaluated as a SUM OVER THE EMITTER'S PIECES - each a FINITE
    // segment - rather than once at the nearest-edge distance. Four straights
    // run between the tangent points, and above cornerRadius 0 four more carry
    // the corner arcs, developed onto their tangents.
    //
    // Analytic rather than gathered, because a closed form cannot bead however
    // far apart the loop samples are, so it needs no sample-spacing floor and
    // glowRadius sets the width directly at any rect size. That property is
    // untouched by the segment form: kh and bw still set the profile width.
    //
    // A sum rather than one term, because a single term at the nearest distance
    // is the field of an INFINITE line, and the sum buys two things:
    //
    //   - No interior medial-axis creases. A fragment on a corner diagonal has
    //     two edges equally near; the nearest-distance form lit it from one,
    //     which is where the dark diagonal wedges came from.
    //   - The corner behaviour of the gather this replaced, which summed over
    //     perimeter samples and so picked up both incident edges at a corner.
    //
    // haloSegment / bloomSegment reduce to the old infinite-line expressions in
    // the limit, so HALO_NORM_FACTOR / BLOOM_NORM_FACTOR keep the calibration
    // they were tuned to and the peak on a long edge is unmoved.
    //
    // Two things DO move, both measured in
    // docs/corner-crease-and-filament-nyquist.md:
    //
    //   - A small rect. An edge shorter than a few multiples of bw subtends
    //     less than the infinite line the old form assumed, so its bloom is
    //     dimmer - correctly, since a short tube emits less light. Below about
    //     100 px wide at the default glowRadius; at 320 px and up the
    //     difference is under 2/255.
    //   - The INTERIOR, which is the larger change of the two. The old term was
    //     a function of ad alone, so it lit a fragment 120 px inside the edge
    //     exactly as brightly as one 120 px outside. Inside, the emitter wraps
    //     around the fragment rather than receding from it, so the interior now
    //     settles on a floor instead of decaying to nothing: centre of the rect
    //     162 -> 186 on the 1000x500 / glowRadius 60 probe. Intended, and
    //     capped by insideCutoff rather than by a gain - see neon-tuning.h.
    //
    // Per edge: the perpendicular distance is the per-axis offset, and the
    // segment runs the full extent of the opposite axis, relative to this
    // fragment's foot of perpendicular - TRIMMED TO THE TANGENT POINTS, so a
    // straight stops where the tube actually turns. The corner arcs are the
    // four further segments below.
    vec2  straight = max(halfSize - vec2(uCornerRadius), vec2(0.0));
    float aLeft  = abs(vPos.x + halfSize.x);
    float aRight = abs(vPos.x - halfSize.x);
    float aTop   = abs(vPos.y + halfSize.y);
    float aBot   = abs(vPos.y - halfSize.y);
    float tv1    = -straight.y - vPos.y;
    float tv2    =  straight.y - vPos.y;
    float th1    = -straight.x - vPos.x;
    float th2    =  straight.x - vPos.x;

    // Distance at which the emission has to be gone: the CPU's uncapped
    // quad-sizing formula, recomputed here (see setupGeometry). A pure function
    // of glowRadius, bloomStrength and intensity, so the pedestal below is
    // size-invariant even where the outside cutoff clamps the actual quad
    // smaller - that path is masked by the cutoff smoothstep anyway, and
    // feeding it the clamped margin would subtract a huge pedestal and dim the
    // whole band. `sigma` is the filament half-width from the block above, so
    // the second term is the same filament-reach floor setupGeometry applies -
    // without it the two disagree at small glowRadius, and `reach` hits 0 at
    // glowRadius 0, making the pedestal subtract the entire bloom.
    float reach = max(uGlowRadius * GLOW_REACH_RADIUS_FACTOR *
                      (1.0 + uBloomStrength * uIntensity),
                      sigma * reachSigmas);

    // Each piece's own halo and bloom, and V19's correction to the coverage
    // that scales them (addPieceGlowFix). aTop measures to y = -halfSize.y and
    // aBot to +halfSize.y.

    // One arc over the whole ring and no segments: the coverage is the same at
    // every perimeter position, so every piece's own equals the gathered mean
    // and there is nothing to correct. It zeroes the weights below, so every
    // piece takes addStraightGlowFix's / addCornerGlowFix's first return.
    //
    // Do NOT turn this into an `if (!uniformCover)` around the corrections, or
    // around a second copy of the sums. Measured on an M2 Pro, either one makes
    // EVERY scene 10-15% slower at scale 1.0 - fully lit ones included, which
    // never enter it - presumably because this program carries the gather loop
    // there. Below 1.0 the same branch saves ~3%, which is not worth it.
    //
    // NeonRenderer skips baking the coverage table on this same test
    // (IsGlowCoverUnread, deliberately a hair stricter), so the table can be
    // stale whenever it holds - change the two together.
    bool uniformCover = uSegmentCount == 0 && uArcCount == 1 && uArcs[0].y >= 1.0 - 1e-6;

    // Each piece's linear weight in the output, for the GLOW_PIECE_MIN tests.
    // bloomGain is the renormalisation applied further down.
    float bloomPeak = BLOOM_NORM_FACTOR * PI;
    float bloomPed  = BLOOM_NORM_FACTOR * PI * bw / sqrt(reach * reach + bw * bw);
    float bloomGain = bloomPeak / max(bloomPeak - bloomPed, 1e-6);
    float haloW     = uniformCover ? 0.0 : HALO_NORM_FACTOR * HALO_GAIN;
    float bloomW    = uniformCover ? 0.0 : BLOOM_NORM_FACTOR * bloomGain * uBloomStrength;

    float hLeft  = haloSegment(aLeft,  tv1, tv2, kh);
    float hRight = haloSegment(aRight, tv1, tv2, kh);
    float hTop   = haloSegment(aTop,   th1, th2, kh);
    float hBot   = haloSegment(aBot,   th1, th2, kh);
    float bLeft  = bloomSegmentPedestalled(aLeft,  tv1, tv2, bw, reach);
    float bRight = bloomSegmentPedestalled(aRight, tv1, tv2, bw, reach);
    float bTop   = bloomSegmentPedestalled(aTop,   th1, th2, bw, reach);
    float bBot   = bloomSegmentPedestalled(aBot,   th1, th2, bw, reach);

    // The plain sums, as before V19, and V19's per-piece correction to them.
    float halo  = hLeft + hRight + hTop + hBot;
    float bloom = bLeft + bRight + bTop + bBot;
    float gatheredSeg = segmentGlow(gatheredCover);
    vec4  glowFix = vec4(0.0);
    // Each straight's table is read at the fragment's projection along it
    // from its t1 end - unclamped, since past an end its coverage still
    // changes - and its distance from the line.
    float ws       = 2.0 * straight.x;
    float hs       = 2.0 * straight.y;
    float alongX   = vPos.x + straight.x;
    float alongY   = vPos.y + straight.y;
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_NEG_X, alongY, hs, aLeft, kh, gatheredCover,
                       gatheredSeg, hLeft, bLeft, haloW * hLeft, bloomW * bLeft);
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_POS_X, alongY, hs, aRight, kh, gatheredCover,
                       gatheredSeg, hRight, bRight, haloW * hRight, bloomW * bRight);
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_NEG_Y, alongX, ws, aTop, kh, gatheredCover,
                       gatheredSeg, hTop, bTop, haloW * hTop, bloomW * bTop);
    addStraightGlowFix(glowFix, GLOW_COVER_BAND_POS_Y, alongX, ws, aBot, kh, gatheredCover,
                       gatheredSeg, hBot, bBot, haloW * hBot, bloomW * bBot);

    // The four corner arcs, each developed onto its own tangent - see
    // arcTangentSegment. Gated because at cornerRadius 0 there is nothing to
    // model: `straight` is then halfSize, the four segments above already run
    // corner to corner, and each arc term would contribute a zero-length
    // segment. uCornerRadius is a UNIFORM, so this branches uniformly and the
    // sharp-cornered case pays nothing for the four atans behind it.
    //
    // A fragment's offset from each arc centre, in that corner's own outward
    // frame, is (+/-|vPos.x| - straight.x, +/-|vPos.y| - straight.y): the two
    // axes are the incident edges' outward normals, so folding through abs()
    // puts every corner in the same first-quadrant form arcTangentSegment
    // expects, with the sign pair selecting which corner.
    if (uCornerRadius > 0.0)
    {
        vec2 wNear = abs(vPos) - straight;
        vec2 wFar  = -abs(vPos) - straight;

        // Which corner each arc is, for its coverage table: the folded frames
        // above unfolded by the corner's sign pair. N is the fragment's own
        // side of an axis, F the opposite one.
        vec2 sN    = vec2(vPos.x >= 0.0 ? 1.0 : -1.0, vPos.y >= 0.0 ? 1.0 : -1.0);

        // One shared pedestal for all four arcs, and unlike the straights'
        // it does not have to be per-piece. A pedestal is that piece's own
        // bloom evaluated at `reach`, and an arc's developed extent is the same
        // lam*HALF_PI wherever the fragment sits at a given distance - only its
        // offset along the tangent varies with position AROUND the arc. So
        // evaluating that extent CENTRED (t = -L/2 .. +L/2) leaves an
        // expression in uniforms alone: exact for every fragment that faces an
        // arc from `reach`, and an over-subtraction only for ones off to its
        // side, where the arc term is small and the clamp below takes it to
        // zero anyway.
        //
        // Why the straights cannot do this, from the other direction: their
        // half-length routinely EXCEEDS `reach`, so their pedestal swings with
        // the fragment's position along the edge. Sharing one there is what
        // killed the exterior tail 300 px early - section 1.5.1 of
        // docs/corner-crease-and-filament-nyquist.md.
        //
        // What it costs, measured on the worst case there is - a CIRCLE
        // (cornerRadius == halfMin), which is four arcs and no straights, so
        // nothing else carries an exact pedestal. Against a per-arc pedestal
        // the exterior tail ends at 506 px instead of 644 from a 200 px
        // emitter, and the lit fraction runs 0.589 against 0.638. The whole
        // difference sits in values of 4/255 and below, and the largest
        // adjacent-pixel step is 1/255 either way, so the tail ends sooner
        // rather than being chopped - which is the failure 1.5.1 was about.
        // On a rounded RECT the straights' own pedestals dominate and the
        // difference is 0.1% of the lit fraction.
        //
        // Do NOT collapse this further to bw*L/c^2 (atan(x) -> x). That is a
        // ~7% over-subtraction at the largest arc in range, and with the clamp
        // sitting right underneath it, 7% of the pedestal took the same circle
        // to 0.576. (The lam below makes the atan's argument LARGER than the
        // arclength form did, so the linearisation is worse here, not better.)
        //
        // Measured: one atan here instead of four more bloomSegments takes the
        // neon pass from 12.2 ms to 10.5 ms on the rounded cases below, and -
        // because it is the register pressure of this block that decides it -
        // takes the SHARP cases, which never execute it, back to parity.
        //
        // The developed length is lam*HALF_PI rather than the arclength, and
        // lam is per-fragment, so the centred evaluation is no longer in
        // uniforms alone by itself. It becomes so again by pinning lam to the
        // value a fragment AT `reach` from the arc would carry: such a
        // fragment sits reach + r from the arc centre, and out there lam is
        // sqrt(rho*r). That is the only place the pedestal is meant to be
        // exact, and everywhere else it was already an approximation.
        float arcC        = sqrt(reach * reach + bw * bw);
        float arcLamPed   = sqrt((reach + uCornerRadius) * uCornerRadius);
        float arcPedestal = uCornerRadius / arcLamPed *
                            bw / arcC * 2.0 * atan(arcLamPed * HALF_PI / (2.0 * arcC));

        // Each arc in turn - developed, its halo and bloom, its coverage read -
        // see addCornerPiece. The two sums run in the order the four-term
        // expressions they replaced did, so a fully lit ring is unchanged bit
        // for bit.
        vec2 cornerGlow = vec2(0.0);
        addCornerPiece(cornerGlow, glowFix, vec2( sN.x,  sN.y), vec2(wNear.x, wNear.y), kh, bw, arcPedestal,
                       gatheredCover, gatheredSeg, haloW, bloomW);
        addCornerPiece(cornerGlow, glowFix, vec2( sN.x, -sN.y), vec2(wNear.x, wFar.y),  kh, bw, arcPedestal,
                       gatheredCover, gatheredSeg, haloW, bloomW);
        addCornerPiece(cornerGlow, glowFix, vec2(-sN.x,  sN.y), vec2(wFar.x,  wNear.y), kh, bw, arcPedestal,
                       gatheredCover, gatheredSeg, haloW, bloomW);
        addCornerPiece(cornerGlow, glowFix, vec2(-sN.x, -sN.y), vec2(wFar.x,  wFar.y),  kh, bw, arcPedestal,
                       gatheredCover, gatheredSeg, haloW, bloomW);
        halo  += cornerGlow.x;
        bloom += cornerGlow.y;
    }

    halo    *= HALO_NORM_FACTOR;
    bloom   *= BLOOM_NORM_FACTOR;
    glowFix *= vec4(HALO_NORM_FACTOR, HALO_NORM_FACTOR, BLOOM_NORM_FACTOR, BLOOM_NORM_FACTOR);

    // The pedestal itself is applied per-edge inside the sum above - see
    // bloomSegmentPedestalled for why it cannot be one shared subtraction any
    // more. What is left here is the RENORMALISATION: scale the pedestalled
    // sum back up by peak/(peak - pedestal) so the value on the line is
    // unchanged and BLOOM_NORM_FACTOR keeps its calibration, with the tail
    // slightly compressed in exchange for going cleanly to zero.
    //
    // The gain still uses the infinite-line pedestal. On the line the nearest
    // edge is the whole of the sum to within a few percent, and its own
    // pedestal differs from this one only by how much of that edge is cut off -
    // a few percent again, against a tuning constant. Deriving it from the
    // near edge's own extents would mean branching to find which edge that is,
    // on the one term where it would buy nothing visible.
    //
    // The halo needs no pedestal at all: it falls as 1/a^2, so at `reach` it is
    // ~2e-4 of peak - already invisible.
    bloom      = bloom * bloomGain;
    glowFix.zw = glowFix.zw * bloomGain;

    // glowRadius == 0 must read as "filament only", but an analytic profile at
    // radius 0 is a sub-pixel spike of FULL height rather than nothing, so
    // both layers fade in over glowRadius = [0, GLOW_GATE_FADE_PX]. Measured
    // against a fixed pixel width: gating against the sampleSpacing-derived
    // floor instead would re-couple brightness to the rect size. (The bloom is
    // gated too now - the gather's spacing floor used to keep it finite here.)
    // Full-res constant against a scaled uGlowRadius, so it is converted like
    // the feathers above - identity at scale 1.0.
    float glowGate = clamp(uGlowRadius / (GLOW_GATE_FADE_PX * uResolutionScale), 0.0, 1.0);

    // Compose: base arc x intensity + segments (independent of intensity, so
    // a segment stays lit even on a dark arc - the whole point of the
    // additive segment model).
    //
    // EACH SOURCE CARRIES ITS OWN COVERAGE. `col` is gated-normalised up in the
    // gather, which makes it a pure hue of unit magnitude EVERYWHERE on the
    // quad - it no longer decays with distance from the lit arc, because the
    // coverage that used to ride in it moved to emitCover. So it must be
    // multiplied by emitCover here. Summing the two sources first and applying
    // one shared gate (the old `lightCol * filamentGate`) let the segment's
    // gate lift the arc term on a stretch NO arc covers: a blue arc over half
    // the ring plus a red segment on the other half rendered the segment
    // magenta at ~2x brightness, and violet - arc-dominant - on its shoulders.
    //
    // The segment keeps the gates it already had, so nothing about the common
    // "tracer running along a lit arc" case moves: with an arc covering, both
    // emitFil and emitGlow reduce to exactly the old expression. Only the paths
    // where the two coverages DISAGREE change, which is the bug.
    vec3 arcCol = col * uIntensity;

    // filamentGate is the segment's SHARP gate (smoothstep 0.5..1) maxed with
    // emitCover; segmentGlow's is the soft one. Applied to the segment term
    // only - the arc takes its own coverage directly in both, since for an arc
    // the two gates were just that coverage anyway.
    //
    // THE TWO TAKE DIFFERENT COVERAGES, and that is the point. The filament is
    // an SDF-derived line: it lives ON the perimeter, so it wants the
    // POINTWISE pair, exact at this fragment's own perimeter position. The
    // halo and bloom are integrals over the whole emitter, so they want the
    // GATHERED pair - plus glowFix, V19's per-piece correction toward the
    // coverage at each piece's foot, in the same arc / segment halves.
    vec3 emitFil  = arcCol * emitCover           + segCol     * filamentGate;
    vec3 emitGlow = arcCol * gatheredCover.x     + segColHue  * gatheredSeg;

    vec3 result  = emitFil  * core  * FILAMENT_GAIN  * lineGate;
    result      += (emitGlow * halo  + arcCol * glowFix.x + segColHue * glowFix.y) * HALO_GAIN      * glowGate;
    result      += (emitGlow * bloom + arcCol * glowFix.z + segColHue * glowFix.w) * uBloomStrength * glowGate;

    // NOTE neither the one-sided cut NOR the hard cutoff masks are applied
    // here. Both are COVERAGE, not emission, so both belong below the grade -
    // see the block after the tone map. The quad-edge fade below stays, and is
    // the one mask that genuinely shapes emission: it hides a clip rather than
    // drawing an edge, and it is tens of pixels wide.

    // --- Quad-edge fade: the draw quad ends uQuadMargin past the rect ON EACH
    // AXIS. Fade the emission to zero over the last stretch so a strong bloom
    // never shows a hard rectangular cutoff where the quad clips it. Interior
    // pixels sit far inside the quad, so they're unaffected.
    //
    // Measured PER-AXIS via dQuad, NOT from the Euclidean d. What this fade
    // hides is the quad, and the quad is a rectangle; keying on d put the ramp
    // on a rounded contour that ate the corners early:
    //
    //   - At cornerRadius 0 the band is per-axis too (see bandOuterDistance),
    //     so it reaches d = sqrt(2) * outsideCutoff at a corner while
    //     setupGeometry clamps uQuadMargin to cutoff + softness + 1. On the
    //     stock 800x600 rect at cutoff 12 the band ran out to d = 16.97 but the
    //     d-keyed ramp was already zero by d = 14, erasing the outer ~30% of
    //     every corner - while black-rect.frag, drawn on a fullscreen quad with
    //     no fade at all, kept its square corner. That is exactly the black
    //     bulge past the glow the r == 0 branch exists to prevent.
    //   - With the cutoff disabled the same rounding chopped the bloom at
    //     d = uQuadMargin though the quad ran on to its own corner, so corners
    //     faded sooner than edges for no reason.
    //
    // dQuad is 0 on the quad edge and negative inside, so the ramp runs over
    // [-(uQuadMargin - fadeStart), 0]. On a straight edge dQuad == d -
    // uQuadMargin, so that stretch is bit-identical to the old expression.
    //
    // fadeStart is unchanged. The ramp must not begin INSIDE the outside
    // cutoff, or it dims the band's outer edge before the cutoff mask above
    // ever gets there - and only on the exterior, because the interior half of
    // a symmetric band never reaches the quad. An opaque-INSIDE vs
    // opaque-OUTSIDE pair sharing an outer rect is what exposes it: at cutoff
    // 12, uQuadMargin is 13, so a bare fraction of the margin started the ramp
    // at 10.4 and left the outermost band pixel at ~0.66 of its mirrored
    // counterpart (15.3 vs 23.3 on the right edge, same ratio on the other
    // three). Flooring the start at the cutoff boundary hands everything up to
    // that point back to the cutoff smoothstep.
    //
    // Only when that boundary actually falls inside the quad, though. A
    // disabled cutoff arrives as a huge sentinel, and the whole point of this
    // fade is the case where uQuadMargin is SMALLER than outsideCutoff - in
    // both, flooring would push the start to or past uQuadMargin. Those keep
    // the unfloored start, so fadeStart < uQuadMargin always holds and the ramp
    // width below is strictly positive (an inverted smoothstep is undefined in
    // GLSL).
    float fadeFloor = uQuadMargin * QUAD_FADE_START_FRAC;
    // Where the band's emission ends: the outside fade is centred on outMid
    // and reaches outHalf past it, and the discard above culls everything
    // beyond - cutGuard further out on the scaled path, whose guard texels
    // must reach the blit unfaded or its rebuilt boundary darkens.
    // NeonRenderer::setupGeometry caps uQuadMargin at the same end
    // (GetCutoffEnd, plus the same guard) plus 1 full-res px, so while that
    // cap is in force cutEdge < uQuadMargin holds by that pixel, the fade
    // starts at the end, and nothing it dims survives the discard. It used to
    // budget a whole outSoft past outMid, against a cap that did the same; the
    // two moved together, and only past the discard.
    float cutEdge   = outMid + outHalf + cutGuard;
    float fadeStart = (cutEdge < uQuadMargin) ? max(fadeFloor, cutEdge) : fadeFloor;
    float dQuad     = sdRoundBox(vPos, halfSize + vec2(uQuadMargin), 0.0);
    result *= 1.0 - smoothstep(-(uQuadMargin - fadeStart), 0.0, dQuad);

    // --- Grade --------------------------------------------------------
    // Hue-preserving Reinhard: tonemap the peak channel and scale the
    // others by the same ratio. Per-channel tonemap desaturates warm mixes
    // (orange -> peach) because R saturates while G/B are still linear;
    // scaling by the peak's compression preserves the original R:G:B ratio.
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    result = pow(result, vec3(GAMMA_EXPONENT));

    // --- One-sided cut: mask the WHOLE layer at the line --------------
    // Anchored at the opaque fill's own edge and feathered INTO the lit side -
    // see the derivation of sideAA / sideSoft / sideBack at the top of main().
    //
    // BELOW THE GRADE, and that placement is the whole point of this block.
    // The cut is a COVERAGE boundary - "how much of this pixel is on the lit
    // side" - not a dimming of the emission, so it has to scale the value that
    // actually reaches the framebuffer. Multiplied into the linear emission
    // ABOVE the tone map instead, it was very nearly annihilated by it: the
    // filament core runs FILAMENT_GAIN times the Reinhard shoulder, so
    // peak/(peak + TONE_MAP_SHOULDER) maps a HALF-covered pixel to 94% of a
    // fully covered one and hands back most of what the mask took.
    //
    // Measured on the identical sub-pixel sweep - the rect edge walked across
    // one pixel in 1/8 px steps, reading the single pixel that straddles it,
    // with black-rect.frag's own d == 0 edge rendered beside it for reference:
    //
    //   d(straddle)   +0.500  +0.375  +0.250  +0.125   0.000  -0.250  -0.375
    //   cut, above       239     238     236     233     225     180     104
    //   cut, below       239     229     202     163     120      37      10
    //   opaque fill      255     223     191     159     128      64      32
    //   1 px box filter  239     209     179     149     120      60      30
    //
    // The last row is the exact analytic coverage, for scale. The shipped cut
    // is a smoothstep, so it meets the box filter at the ends and at half
    // coverage and rides up to ~0.08 coverage away from it in the shoulders -
    // the same difference black-rect.frag prices as "not resolvable" where it
    // chose a linear ramp for its own d == 0 edge and a smoothstep for its
    // wider ones. What matters is that it tracks the FILL row, which is the
    // edge it has to register with, rather than the row above it.
    //
    // The row below the grade IS the 1 px box filter, and it tracks the fill it
    // has to register with; the row above it is four pixels of nothing followed
    // by a cliff, which is the stair-step the floor was added to remove.
    //
    // A non-zero uGlowSideSoftness was as badly served, and less visibly. The
    // tone map re-lifted the middle of the feather harder than its ends, so the
    // ramp was not monotonic: OUTSIDE at softness 4 read 179, 215, 212, 186 on
    // the first four pixels - a "feather" that brightens for two pixels before
    // it fades. INSIDE at softness 20 (the demos' slider maximum) read 30, 65,
    // 73, 61, 49, 46, 49, 54 going inward, a smear with a ridge in it rather
    // than a fade. Both are monotonic below the grade.
    //
    // ALPHA follows for free and had the same bug: `alpha` is peak(result)
    // taken right after this, so a half-covered pixel used to occlude the
    // background at 0.94 instead of 0.5. That is a compositing error, not a
    // look preference - it is exactly the premultiplied-coverage contract the
    // note below states, and a host blending this layer over video sees it.
    //
    // Written as `1.0 - smoothstep(lo, hi, d)` for the INSIDE arm rather than
    // smoothstep(hi, lo, d). The descending form has edge0 > edge1, which GLSL
    // leaves UNDEFINED; it happens to work on desktop drivers, it was the only
    // reversed-edge smoothstep in these shaders, and the quad fade above
    // already documents the rule. Same curve, portably.
    //
    // DIRECT PATH ONLY, for the reason given at sideBack: a cut applied to
    // buffer texels cannot survive the bilinear upsample, so below scale 1.0
    // neon-blit.frag applies this exact expression against ITS own fwidth
    // instead, and the cull above leaves it a guard band to work from. Keep
    // the two in step - they are one edge, written twice because only one of
    // them ever runs.
    if (!blitOwnsCut)
    {
        if (uGlowSide == GLOW_SIDE_INSIDE)       result *= 1.0 - smoothstep(sideBack - sideSoft, sideBack, d);
        else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-sideBack, sideSoft - sideBack, d);
    }

    // --- Hard cutoff masks: close the band at its two boundaries ------
    // The band is [-uInsideCutoff, +uOutsideCutoff]; these fade the layer out
    // over the per-side softness at each end, so bloom and halo never punch
    // past the artist's stated reach even if uGlowRadius says otherwise. In
    // the band means outside the shrunk rect (dIn >= 0) and inside the grown
    // one (dOut <= 0). Disabled sides push their boundary to a huge sentinel,
    // so the smoothstep evaluates to a pass-through 1.0.
    //
    // BELOW THE GRADE for the same reason the one-sided cut above is: a cutoff
    // boundary is a geometric limit, so the ramp across it is coverage.
    //
    // THE TWO HALVES OF THIS FIX ARE NOT EQUALLY IMPORTANT HERE, and the split
    // is the opposite of the one-sided cut's. Sweeping the boundary across one
    // pixel in 1/8 px steps, full res, softness 0, against a full-brightness
    // 132 at that distance:
    //
    //   no floor, above grade    0    0    0    0    0  132  132  132
    //   floor, above grade       0   15   41   68   91  109  122  129
    //   floor, below grade       0    6   21   42   66   90  111  126
    //
    // The FLOOR is what removes the staircase; the placement then corrects a
    // brightness bias, 69% of full at half coverage down to the 50% the last
    // row reads. That last row is an exact smoothstep - the mask delivered as
    // authored.
    //
    // The placement matters less here than it did at the filament because the
    // tone map is only violent where peak >> TONE_MAP_SHOULDER, and a band
    // EDGE is by construction the dim end of the glow. Which is also why the
    // floor alone was never going to be the whole answer: the same boundary
    // crossing a bright stretch - a tight outsideCutoff over a hot filament -
    // sits back in the compressed region where the one-sided cut's numbers
    // apply.
    //
    // Ramps span inSoft / outSoft TOTAL, centred on inMid / outMid - hence the
    // halves, and see where they are derived for what the doubled form cost.
    // dIn / dOut are already measured from those midpoints, so each ramp
    // starts at its cutoff and ends softness past it.
    //
    // DIRECT PATH ONLY, for the reason the one-sided cut above is: a mask
    // applied to buffer texels is smeared across 1/scale destination pixels by
    // the bilinear upsample, which softened a hard band edge by a buffer texel
    // and snapped it toward the buffer grid. Below scale 1.0 neon-blit.frag
    // applies these same two ramps at destination resolution, and the discards
    // above leave it BLIT_CUTOFF_GUARD_PX of lit texels to work from. Keep the
    // two in step - one edge, written twice because only one of them runs.
    if (!blitOwnsCut)
    {
        result *= smoothstep(-inHalf, inHalf, dIn);
        result *= 1.0 - smoothstep(-outHalf, outHalf, dOut);
    }

    // Premultiplied-alpha output so the effect composites over arbitrary
    // background objects instead of only adding light. Coverage = brightest
    // channel: the hot filament core (alpha ~ 1) occludes the background and
    // reads as a solid tube; the dim halo/bloom (alpha ~ 0) stay additive; the
    // dark surround (alpha = 0) leaves the background untouched. Pairs with
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) in the renderer.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
