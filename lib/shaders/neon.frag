// precision, vPos, uRectSize / uCornerRadius, the segment block and PI are in
// neon-common.glsl, which is injected ahead of this file and neon-gather.frag;
// the perimeter gather itself (gatherPerimeter) is neon-gather.frag's.

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

// The SHADING: everything except the perimeter gather, which neon-gather.frag
// runs as a pass of its own at every scale and this file reads back with a
// bilinear fetch. Built into TWO program objects, one per target: pass 1b (at
// resolutionScale, into the reduced buffer) and the edge ring (at full
// resolution, onto the target), at every scale - 1.0 included, which takes
// the same path with a full-size reduced buffer. See
// NeonRenderer::ensurePathPrograms. The one variant NeonRenderer
// splices in after the version line (WithDefine) is NEON_FIELD_BAKE, the
// hue-invariant field's bake; see neon-field.frag.
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
// Non-zero when nothing below reads the fragment's perimeter position: no
// segments, every lit arc over the whole ring with no stops of its own, and a
// gradient ring whose alpha is 255 at every texel. Then sPos and the
// gradient's alpha read are skipped - perimeterPosition is an atan and a
// cascade of branches, ~9% of a default frame - and the alpha is the 1.0 the
// read would have returned. Set by the CPU (IsPerimeterUnread), which tests
// the arcs' length a hair more strictly than arcCoverContinuous does, so it
// can never claim this where a read would happen.
uniform int   uPerimeterUnread;
// Non-zero on a ring lit uniformly - one arc over the whole ring, no segments
// - where every piece's own coverage IS the gathered one and the per-piece glow
// fix adds nothing (uniformCover in main()). Set by the CPU from
// IsGlowCoverUnread, the same test that skips baking the coverage table, so the
// two can never disagree about whether the table is current.
uniform int   uUniformCover;

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

// 1 for a pass whose output the blit composites - pass 1b, and the field bake
// that stands in for it - which leaves the one-sided cut and the cutoffs to
// the blit; 0 for the edge ring and its field bake, which draw the frame's own
// pixels and apply both after the grade. Not the scale: at 1.0 pass 1b still
// goes through the blit.
uniform int uBlitOwnsCut;

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
// table uEmission - are neon-gather.frag's alone: this file reads the gather's
// RESULT instead of running it, so it has no loop to feed.

// The gather - neon-gather.frag, its own pass at its own coarse scale
// (GetGatherScale) - stores four results this file reads back instead of
// running the loop: pass 1 at resolutionScale, the edge ring at full resolution
// (the filament and every hard edge, which a reduced buffer cannot carry).
// They are Lorentzian-weighted means whose kernel
// is never narrower than kc, so they survive a bilinear read from a grid a
// fraction of kc apart.
//
// uGather / uGatherSeg are the gather buffer's attachments 0 and 1 (uGatherSeg
// read only with segments). The buffer covers a region (GetBufferRegion),
// mapped from vPos by an affine uv map set for the space this pass's vPos is
// in.
uniform sampler2D uGather;
uniform sampler2D uGatherSeg;
uniform vec2      uGatherUVScale;
uniform vec2      uGatherUVOffset;

// Distance (in pixels, from the rect edge) to the draw quad's edge. The whole
// emission is faded to zero just before this, so the bloom never shows a hard
// rectangular cutoff where the quad clips it - independent of bloom strength.
uniform float uQuadMargin;

// Distance (in pixels, from a corner arc's circle, outward) past which that
// arc's halo and bloom are skipped: provably under a quarter of half an 8-bit
// level each. See addCornerPiece and NeonRenderer::GetCornerSkip.
uniform float uCornerSkip;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// PI, TWO_PI and HALF_PI are in neon-common.glsl. sdRoundBox and the band
// boundaries (bandOuterDistance, bandInnerDistance) are in neon-sdf.glsl,
// shared with neon-blit.frag and black-rect.frag; the tone map, neonToneMap,
// in neon-grade.glsl.

// --- Finite-segment halo / bloom ---------------------------------------
// The halo and bloom kernels integrated along a STRAIGHT SEGMENT: `a` the
// perpendicular distance to its line, t1 / t2 its endpoints along it from the
// fragment's foot. As t1 -> -inf, t2 -> +inf they reduce to the infinite-line
// forms HALO_NORM_FACTOR / BLOOM_NORM_FACTOR were calibrated against, so the
// sum over the pieces generalises the old nearest-edge term rather than
// retuning it - and removes its medial-axis creases.
float haloSegment(float a, float t1, float t2, float k) {
    float c2 = a * a + k * k;
    return k * k / c2 * (t2 / sqrt(c2 + t2 * t2) - t1 / sqrt(c2 + t1 * t1));
}
// ONE atan, not the difference of two: atan(c * (t2 - t1), c^2 + t1 * t2) is
// the angle the segment subtends, exactly (c > 0 and t2 >= t1 keep it in [0,
// PI]). The costliest term in the shader; the identity halves its atans
// (1.13-1.26x on the shading), and the atan is minimaxAtan (neon-pieces.glsl),
// not the driver's (I48).
float bloomSegment(float a, float t1, float t2, float k) {
    float c = sqrt(a * a + k * k);
    return k / c * minimaxAtan(c * (t2 - t1), c * c + t1 * t2);
}

// The bloom's 1/a tail is pedestal-subtracted to reach zero at the draw quad's
// edge. Each edge carries its OWN pedestal - the same segment evaluated at
// `reach` - so its contribution lands on zero at `reach` from it; one shared
// infinite-line pedestal over-subtracts a finite segment and cut the exterior
// tail short. The two edges of a pair span the same t1 / t2, so main() computes
// each pair's pedestal once.
//
// SKIPPED past `reach`, where it is exactly zero: bloomSegment falls
// monotonically with `a` for fixed t1 / t2, so the max() would clamp it to 0
// anyway. Byte-identical.
float bloomSegmentPedestalled(float a, float t1, float t2, float k, float reach, float pedestal) {
    return (a < reach) ? max(bloomSegment(a, t1, t2, k) - pedestal, 0.0) : 0.0;
}

// --- Corner arcs, developed onto their tangent -------------------------
// arcTangentSegment is in neon-pieces.glsl, injected ahead of this file: the
// glow coverage bake (neon-glow-cover.frag) lays each corner's coverage along
// the same line this file integrates the corner's halo and bloom along, so the
// two share one copy.

// Exact per-fragment perimeter position: this fragment's nearest perimeter
// point mapped back to its arc-length parameter t in [0, 1), matching the CPU's
// GeometryUtils::GetPointOnRectangle for BOTH windings (uWinding 0 / 1).
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
// which gates the sharp SDF filament. Feathered PER ENDPOINT, from the abut
// flags packed into uArcs[].w (packLightBlocks):
//
//   FREE endpoint     -> ramp INWARD: exactly 0 at the endpoint,
//                        so nothing outside the arc is lit.
//   ABUTTING endpoint -> ramp OUTWARD into the neighbour: 1 at the
//                        endpoint, so max() over two arcs hands over
//                        at full brightness with no notch.
//
// Both halves are needed: inward everywhere notches the seam between tiling
// arcs, and outward at a free endpoint lights a whole exterior quadrant at a
// sharp corner, where every fragment of the wedge shares one sPos. Feather
// widths arrive as perimeter fractions (converted at the call site).
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
//
// Without segments the table may have only two channels (RG16F, half the
// memory): its .b / .a would be zero anyway, but a two-channel texture samples
// .a as 1, which the decode turns into 1024. So they are zeroed here instead -
// on a uniform, and exactly what a four-channel table holds there.
vec4 glowCoverAt(vec2 uv) {
    vec4 e = textureLod(uGlowCover, uv, 0.0);
    e.ba = (uSegmentCount > 0) ? e.ba : vec2(0.0);
    return e / max(1.0 - e, vec4(1.0 / 1024.0));
}

// V19, V20 and V21: ONE PIECE's halo and bloom scaled by that piece's OWN
// coverage rather than the gathered mean. The exact term is INTEGRAL cover(s) *
// K(|p - P(s)|) ds over the piece; the table (glowCoverAt) holds its ratio to
// INTEGRAL K over the same piece, so times the piece's own halo or bloom it is
// exact - per piece, continuous in the fragment's position, so nothing switches
// at the medial axis.
//
// Applied as a CORRECTION to the gathered-mean scaling: each piece adds only
// the difference its own coverage makes, times its halo `h` and bloom `b`, to
// `fix` = (halo arc, halo segment, bloom arc, bloom segment). A skipped piece
// costs one compare. Keep what runs here small: earlier exact forms evaluated
// in this function cost 1.2-1.7x on EVERY scene through the program's register
// use.
//
// `haloWeight` / `bloomWeight` are the piece's halo and bloom as they reach the
// output, for the GLOW_PIECE_MIN test; the caller zeroes both on a ring lit
// uniformly, where the correction is exactly 0. `gatheredSeg` is
// segmentGlow(gathered).
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
// halo and its bloom against the arcs' shared `pedestal`, added to `sum` (halo,
// bloom), and its own coverage correction.
//
// ONE ARC AT A TIME, AND THAT IS LOAD-BEARING. Developing all four arcs first
// kept sixteen floats live across the block and made every scene slower,
// sharp-cornered and fully lit ones included (measured history in
// docs/neon-frag-notes.md, from when this program also carried the gather
// loop). Keep the four calls whole.
//
// SKIPPED, whole, past uCornerSkip from the arc's circle: every developed point
// is at least length(w) - r away, where NeonRenderer::GetCornerSkip's bound
// puts the bloom at exactly 0 and the halo under a quarter of half an 8-bit
// level - four skipped arcs stay under half a level. The concave side, where
// length(w) < r, is never skipped. Change the halo or bloom terms and
// GlowBoundTerms has to follow.
void addCornerPiece(inout vec2 sum, inout vec4 fix, vec2 signs, vec2 w, float kh, float bw, float pedestal,
                    vec2 gathered, float gatheredSeg, float haloW, float bloomW) {
    if (length(w) - uCornerRadius >= uCornerSkip) {
        return;
    }
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

    // The far exterior is culled on the CPU: the draw quad is sized to rect +
    // glowReach (NeonRenderer::setupGeometry), with a hole cut for
    // GlowSide::OUTSIDE, an inside cutoff, or the depth past which the output
    // is provably under half a level (GetGlowInnerReach). The geometry is
    // conservative by a few px; the discards below place the edge exactly. So
    // glowSide and insideCutoff are inputs to the QUAD, and in setupGeometry's
    // dirty gate.
    //
    // The one-sided cut's antialiasing width: one DESTINATION pixel in this
    // shader's units - fwidth(d) is one buffer pixel for an SDF, and
    // uResolutionScale converts it to the pixel the opaque fill (always
    // full-res) registers with. Computed HERE, above every discard, because a
    // derivative downstream of non-uniform control flow is undefined. This is
    // the only derivative in this shader - keep it at the top if a second one
    // is ever needed.
    float sideAA = max(fwidth(d) * uResolutionScale, 1e-6);

    // TOTAL feather width of the one-sided cut, floored at that pixel - the
    // floor is what antialiases the cut, and only because the ramp is applied
    // BELOW the tone map, as coverage.
    float sideSoft = max(uGlowSideSoftness, sideAA);

    // How far the ramp reaches back across the line: half a pixel, exactly as
    // far as the fill's own box filter reaches (black-rect.frag), so the two
    // share one ramp. The cut is ANCHORED at the line and feathers INTO the lit
    // side, never back past the fill.
    //
    // THE RING ONLY. A buffer texel lit at its centre is smeared over 1/scale
    // destination pixels both ways by the blit, so for pass 1b neon-blit.frag
    // applies the cut from ITS fwidth, and what is left here is the CULL,
    // BLIT_SIDE_GUARD_PX past the cut so the blit has lit texels to rebuild
    // the boundary from (neon-tuning.h).
    bool  blitOwnsCut = (uBlitOwnsCut != 0);
    float sideBack = 0.5 * sideAA;
    float sideCull = blitOwnsCut ? BLIT_SIDE_GUARD_PX : sideBack;
    // Deliberately NOT mirrored in neon-gather.frag - see the note at its top.
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  sideCull) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -sideCull) discard;

    // Hard geometric cutoffs. The band is [-uInsideCutoff, +uOutsideCutoff];
    // each side's feather STARTS at that boundary and runs its softness beyond
    // it, and anything past the feather is culled, so the glow cannot leak past
    // the stated reach whatever uGlowRadius says. Softness is per side and
    // decoupled from uGlowSideSoftness. Disabled sides arrive as a huge
    // sentinel, so these no-op.
    //
    // The floor is an ANTIALIASING floor, one DESTINATION pixel (sideAA - no
    // second derivative) on both paths: without it a cutoff is a binary edge, a
    // staircase wherever the boundary curves. Below 1.0 the masks are the
    // blit's; here the floor only places the discards, which have to agree with
    // the blit's ramp.
    float softFloor = sideAA;
    float inSoft  = max(uInsideCutoffSoftness,  softFloor);
    float outSoft = max(uOutsideCutoffSoftness, softFloor);

    // TOTAL widths, halved because each ramp is centred on the midpoint of its
    // fade (inMid / outMid): smoothstep(-w, w, x) spans 2w, and the unhalved
    // form feathered over twice the stated softness, off from an OpaqueMode
    // fill sharing the boundary.
    float inHalf  = 0.5 * inSoft;
    float outHalf = 0.5 * outSoft;

    // WHERE each fade sits: it starts at the cutoff and runs the requested
    // softness beyond it, so its MIDPOINT is softness/2 past the cutoff, and
    // the ramps are built symmetrically about it over the floored width. Below
    // the floor the ramp stays centred on the requested fade, so at softness 0
    // the edge's 50% point is the cutoff itself at every resolution scale.
    // Disabled and neutralised sides need no branch here.
    float inMid  = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
    float outMid = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);

    // Band boundaries measured against the offset rect, so a cornerRadius-0
    // band keeps square corners.
    //
    // A CUTOFF ON THE SIDE glowSide ALREADY CULLS IS NEUTRALISED - handed the
    // same huge distance a disabled one gets - so every use below no-ops
    // through the arithmetic. It never binds the silhouette there, but it was
    // not harmless: on the scaled path it ate the blit's guard band (a dark
    // seam), and at full res it squared the shared boundary.
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
    // For pass 1b the blit owns the masks as well as the cut, and rebuilds
    // each boundary from the buffer texels around it - so the cull runs
    // BLIT_CUTOFF_GUARD_PX past the end of the ramp rather than at it, for the
    // reason sideCull does. The guard is an exact 0 for the ring, which keeps
    // culling exactly where its own masks end.
    float cutGuard = blitOwnsCut ? BLIT_CUTOFF_GUARD_PX : 0.0;
    if (dOut >  outHalf + cutGuard ) discard;
    if (dIn  < -(inHalf + cutGuard)) discard;

    // --- Filament -----------------------------------------------------
    // Generalized-Gaussian profile: core(ad) = exp(-ln(2) * (ad / sigma)^N),
    // sigma the half-brightness radius, N = 2 * uFilamentFalloff (0.5 Laplace,
    // 1.0 Gaussian - the default, 2.0 flatter top, 5.0 near-rectangular). Peak
    // at ad = 0 is exactly 1.0, with no power-law tail. lineGate fades the
    // filament in from lineWidth 0 ("no line") to FILAMENT_MIN_HALF_WIDTH * 2.
    // That constant is full-res px and uLineWidth arrives scaled, so it is
    // converted first - the identity at scale 1.0.
    float minHalf   = FILAMENT_MIN_HALF_WIDTH * uResolutionScale;
    float halfWidth = uLineWidth * 0.5;
    // Generalized-Gaussian exponent, needed before the sampling floor below
    // because that floor depends on the profile's SHAPE, not only its width.
    float N         = 2.0 * max(uFilamentFalloff, 1e-3);
    // Two floors, in different spaces on purpose. minHalf is the STATED-width
    // floor, converted like every full-res constant; it pairs with lineGate to
    // make lineWidth 0 mean no line. `nyquist` is the SAMPLING floor, in BUFFER
    // px and NOT converted: below 1.0 a filament the reduced buffer cannot
    // sample does not survive the blit.
    //
    // It is not a fixed half width: sigma must leave the profile at
    // FILAMENT_NYQUIST_MIN_SHARE of its peak FILAMENT_NYQUIST_SAMPLE_PX out -
    // what the bilinear filter reconstructs the peak from - and inverting
    // core() for sigma gives the expression. 0.5 buffer px at the default
    // falloff, under what the caller asked for at a soft one. Gated to the
    // scaled path: at 1.0 the filament stays exactly the width it was asked
    // for. NeonRenderer::setupGeometry mirrors both the expression and the gate
    // when it sizes the quad. See docs/corner-crease-and-filament-nyquist.md
    // section 2.8.
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
    //    (neon-gather.frag) as a fraction of the PERIMETER rather than in
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
    // The gather's four results, read back from what neon-gather.frag stored:
    // the base and segment hues, each of unit magnitude, and the two g-weighted
    // mean coverages that scale the glow. COLOUR ONLY - the halo and bloom are
    // closed forms below.
    //
    // textureLod at level 0, not texture(): the discards above make control
    // flow non-uniform here, where an implicit derivative is undefined. The
    // coverages are stored as e = c / (1 + c) (neon-gather.frag) and decoded
    // after the bilinear read; the floor keeps a saturated texel finite. The
    // two sides change together.
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
#ifdef NEON_FIELD_BAKE
    // The hue-invariant field (neon-field.frag). Everything below is linear in
    // the two hues - the output is col * Fa + segColHue * Fs before the tone
    // map - so with the arc hue on red alone and the segment hue on green
    // alone, .r computes Fa and .g computes Fs in one draw. .r is the same
    // expression as with col at 1 and segColHue 0, bit for bit; without
    // segments segColHue stays 0, so .g is 0 and the R16F field drops it.
    col = vec3(1.0, 0.0, 0.0);
    if (uSegmentCount > 0) {
        segColHue = vec3(0.0, 1.0, 0.0);
    }
#endif

    // --- Continuous coverage, read at this fragment's own position -------
    // Everything in this block - sPos, the pointwise alpha, emitCover and
    // segCoverPt - feeds ONE term, the filament (emitFil * core * lineGate,
    // below), and `core` is exactly 0 past the filament's reach. So off the
    // line - most of the glow quad - none of it is computed: the term is 0 * a
    // finite value either way, so the output is the same bits. Add a reader of
    // any of the four outside the filament and it has to move out of this gate.
    // The LUT reads are explicit-LOD for the same reason: they now sit in
    // non-uniform control flow, and every LUT has a single level.
    bool filamentLit = core > 0.0 && lineGate > 0.0;
    // The fragment's own perimeter position, recovered geometrically from vPos,
    // with each arc read directly there - exact at the corners too. Skipped
    // where nothing reads it (uPerimeterUnread).
    float sPos = 0.0;
    if (uPerimeterUnread == 0 && filamentLit)
    {
        sPos = perimeterPosition(vPos);
    }
    // Inward feathers: convert pixel widths to perimeter fractions at the
    // current geometry (`peri` is computed above the gather). `peri` is derived
    // from uRectSize and so is in SCALED px, while the two constants are
    // full-res - hence the conversion, which is identity at scale 1.0. Getting
    // it wrong changes the feather's width in perimeter fractions, i.e. the arc
    // ends soften over a different length at a different resolution scale.
    float headF  = HEAD_FEATHER_PX * uResolutionScale / peri;
    float tailF  = TAIL_FEATHER_PX * uResolutionScale / peri;
    // The arcs' coverage at this point, per-arc intensity folded in, for the
    // FILAMENT: `col` is gated-normalised, so intensity reaches the filament
    // only through emitCover. The halo and bloom take the gathered coverage and
    // each piece's table instead (see Compose below) - which is what lets this
    // whole block sit behind filamentLit.
    //
    // Colour-stop ALPHA rides here too, on the magnitude and POINTWISE: folded
    // into `col` it would cancel, and gathered it would be a ring-wide mean
    // dragged toward the opaque far side. Alpha 0 kills the filament at that
    // position (the glow does not see stop alpha - V18), and the premultiplied
    // output alpha follows.
    float baseAlphaPt = 1.0;
    if (uPerimeterUnread == 0 && filamentLit)
    {
        baseAlphaPt = textureLod(uGradientLUT, vec2(sPos - uTime * uHueRotationRate, 0.5), 0.0).a;
    }
    // Winner-take-all across arcs, as documented for overlap. This can be a
    // plain max() again because arcCoverContinuous now reaches a FULL 1.0 at an
    // abutting endpoint rather than 0 (inward) or 0.5 (straddling), so two arcs
    // tiling the ring hand over at max(w1, w2) with no notch - and because
    // their ramps overlap, the handover stays smooth even when w1 != w2.
    float emitCover = 0.0;
    int   arcCount  = filamentLit ? uArcCount : 0;
    for (int a = 0; a < arcCount; a++) {
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
            // NO hue-rotation term here: uArc is the arc's OWN head-to-tail
            // coordinate, so there is nothing to rotate (it slid the gradient
            // off one end and seamed it mid-edge). An arc's gradient moves by
            // moving the arc or animating its stops, as a segment's does.
            //
            // WRAPPED, as arcCoverContinuous wraps its rel - an arc may
            // straddle the seam - with the same midpoint split, so a fragment
            // behind the start clamps to the head colour.
            float rowY = (float(a) + 0.5) / float(MAX_ARCS);
            float rel  = sPos - arc.x;
            rel       -= floor(rel);                       // wrap to [0, 1)
            if (rel > 0.5 * (1.0 + arc.y)) { rel -= 1.0; } // behind the start, not past the head
            float uArc = rel / max(arc.y, 1e-4);
            aA         = textureLod(uArcLUT, vec2(uArc, rowY), 0.0).a;
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
    int   segCount   = filamentLit ? uSegmentCount : 0;
    for (int s = 0; s < segCount; s++) {
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
            sA           = textureLod(uSegmentLUT, vec2(tLocal, rowY), 0.0).a;
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
    // The halo and bloom integrate over the WHOLE emitter, so what scales them
    // is the emitter's coverage averaged over that integral, not the nearest
    // perimeter point's: sPos is a nearest-point map that JUMPS across the
    // medial axis, and a smooth field scaled by it creases there (V4 one level
    // up). The gather's ratio SUM(cover * g) / SUM(g) is that mean - smooth by
    // construction, and exactly 1.0 on a fully lit ring.
    //
    // Colour-stop alpha is NOT in this pair (V18): the glow does not see it. On
    // the line the gathered mean is too wide (V19), so the halo block corrects
    // it per piece toward each piece's own coverage - see addPieceGlowFix.
    vec2 gatheredCover = vec2(emitCoverGathered, segCoverGathered);

    // Sharp gate for the SDF-derived filament, from the same two pointwise
    // coverages. Both are exact at this fragment's perimeter position, so
    // neither can quantise a slow tracer's head to the gather points nor light
    // the corner preceding an arc's tail - the two bugs the old
    // circular-mean/sample-based gates had.
    float filamentGate = max(smoothstep(0.5, 1.0, min(segCoverPt, 1.0)), emitCover);

    // --- Analytic halo + bloom --------------------------------------------
    // Closed forms evaluated as a SUM OVER THE EMITTER'S PIECES - four
    // straights between the tangent points, and above cornerRadius 0 four
    // corner arcs developed onto their tangents - rather than one infinite-line
    // term at the nearest distance. Closed forms cannot bead, so glowRadius
    // sets the width directly at any size; the sum removes the medial-axis
    // creases and keeps both edges' light at a corner. The NORM factors keep
    // their calibration. What does move: a short edge's bloom is dimmer (it
    // emits less), and the interior settles on a floor rather than decaying
    // (capped by insideCutoff) - see
    // docs/corner-crease-and-filament-nyquist.md.
    //
    // Per edge: the perpendicular distance is the per-axis offset, and the
    // segment runs the opposite axis's extent, TRIMMED TO THE TANGENT POINTS.
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
    // quad-sizing formula (setupGeometry), recomputed here, so the pedestals
    // are size-invariant even where the outside cutoff clamps the quad. The
    // `sigma * reachSigmas` term is setupGeometry's filament-reach floor;
    // without it `reach` is 0 at glowRadius 0 and the pedestal subtracts the
    // whole bloom.
    float reach = max(uGlowRadius * GLOW_REACH_RADIUS_FACTOR *
                      (1.0 + uBloomStrength * uIntensity),
                      sigma * reachSigmas);

    // Each piece's own halo and bloom, and V19's correction to the coverage
    // that scales them (addPieceGlowFix). aTop measures to y = -halfSize.y and
    // aBot to +halfSize.y.

    // One arc over the whole ring and no segments: every piece's own coverage
    // equals the gathered mean, so there is nothing to correct. It zeroes the
    // weights below, so every piece takes addStraightGlowFix's /
    // addCornerGlowFix's first return.
    //
    // Do NOT turn this into an `if (!uniformCover)` around the corrections, or
    // around a second copy of the sums. Measured on an M2 Pro, either one makes
    // EVERY scene 10-15% slower at scale 1.0 - fully lit ones included, which
    // never enter it - presumably because this program carries the gather loop
    // there. Below 1.0 the same branch saves ~3%, which is not worth it.
    // (Measured while the loop ran inline at 1.0; a compile-time variant was
    // measured since, at 1.05-1.10x on intensity frames only -
    // docs/neon-shader-cleanup-plan.md.)
    //
    // Decided ONCE, on the CPU (uUniformCover): the very test NeonRenderer
    // skips baking the coverage table on, so wherever the table may be stale
    // this holds and nothing reads it.
    bool uniformCover = uUniformCover != 0;

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
    float pedV   = bloomSegment(reach, tv1, tv2, bw);
    float pedH   = bloomSegment(reach, th1, th2, bw);
    float bLeft  = bloomSegmentPedestalled(aLeft,  tv1, tv2, bw, reach, pedV);
    float bRight = bloomSegmentPedestalled(aRight, tv1, tv2, bw, reach, pedV);
    float bTop   = bloomSegmentPedestalled(aTop,   th1, th2, bw, reach, pedH);
    float bBot   = bloomSegmentPedestalled(aBot,   th1, th2, bw, reach, pedH);

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

        // One shared pedestal for all four arcs. An arc's developed extent is
        // the same lam*HALF_PI wherever the fragment sits at a given distance,
        // so evaluating it CENTRED leaves an expression in uniforms alone -
        // exact for a fragment facing an arc from `reach`, an over-subtraction
        // only off to its side, where the clamp takes it to 0. lam is pinned to
        // the value at `reach` from the arc, sqrt((reach + r) * r). The
        // straights cannot share one: their half-length routinely exceeds
        // `reach`. Worst case, a circle: the tail ends sooner, all within
        // 4/255.
        //
        // Do NOT collapse this further to bw*L/c^2 (atan(x) -> x). That is a
        // ~7% over-subtraction at the largest arc in range, and with the clamp
        // sitting right underneath it, 7% of the pedestal took the same circle
        // to 0.576. (The lam below makes the atan's argument LARGER than the
        // arclength form did, so the linearisation is worse here, not better.)
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

    // The RENORMALISATION: the pedestalled sum scaled back up by peak/(peak -
    // pedestal), so the value on the line is unchanged and BLOOM_NORM_FACTOR
    // keeps its calibration. The gain uses the infinite-line pedestal - within
    // a few percent of the near edge's own on the line. The halo needs no
    // pedestal: at `reach` it is ~2e-4 of peak.
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

    // Compose: base arc x intensity + segments (independent of intensity, so a
    // segment stays lit on a dark arc). EACH SOURCE CARRIES ITS OWN COVERAGE:
    // `col` is a unit hue everywhere, so it is multiplied by emitCover here;
    // one shared gate let a segment's gate lift the arc term where no arc
    // covers (a red segment on a blue half-ring read magenta).
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
    // AXIS. The emission fades to zero over the last stretch so a strong bloom
    // never shows a hard edge where the quad clips it. PER-AXIS (dQuad), not
    // from d: what this hides is a rectangle, and a d-keyed ramp ate the
    // corners early.
    //
    // fadeStart is floored at the outside cutoff's end when that falls inside
    // the quad, so the ramp never dims the band's outer edge before the cutoff
    // mask does. Otherwise the unfloored start, so fadeStart < uQuadMargin
    // always holds and the ramp width is strictly positive (an inverted
    // smoothstep is undefined in GLSL).
    float fadeFloor = uQuadMargin * QUAD_FADE_START_FRAC;
    // Where the band's emission ends: outMid + outHalf, plus cutGuard on the
    // scaled path. NeonRenderer::setupGeometry caps uQuadMargin at the same end
    // (GetCutoffEnd, plus the same guard) plus 1 full-res px, so while that cap
    // holds cutEdge < uQuadMargin and the fade starts at the end.
    float cutEdge   = outMid + outHalf + cutGuard;
    float fadeStart = (cutEdge < uQuadMargin) ? max(fadeFloor, cutEdge) : fadeFloor;
    float dQuad     = sdRoundBox(vPos, halfSize + vec2(uQuadMargin), 0.0);
    result *= 1.0 - smoothstep(-(uQuadMargin - fadeStart), 0.0, dQuad);

    // --- Grade --------------------------------------------------------
    // neonToneMap (neon-grade.glsl, shared with neon-field.frag).
#ifdef NEON_FIELD_BAKE
    // Fa and Fs: the pre-tone-map result per unit of each hue, which
    // neon-field.frag tone-maps times the gathered hues. The masks below are
    // not in it: for the shading's field the blit owns both; the edge ring's
    // field, baked with the ring's uniforms, has its composite apply them
    // (neon-field.frag, NEON_FIELD_RING).
    fragColor = vec4(result.r, result.g, 0.0, 1.0);
    return;
#endif
    result = neonToneMap(result);

    // --- One-sided cut: mask the WHOLE layer at the line --------------
    // Anchored at the opaque fill's own edge and feathered INTO the lit side
    // (sideAA / sideSoft / sideBack at the top of main()).
    //
    // BELOW THE GRADE: the cut is COVERAGE, so it scales what reaches the
    // framebuffer. Above the tone map it was nearly annihilated - a
    // half-covered filament pixel came out at 94% - and the alpha (the peak
    // channel) occluded at 0.94 instead of 0.5. Below it, the cut is the 1 px
    // box filter and tracks the fill it has to register with.
    //
    // Written as `1.0 - smoothstep(lo, hi, d)` for the INSIDE arm rather than
    // smoothstep(hi, lo, d): edge0 > edge1 is UNDEFINED in GLSL. Same curve,
    // portably.
    //
    // THE RING ONLY: for pass 1b neon-blit.frag applies this exact expression
    // against ITS own fwidth, and the ring field's composite (neon-field.frag)
    // applies it for the ring's field. Keep the three in step - one edge.
    if (!blitOwnsCut)
    {
        if (uGlowSide == GLOW_SIDE_INSIDE)       result *= 1.0 - smoothstep(sideBack - sideSoft, sideBack, d);
        else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-sideBack, sideSoft - sideBack, d);
    }

    // --- Hard cutoff masks: close the band at its two boundaries ------
    // In the band means outside the shrunk rect (dIn >= 0) and inside the grown
    // one (dOut <= 0); each ramp spans its full softness, centred on inMid /
    // outMid. Disabled sides arrive as a huge sentinel, so the smoothstep
    // passes 1.0 through. BELOW THE GRADE, as coverage, like the cut: the floor
    // removes the staircase, the placement the brightness bias at half
    // coverage.
    //
    // THE RING ONLY: for pass 1b neon-blit.frag applies these same two ramps at
    // destination resolution, from BLIT_CUTOFF_GUARD_PX of lit texels the
    // discards leave it, and the ring field's composite applies them for the
    // ring's field. Keep the three in step - one edge.
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
