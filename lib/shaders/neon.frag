precision highp float;

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

in vec2 vPos;
out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform float uLineWidth;
uniform float uFilamentFalloff; ///< Generalized-Gaussian exponent (N = value * 2); 1.0 = pure Gaussian, lower = smoother (Laplace-like), higher = flatter top.
uniform float uIntensity;
uniform float uTime;
uniform float uHueRotationRate;
uniform float uGlowRadius;
uniform float uBloomStrength;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;
uniform float uInsideCutoff;          ///< Positive px distance INSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel CPU-side so this branch no-ops.
uniform float uInsideCutoffSoftness;  ///< Feather width in px at the inside cutoff boundary.
uniform float uOutsideCutoff;         ///< Positive px distance OUTSIDE the rect edge past which the emission is culled. Disabled sides collapse to a huge sentinel CPU-side.
uniform float uOutsideCutoffSoftness; ///< Feather width in px at the outside cutoff boundary.
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

// Loop sample positions (perimeter points) as a std140 uniform block. Each
// entry is packed as a vec4 (only .xy is meaningful; std140 pads vec2 to a
// 16-byte stride anyway) so the shader reads raw float32 out of the constant
// cache in the gather loop. Fixed size at compile time - see the shared
// NEON_MAX_LOOP_SAMPLES tuning constant.
//
// Read .xy ONLY in the loops. A per-sample value in .z was tried for the
// lit-extent shares (see uPieceWhole) and cost the fully lit ring 10%:
// loading the whole vec4 doubles the constant traffic of the hottest loop in
// the renderer.
layout(std140) uniform LoopSamplesBlock
{
    vec4 uLoopSamples[NEON_MAX_LOOP_SAMPLES];
};

// The eight emitter pieces - four straights, four corner arcs - and which run
// of loop samples lies on each, packed in NeonRenderer::rebuildLoopSamples:
// .x / .y = the piece's perimeter span as fractions, .z = its first sample
// index, .w = how many samples it owns. Ids are fixed: 0 the x<0 side, 1 x>0,
// 2 y<0, 3 y>0, then the corners (+,+), (+,-), (-,+), (-,-).
//
// This replaces the uNumSamples bound the gather used to walk to. The runs
// tile [0, NeonConfig::numSamples) exactly - position 0 is a piece boundary in
// both windings, so no run wraps - and the emission pre-pass bakes its table
// over the same indices, so texel i there is still sample i here. The two
// must agree or the gather reads emission belonging to a different perimeter
// position.
//
// uPieceLit narrows each piece to the stretch of it the ARCS light - its lit
// extent, the hull of every arc's overlap with the piece - so an arc's glow
// ends where the arc does, wherever along a piece that falls. Without it a
// piece's field ran its whole length and only the averaged coverage said
// where the light stopped: an end ON a piece boundary got the field's own
// analytic cap, an end in the MIDDLE of a piece got the average's slow
// falloff instead - a tail hundreds of px long - so the two ends of one arc
// looked unrelated (V22 in docs/review-findings.md). Packed in
// NeonRenderer::packLightBlockData:
//   uPieceLit   .x / .y = the lit extent in the piece's own terms: a
//                         straight's coordinate along its axis, a corner's
//                         angle from its tangent point on the x = +-halfW
//                         side. The whole piece's when nothing narrows it.
//               .z = the first sample whose cell reaches into the extent,
//               .w = how many do: 0 when no arc lights the piece, -1 for a
//                    lit borrowed piece (see .w of uPieces).
//   uPieceWhole .x / .y = the whole piece in the same terms, for the
//                         segments, which light a piece wherever they sit.
//               .z / .w = the SHARE of the lit run's first / last cell that
//                         lies inside the extent: 1 unless an arc's end falls
//                         in it. On a lit borrowed piece .z is instead the
//                         arc-lit share of the borrowed sample's cell. Each cell's emission weight is its mean over
//                         the WHOLE cell, so the arc average divides by those
//                         two cells' g at their shares only - a uniform arc
//                         then averages to exactly 1 up to its end, and a
//                         sample the end sweeps into arrives with nothing and
//                         grows, with no step as the end crosses a cell.
layout(std140) uniform PieceBlock
{
    vec4 uPieces[NEON_EMITTER_PIECES];
    vec4 uPieceLit[NEON_EMITTER_PIECES];
    vec4 uPieceWhole[NEON_EMITTER_PIECES];
};

// Travelling segments - up to MAX_SEGMENT_BOOSTS independent coloured lights
// on the perimeter. Each vec4 is packed as (position, invSigma, boost,
// hasStops): when .w > 0.5 the segment's colour comes from row `s` of
// uSegmentLUT (its own head-to-tail gradient); when .w == 0 it inherits the
// current base gradient sample. When uSegmentCount == 0 the whole feature is
// skipped in the gather loop.
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

// Per-segment gradient atlas (RGBA8, CLAMP-wrapped both axes). One row per
// segment, laid out head-to-tail across the segment's visible span. Sampled
// only when the segment's hasStops flag is set (see uSegments.w above).
uniform sampler2D uSegmentLUT;

// Arc gating - up to MAX_ARCS independent perimeter slices, each with its own
// start, length, intensity, and optional colour stops. Each vec4 is
// (start, length, intensity, hasStops). Overlap resolves winner-take-all:
// per sample, the arc covering the most of the sample's cell (overlap x
// intensity) contributes its colour, and the coverage is capped at the
// brightest covering arc's intensity (neon-emission.frag). Because the gather
// blends neighbouring samples, adjacent arcs of different colours crossfade
// at the seam in the glow rather than snapping; the filament does the same
// from its own pointwise read, over the arcs' overlapping feathers.
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

// Perimeter emission table from neon-emission.frag: NEON_MAX_LOOP_SAMPLES
// wide, 2 tall, RGBA16F (RGBA8 where the driver refuses float rendering).
//   row 0: .rgb = arcColour * arcW, .a = arcW
//   row 1: .rgb = SUM(segColour * bell), .a = SUM(bell)
// where arcW and bell are each sample's MEAN coverage over its own cell of
// perimeter, with colour-stop alpha folded in - the .a channels are the glow's
// coverage as well as the hue's weights. See neon-emission.frag's header.
// Read with texelFetch at integer sample index - never filtered, since
// neighbouring texels are unrelated perimeter samples. See the gather loop
// and docs/emission-prepass.md.
uniform sampler2D uEmission;

// Distance (in pixels, from the rect edge) to the draw quad's edge. The whole
// emission is faded to zero just before this, so the bloom never shows a hard
// rectangular cutoff where the quad clips it - independent of bloom strength.
uniform float uQuadMargin;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const float PI      = 3.141592653589793;
const float TWO_PI  = 6.283185307179586;
const float HALF_PI = 1.5707963267948966;

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
// The four straights above cover the rect's flat runs. Above cornerRadius 0 the
// emitter also turns through four quarter arcs, and a circular arc has no
// elementary antiderivative under either kernel. This develops each arc onto a
// straight line instead: the tangent at whichever ARC POINT IS NEAREST the
// fragment, carrying the arc's full length PI*r/2 and split about that point.
//
// That choice is what makes it accurate where the naive one is not. The
// perpendicular distance it reports is the true distance to the arc wherever
// the fragment faces it (|length(w) - r|), and the two halves run exactly as
// far as the real arc does in each direction, so the developed arc abuts the
// trimmed straights in arclength and the emitter is continuous - no gap and no
// overlap at the tangent points.
//
// `w` is the fragment's offset from the arc centre in that corner's own frame:
// x along the outward normal of one incident edge, y along the other's, both
// positive pointing away from the rect. So the arc occupies exactly the first
// quadrant of `w`, from +x (one tangent point) to +y (the other), and clamping
// the direction into that quadrant is max(w, 0).
//
// Off the ends the nearest arc point is a tangent point, and which one follows
// from |w - (r,0)|^2 - |w - (0,r)|^2 = 2r*(w.y - w.x): the +x end when
// w.x >= w.y. That is what the fallback picks, so it is the right clamp for the
// whole region it covers and not only for the degenerate point that forces it.
//
// --- THE DEVELOPMENT RATE IS NOT r. ------------------------------------
// Laying the arc out at its own arclength - one unit of tangent per unit of
// arc - is only right for a fragment ON the arc. The exact distance to the
// point at angle dphi from the nearest one is
//
//     D^2 = a^2 + (2*sqrt(rho*r)*sin(dphi/2))^2,   rho = length(w)
//
// so the tangent coordinate the kernels actually want is t = 2*sqrt(rho*r)*
// sin(dphi/2), whose slope at the foot is sqrt(rho*r), not r. Develop at rate
// `lam` instead of r and the emitter comes out short or long, so the measure
// is put back by scaling the whole segment by r/lam - `w` of the returned
// vec4. Rate x weight is r either way, so the arc always carries its full
// PI*r/2 of emitter.
//
// Two things follow, and they are the whole reason for the change:
//
//   - AT THE CENTRE OF CURVATURE IT IS NOW EXACT. rho -> 0 collapses the
//     segment to zero length against an infinite weight, and the limit is
//     f(r) * PI*r/2: every point of the arc at distance r, which is what a
//     fragment at the centre actually sees. At rate r the arc ran off to one
//     side of the foot instead, and since both kernels peak at t = 0 that
//     UNDER-counted - on a circle, where all four arc centres coincide at the
//     middle of the shape, to 54% of the true value.
//   - THE CLAMP STOPS CREASING. Crossing w.y = 0 the arc's endpoints slide at
//     -lam * d(th) on the facing side and at -d(off) on the clamped side; the
//     first is -lam/w.x and the second -1, and they agree only where lam is
//     length(w). At rate r they agreed only at w.x == r, so every other point
//     of the lines through the arc centre carried a C1 crease - a dark cross
//     at the centre of curvature, unmistakable on a circle. Measured as the
//     spurious curvature of (model - numerically integrated truth): 19.9% of
//     the local value at rate r, 0.5% here.
//
// lam is min(rho, sqrt(rho*r)), i.e. sqrt(rho * min(rho, r)). The inner
// branch's rate has to be rho for the clamp to join smoothly; outside the arc
// the linearisation above wants sqrt(rho*r), and the two meet at rho == r,
// where lam is r and the weight is 1 - so a fragment on the arc is bit-
// identical to the rate-r form this replaces, and the calibration the NORM
// factors carry is untouched. Against a numerically integrated perimeter the
// whole emitter's worst error drops as well, on every geometry tested: 16.5 ->
// 9.8% on a 600x400 r=40, 37.3 -> 26.3% on a circle.
//
// Cost is 1.03x of the neon pass at 1280x720, and - as the arc pedestal's note
// below warns - that includes the cornerRadius 0 path, which never executes a
// line of this and still pays 1.026x for the register pressure. Re-time both
// after touching it. An inversesqrt formulation that trades the sqrt and the
// divide for two inversesqrts was measured and came out inside the noise, so
// the readable form stays.
//
// Returns (a, t1, t2, weight) for haloSegment / bloomSegment; the caller
// multiplies the segment by .w.
//
// `phi` narrows the arc to the angles phi.x to phi.y of this frame (0 at the
// tangent point on the x side, HALF_PI at the y side): the development is
// linear in the angle at rate lam, so each end moves by lam times how far
// its angle moved. Written as corrections to the whole quarter's ends rather
// than as lam * (phi - th), so that at (0, HALF_PI) both corrections are
// exactly 0 and the whole quarter is the expression it always was.
vec4 arcTangentSegment(vec2 w, float r, vec2 phi) {
    vec2  wq = max(w, vec2(0.0));
    float ql = length(wq);
    vec2  u  = (ql > ARC_FRAME_EPSILON) ? wq / ql
                                        : ((w.x >= w.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    // Arclength from the +x tangent point to the nearest arc point. u is a unit
    // vector in the first quadrant whenever it came from wq, so th is in
    // [0, HALF_PI] and needs no clamp of its own.
    float th  = atan(u.y, u.x);
    float a   = abs(dot(w, u) - r);
    // Offset of the fragment ALONG the tangent, zero whenever u came from wq
    // (the foot of perpendicular is then the tangent point itself) and non-zero
    // only on the fallback, where it correctly pushes the whole arc to one side.
    // A LENGTH, not an arclength, so it is not scaled by the rate below.
    float off = dot(w, vec2(-u.y, u.x));
    // Floored so the exact centre of curvature cannot divide by zero. The
    // floor is far below one px, and the limit it lands on is the exact value
    // anyway - see above.
    float rho = max(length(w), ARC_FRAME_EPSILON);
    float lam = sqrt(rho * min(rho, r));
    return vec4(a, (-lam * th - off) + lam * phi.x,
                   (lam * (HALF_PI - th) - off) - lam * (HALF_PI - phi.y), r / lam);
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

// NOTE: arcInside() used to live here. It picked the winner-take-all arc per
// sample and weighted that sample in the hue average - a pure function of
// (si, config) - so it moved to neon-emission.frag with the rest of the
// per-sample work, and was later replaced there by each sample's CELL
// coverage. The FILAMENT's extent comes from arcCoverContinuous below, whose
// feather is INWARD and in pixels; the glow's from those per-sample cell
// coverages, meaned per emitter piece.

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

// --- The arc average's denominator on a narrowed piece ----------------------
// SUM g over a lit run counts its two end samples whole, but only uPieceWhole
// .z / .w of those cells lie inside the extent - and each cell's emission
// weight, the numerator's, is its mean over the WHOLE cell. This is the part
// to take back out. Called only when a share is below 1, so a fully lit piece
// never pays for it; a one-cell run carries its share in .z and 1 in .w.
float litEndsOutside(vec4 pl, vec4 pw, float kc2) {
    vec2 d0 = vPos - uLoopSamples[int(pl.z + 0.5)].xy;
    vec2 d1 = vPos - uLoopSamples[int(pl.z + pl.w - 0.5)].xy;
    return (1.0 - pw.z) / (dot(d0, d0) + kc2) + (1.0 - pw.w) / (dot(d1, d1) + kc2);
}
// The same denominator for the segment walk, which covers the WHOLE run and
// so has to sum the lit run again - on a narrowed piece only.
float litRunG(vec4 pl, vec4 pw, float kc2) {
    int   from = int(pl.z + 0.5);
    int   end  = from + int(pl.w + 0.5);
    float sumG = 0.0;
    for (int i = from; i < end; i++) {
        vec2 dv = vPos - uLoopSamples[i].xy;
        sumG += 1.0 / (dot(dv, dv) + kc2);
    }
    return sumG - litEndsOutside(pl, pw, kc2);
}

// --- One emitter piece's halo and bloom at this fragment --------------------
// Returns (halo, bloom, a) of piece k: the two fields UNweighted by coverage,
// pedestal applied, and `a`, the fragment's perpendicular distance to the
// piece - which the caller uses to split the bloom between its two coverage
// averages.
// Piece ids as in PieceBlock: 0 the x<0 side, 1 x>0, 2 y<0, 3 y>0, then the
// corners (+,+), (+,-), (-,+), (-,-).
//
// Straights: the perpendicular distance is the per-axis offset, and the
// segment runs the full extent of the other axis relative to this fragment's
// foot of perpendicular - TRIMMED TO THE TANGENT POINTS, so a straight stops
// where the tube actually turns.
//
// Corners: developed onto their tangent by arcTangentSegment, in each
// corner's own outward frame (sx * x - straight.x, sy * y - straight.y). That
// is the same frame the four arcs used to be folded into through abs(vPos) -
// the near corner's sx * x IS |x| - so each value is bit-identical to the
// folded form; only the order they are summed in changed.
//
// Never called for a corner at cornerRadius 0: arcTangentSegment's weight is
// r / lam, which is 0 / 0 there. The caller skips those, which is also what
// keeps the sharp-cornered case from paying for four atans.
//
// `lim` is the stretch of the piece to take the field of - see uPieceLit.
// For a straight, lim.x to lim.y along its own axis; for a corner, the
// angles lim.x to lim.y of its own frame. The whole piece is (-st, st) or
// (0, HALF_PI), exactly the values these expressions used to have built in,
// so an unnarrowed piece computes what it always computed. A narrowed one
// is the field of the stretch that emits, so an arc's end in the middle of
// a piece gets the same analytic cap an end on a piece boundary always had.
vec3 pieceField(int k, vec2 halfSize, vec2 straight, float kh, float bw, float reach,
                float arcPedestal, vec2 lim) {
    if (k < 4) {
        // Axis the piece's normal runs along, and which side of it.
        bool  vertical = k < 2;
        float side     = (k == 0 || k == 2) ? -1.0 : 1.0;
        float pn = vertical ? vPos.x : vPos.y;
        float pt = vertical ? vPos.y : vPos.x;
        float hn = vertical ? halfSize.x : halfSize.y;
        float a  = abs(pn - side * hn);
        float t1 = lim.x - pt;
        float t2 = lim.y - pt;
        return vec3(haloSegment(a, t1, t2, kh),
                    bloomSegmentPedestalled(a, t1, t2, bw, reach), a);
    }
    float sx = (k < 6) ? 1.0 : -1.0;
    float sy = (k == 4 || k == 6) ? 1.0 : -1.0;
    vec4  c  = arcTangentSegment(vec2(sx * vPos.x, sy * vPos.y) - straight, uCornerRadius, lim);
    // The shared pedestal is for the whole quarter; a part of the arc takes
    // its share of it, which is what a shorter arc's pedestal is out at
    // `reach`. x / x is exactly 1, so the whole quarter is untouched.
    float ped = arcPedestal * ((lim.y - lim.x) / HALF_PI);
    // .w is the measure the development rate cost - see arcTangentSegment -
    // and the pedestal is subtracted from the WEIGHTED value, since that is
    // what has to reach zero at `reach`.
    return vec3(haloSegment(c.x, c.y, c.z, kh) * c.w,
                max(bloomSegment(c.x, c.y, c.z, bw) * c.w - ped, 0.0), c.x);
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
    // sharpen it. (Contrast CUTOFF_SOFT_FLOOR_PX, which IS in buffer px on
    // purpose - that boundary has no full-res counterpart to line up with, so
    // quantisation is its only concern. See neon-tuning.h.)
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
    // softEdge at SIDE_SOFT_EPSILON, which is a hard step, so the glow's edge
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
    if (uGlowSide == GLOW_SIDE_INSIDE  && d >  sideCull) discard;
    if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -sideCull) discard;

    // Hard geometric cutoffs. The band is [-uInsideCutoff, +uOutsideCutoff]
    // with a per-side softness feather straddling each boundary; anything
    // past the feather is culled here so bloom/halo can't leak beyond the
    // artist's stated reach even if uGlowRadius says otherwise. Softness is
    // decoupled from uGlowSideSoftness so the one-sided cut at d=0 can stay
    // pixel-tight while the cutoff joins fade smoothly, and per-side so the
    // interior and exterior can taper at different rates. Disabled sides
    // arrive with size = a huge sentinel, so these branches no-op.
    //
    // THE FLOOR IS AN ANTIALIASING FLOOR, and it applies at every resolution.
    // It used to be SIDE_SOFT_EPSILON at scale 1.0 - a hard step - on the
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
    // The scaled path keeps CUTOFF_SOFT_FLOOR_PX, which is in BUFFER px and so
    // is NOT converted with uResolutionScale like the full-res constants
    // elsewhere: there the concern is not coverage but WHERE the boundary lands
    // once the blit has resampled it, and half a buffer pixel of feather lets
    // the sample nearest the boundary carry a fractional value instead of just
    // 0 or 1. See neon-tuning.h for the measured placement table, for why scale
    // 0.50 specifically is unmoved by it, and for why its value is now 1.0
    // where it was 0.5 - the ramp below stopped doubling, so the constant is
    // stated as the total width it always effectively had.
    float softFloor = (uResolutionScale < 1.0) ? CUTOFF_SOFT_FLOOR_PX : sideAA;
    float inSoft  = max(uInsideCutoffSoftness,  softFloor);
    float outSoft = max(uOutsideCutoffSoftness, softFloor);

    // TOTAL widths, halved here because the ramp is centred on the boundary.
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

    // Band boundaries measured against the offset rect, so a cornerRadius-0
    // band keeps square corners instead of being rounded by the cut distance.
    //
    // A CUTOFF ON THE SIDE glowSide ALREADY CULLS IS NEUTRALISED, by handing it
    // the same huge distance a disabled cutoff arrives with, so every use below
    // no-ops through the arithmetic rather than through a branch.
    //
    // It can never be the binding constraint there. GlowSide::OUTSIDE keeps
    // d >= -sideBack, about half a pixel; an inside cutoff of size S keeps
    // d >= -(S + inHalf), which is looser for every S >= 0. Mirror argument for
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
        dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, uOutsideCutoff);
    }
    if (uGlowSide == GLOW_SIDE_OUTSIDE)
    {
        dIn = CUTOFF_NEUTRALISED;
    }
    else
    {
        dIn = bandInnerDistance(d, uInsideCutoff);
    }
    if (dOut >  outHalf) discard;
    if (dIn  < -inHalf ) discard;

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

    // Perimeter of the rounded rect, in px. Used twice below: to size the
    // colour-gather kernel, and to convert the arc feathers from px to
    // perimeter fractions.
    float r    = clamp(uCornerRadius, 0.0, min(uRectSize.x, uRectSize.y) * 0.5);
    float peri = 2.0 * (uRectSize.x + uRectSize.y - 4.0 * r) + TWO_PI * r;

    // --- Kernel widths ------------------------------------------------
    // Two separate kernels, and the split is the point:
    //
    //  - kc is the COLOUR gather weight, and it is the one length here that is
    //    NOT in pixels. The blend is a 128-sample sum over the gradient LUT,
    //    which is indexed by perimeter FRACTION, so a pixel-sized kernel spans
    //    a different slice of the gradient on every geometry - the same stops
    //    render washed out small and crisp large. Sizing it as a fraction of
    //    the perimeter makes the gradient read identically at any size, and
    //    pins the kernel at a constant 1.13 sample spacings so it cannot bead
    //    at any size either. Not coupled to glowRadius: the gather is colour
    //    only, so a wide glow has no business desaturating the ring. See
    //    neon-tuning.h for the measurements behind this.
    //
    //  - kh / bw are the EMISSION widths: raw glowRadius, no floor. The halo
    //    and bloom are evaluated analytically from the SDF distance further
    //    down, and a closed form cannot bead however far apart the gather
    //    samples are, so glowRadius is proportional across its entire range.
    float kc  = max(peri * COLOR_BLEND_PERIM_FRAC, EMISSION_MIN_WIDTH);
    float kc2 = kc * kc;
    float kh  = max(uGlowRadius,                       EMISSION_MIN_WIDTH);
    float bw  = max(uGlowRadius * BLOOM_REACH_TO_GLOW, EMISSION_MIN_WIDTH);


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
    // FILAMENT. The halo and bloom take the per-piece GATHERED coverage instead
    // - see the glow-coverage block below for why they must. `col` is
    // gated-normalised above, so intensity cancels out of it and can no longer
    // reach the filament that way - emitCover is what carries it. The scaling
    // stays linear in intensity, exactly as it was when it rode on `col`.
    //
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
    // Alpha 0 therefore kills the filament at that position, and the glow
    // there too - the glow's own coverage carries the same alpha, folded into
    // the emission table's weights (neon-emission.frag). The premultiplied
    // output alpha (peak channel, bottom of main) follows for free, so the
    // background shows through rather than being occluded by a black tube.
    //
    // THE FILAMENT'S COLOUR IS READ HERE AS WELL, from the very texels the
    // alpha comes from - the .rgb of a fetch this block was already making.
    // It used to be `col`, the gathered hue, and the gather's weight is
    // Euclidean: on a shape h px thick the opposite edge is a whole line of
    // samples at distance h, worth about kc / sqrt(h^2 + kc^2) of this edge's
    // own. A 1200x40 bar meant to be red along the top rendered its top line
    // (241, 0, 127), a third of the way to the blue of the edge across from
    // it; even 1200x400 read (245, 0, 21). And below 128 samples the gathered
    // hue snapped to the nearest sample, so the ring's gradient stepped in
    // 60 px plateaus at 32. The tube's colour is a pure function of its own
    // perimeter position, so it is read there - docs/review-findings.md V18 and
    // V19. The glow keeps `col`: light from the far edge does reach it.
    vec4  baseTexPt   = texture(uGradientLUT,
                                vec2(sPos - uTime * uHueRotationRate, 0.5));
    float baseAlphaPt = baseTexPt.a;
    // Winner-take-all across arcs, as documented for overlap. This can be a
    // plain max() again because arcCoverContinuous now reaches a FULL 1.0 at an
    // abutting endpoint rather than 0 (inward) or 0.5 (straddling), so two arcs
    // tiling the ring hand over at max(w1, w2) with no notch - and because
    // their ramps overlap, the handover stays smooth even when w1 != w2.
    //
    // The COLOUR hands over by a weighted mean instead, with weights
    // (coverage x intensity)^4. A max() would switch hue in one pixel at every
    // seam between arcs of different colours; the fourth power keeps the
    // winner-take-all rule wherever the arcs differ - a 0.3-intensity arc
    // under a 1.0 one contributes 0.8% of the hue - while crossing a seam,
    // where both run up to full coverage over the same overlapping ramps,
    // continuously. Alpha stays out of the hue weight: a transparent arc still
    // defines the colour a stop-less segment over it inherits.
    float emitCover = 0.0;
    float arcBare   = 0.0; // largest coverage with no intensity or alpha in it
    vec3  filHueAcc = vec3(0.0);
    float filHueW   = 0.0;
    for (int a = 0; a < uArcCount; a++) {
        vec4 arc = uArcs[a];
        if (arc.z <= 0.0) continue;                       // dark arc: no filament
        float c = arcCoverContinuous(sPos, arc.x, arc.y, headF, tailF,
                                     arcTailAbuts(arc.w), arcHeadAbuts(arc.w));
        if (c <= 0.0) continue;                           // does not reach here
        // Each arc's own colour and alpha, from the same LUT the gather's came
        // from and in the same coordinate space - arc-local for hasStops,
        // perimeter space otherwise.
        vec4 a4;
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
            a4         = texture(uArcLUT, vec2(uArc, rowY));
        } else {
            a4 = baseTexPt;
        }
        emitCover = max(emitCover, c * arc.z * a4.a);
        float hw  = c * arc.z;
        hw       *= hw;
        hw       *= hw;
        filHueAcc += a4.rgb * hw;
        filHueW   += hw;
        arcBare    = max(arcBare, c);
    }
    // Where no arc reaches, emitCover is 0 and this hue multiplies nothing;
    // the base colour is only there for the segment fallback below.
    vec3 filHue = (filHueW > 0.0) ? filHueAcc / filHueW : baseTexPt.rgb;

    // Segment coverage at this fragment's own perimeter position. This is the
    // segments' whole FILAMENT magnitude: boost * bell, straight off the
    // analytic gaussian, so it cannot inherit either the gather's sample
    // stepping or the far-side dilution that used to make a segment dimmer on
    // a small rect. Segments emit where no arc covers, so they carry their own
    // filament/halo/bloom.
    //
    // Coloured pointwise too, for the filament's reason above. A stop-less
    // segment inherits the arc's colour where an arc covers and the base
    // gradient where none does - the same rule the pre-pass's segFallback
    // applies at the samples - blended across an arc's free end by the arc's
    // own coverage rather than switched at it.
    vec3  segFallbackPt = mix(baseTexPt.rgb, filHue, clamp(arcBare, 0.0, 1.0));
    float segCoverPt    = 0.0;
    // Colour x magnitude, summed: the segments' filament emission. Unclamped on
    // purpose - boost above 1 must still brighten, as it did when the gather's
    // `bell` carried the magnitude. (The glow's min(.., 1.0) below only bounds
    // the shared halo/bloom reach - it is not the segment's brightness.)
    vec3  segCol        = vec3(0.0);
    for (int s = 0; s < uSegmentCount; s++) {
        vec4  seg = uSegments[s];
        float rel = sPos - seg.x;
        rel      -= floor(rel + 0.5);                     // wrap to [-0.5, 0.5]
        float e   = rel * seg.y;
        // Per-segment colour and alpha, pointwise - see emitCover above.
        // Stop-less segments inherit the base gradient's alpha, mirroring how
        // their colour falls back in the gather.
        vec4 s4;
        if (seg.w > 0.5) {
            float tLocal = clamp(0.5 + e * 0.5, 0.0, 1.0);
            float rowY   = (float(s) + 0.5) / float(MAX_SEGMENT_BOOSTS);
            s4           = texture(uSegmentLUT, vec2(tLocal, rowY));
        } else {
            s4 = vec4(segFallbackPt, baseAlphaPt);
        }
        float sb    = seg.z * exp(-e * e) * s4.a;
        segCoverPt += sb;
        segCol     += s4.rgb * sb;
    }

    // --- GLOW coverage: per emitter piece, GATHERED ------------------------
    // The halo and bloom are integrals over the emitter, evaluated below as a
    // sum over its eight pieces. So what scales them has to be each piece's
    // coverage averaged over that piece - one number per piece - and neither
    // of the two simpler things this has been:
    //
    //   - THE COVERAGE AT THIS FRAGMENT'S NEAREST PERIMETER POINT. sPos is a
    //     nearest-point map, so it JUMPS across the medial axis, and a smooth
    //     field times a jumping scalar is a jumping field: hard 45-degree
    //     creases in from every corner and a flat cut along the half-min
    //     extent. Measured on a 1920x1080 rect with one boosted segment on the
    //     top edge: 41 at the half-height line, 8 (background) one pixel below
    //     it. That was V14.
    //
    //   - ONE COVERAGE FOR THE WHOLE RING, which is how V14 was fixed. It
    //     scaled every piece's field by the same ring-wide mean, so an unlit
    //     piece kept its own field - peaked on its own line - at however lit
    //     the rest of the ring looked from here: every dark edge glowed in its
    //     own outline, +6 levels on a 600x400 rect, +20 on 1200x400, +44 on a
    //     1200x40 bar. The same mean diluted a lit edge by the dark one across
    //     from it, which that ridge partly hid. And it carried no colour-stop
    //     alpha, so an alpha-0 stretch kept its whole glow. V16 and V15.
    //
    // Each piece's mean comes out of the colour gather below for one add per
    // sample: the pieces' runs are walked separately, so SUM(cover * g) /
    // SUM(g) over a piece's own samples is that piece's g-weighted mean
    // coverage. Smooth in the fragment by construction - every sample of the
    // piece contributes at every fragment - and exact for what matters most:
    // an unlit piece's samples all carry 0, and so does its weight. Against a
    // numerically integrated reference - SUM over pieces of INTEGRAL cover * K
    // with the shader's own kernels - a quarter-edge arc's glow is within 5.3%
    // on average where the ring-wide mean was 10.1% (worst 17% against 122%,
    // the latter on the unlit edge line).
    //
    // SEGMENTS take a second, tighter average as well, and are deliberately
    // NOT held to that reference: their glow carries a reach bound, and where
    // it is applied - after averaging, per piece near the line and ring-wide
    // far from it - is what gives a segment's glow the feathered falloff V14
    // drew. See the walk below and GLOW_CORE_TO_HALO in neon-tuning.h.
    //
    // A FULLY LIT RING IS UNCHANGED: every sample carries weight 1, every
    // piece's mean is exactly 1.0, and the sum below is the unweighted sum.
    //
    // The emission table's weights are cell means (see neon-emission.frag), so
    // these are also continuous as a light MOVES - which the point-sampled
    // table they replaced was not: a rotating arc's glow ticked by ~20 levels
    // per sample it crossed, a 0.005-long segment's by 80. V17.

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
    // Each piece's own geometry - the per-axis offset and extent of a
    // straight, the developed tangent of a corner - is in pieceField, which
    // the walk below calls once per piece.
    vec2 straight = max(halfSize - vec2(uCornerRadius), vec2(0.0));

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

    // The four corner arcs' shared bloom pedestal. Hoisted out of the piece
    // loop below because it is a function of uniforms alone - which is the
    // whole argument for it, and the reason it can be shared at all:
    //
    // One shared pedestal for all four arcs, and unlike the straights'
    // it does not have to be per-piece. A pedestal is that piece's own
    // bloom evaluated at `reach`, and an arc's developed extent is the same
    // lam*HALF_PI wherever the fragment sits at a given distance - only its
    // offset along the tangent varies with position AROUND the arc. So
    // evaluating that extent CENTRED (t = -L/2 .. +L/2) leaves an
    // expression in uniforms alone: exact for every fragment that faces an
    // arc from `reach`, and an over-subtraction only for ones off to its
    // side, where the arc term is small and the clamp in pieceField takes it
    // to zero anyway.
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
    float arcPedestal = 0.0;
    if (uCornerRadius > 0.0)
    {
        float arcC      = sqrt(reach * reach + bw * bw);
        float arcLamPed = sqrt((reach + uCornerRadius) * uCornerRadius);
        arcPedestal     = uCornerRadius / arcLamPed *
                          bw / arcC * 2.0 * atan(arcLamPed * HALF_PI / (2.0 * arcC));
    }

    // --- Colour gather and per-piece glow, in one walk ----------------------
    // One pass over the emitter's eight pieces does three things for each:
    // gathers its run of loop samples into the ring-wide COLOUR sums, takes
    // the piece's own COVERAGE as the mean over those same samples, and adds
    // the piece's analytic FIELD to the halo and bloom weighted by that
    // coverage (see the glow-coverage block above for why per piece).
    //
    // All three in one walk because of what the alternative cost. Gathering
    // the eight coverages first and weighting the fields afterwards kept
    // sixteen floats live across everything in between, and measured 1.9x the
    // whole neon layer with eight separate loops or 1.25x with one nested
    // loop and a one-hot flush. Folded into the walk, a piece's coverage dies
    // the moment its field is weighted and what stays live is two running
    // pairs: 1.21x. The rest of the way - to FASTER than the single loop this
    // replaced - was the unroll described below.
    //
    // The sums are PAIRS: .x the arc term, .y the segment term, which carry
    // different hues and are split only at the compose. The pairs ARE the
    // gathered means - the segment's reach bound is already in its weight,
    // applied per sample by the pre-pass.
    //
    // Per sample the body is the ring-wide gather this replaced: 1 UBO read
    // for the sample position, 1 sub, 1 dot, 1 reciprocal and 2 texelFetches -
    // 1 where the config has no segments - grouped four or two samples to a
    // step (see the unroll note below). What used to live
    // here - the arc winner-take-all scan over uArcCount, the segment loop over
    // uSegmentCount, and one to two FILTERED LUT fetches - was a pure function
    // of (si, uTime, config), so it moved to neon-emission.frag; see
    // docs/emission-prepass.md for the packing and the invariant that keeps
    // that split honest.
    //
    // GATED normalisation of the colour, and it is the point. Dividing by the
    // same weight the numerator was gathered with makes `col` a pure hue of
    // unit magnitude: it carries no coverage, no per-arc intensity and no
    // colour-stop alpha, all of which cancel. Those reach the emission through
    // the coverages instead - pointwise for the filament, per piece for the
    // glow. segAcc / wsumSegW does the identical thing for the segment hue.
    // Both used to divide by an UNGATED sum over every sample, so an unlit far
    // side of the ring dragged the lit colour toward black by roughly
    // kc / rectHeight - 0.79 of full brightness at 200x150 against 0.97 at
    // 1920x1080 for a quarter-perimeter arc. Gated normalisation is exactly
    // 1.0 at every size. Segments are gathered with the raw proximity weight
    // g, NOT the arc-gated one, so a segment lights even on perimeter
    // stretches no arc covers.
    //
    // UNROLLED BY HAND - four samples a step with no segments, two with - and
    // that is a measured choice, not tidiness. The single ring-wide loop this
    // replaced was left to the compiler; split into eight short runs it cost
    // the layer 0.7 ms more at 1920x1080, because each run is too short for
    // the compiler to pipeline. Unrolled, the segment-less body is FASTER than
    // the single loop it replaced (default ring 4.43 against 4.71 ms, sharp-
    // cornered 3.67 against 4.28, medians of eight interleaved runs). The
    // segment body carries the segments' second, tight average - two more
    // sums and five multiplies a sample - and runs 6.89 against 6.54. Four samples a
    // step in the segment body was 0.25 ms faster there and 0.2 to 0.35 ms
    // slower on every segment-less config - both bodies share one register
    // allocation - so it stays at two. The unroll reassociates the sums,
    // which moves at most 5 pixels of 800,000 by one level. Re-time a
    // segment-less AND a segmented scene after touching either body.
    //
    // TWO LOOP BODIES, ONE UNIFORM BRANCH. Row 1 of the emission table is
    // exactly vec4(0.0) when uSegmentCount is 0, so the segment-less body drops
    // its fetch and the two adds it feeds. Branching on that per SAMPLE was
    // measured SLOWER than the fetch it skipped (4.52 against 4.23 ms at
    // 1920x1080); per PIECE it is eight uniform branches a fragment. Safe for
    // the same reason the lens flare's uSpread guard is: the condition is
    // uniform across the draw. Nothing here takes a derivative - texelFetch
    // has no LOD to compute.
    //
    // A piece that owns no sample but has length - a corner shorter than one
    // spacing - arrives with a count of -1 and the index of the sample nearest
    // its middle, and takes that sample's coverage without adding its colour a
    // second time. A piece of no length has a count of 0 and contributes
    // nothing. Corners are skipped outright at cornerRadius 0: the straights
    // then run corner to corner and there is no arc to model.
    vec3  acc      = vec3(0.0); // base colour x arc-gated gather weight
    vec3  segAcc   = vec3(0.0); // segment colour x bell x gather weight
    float wsumLit  = 0.0;       // SUM ARC-GATED g     - normalises `col`
    float wsumSegW = 0.0;       // SUM SEGMENT bell*g  - normalises the segment hue
    vec2  halo     = vec2(0.0); // (arc, segment) halo
    vec2  bloom    = vec2(0.0); // (arc, segment) bloom
    float bloomFar = 0.0;       // a segment's bloom, its far share - see GLOW_CORE_TO_HALO
    float wsumAll  = 0.0;       // SUM g over the whole ring - that far share's reach bound
    // Where a segment's bloom crosses from its tight coverage to its broad
    // one - see GLOW_CORE_TO_HALO in neon-tuning.h.
    float coreR    = GLOW_CORE_TO_HALO * kh;
    float coreR2   = coreR * coreR;
    for (int k = 0; k < NEON_EMITTER_PIECES; k++) {
        if (k >= 4 && uCornerRadius <= 0.0) break;   // uniform: no arcs to model
        vec4 pc    = uPieces[k];
        vec4 pl    = uPieceLit[k];
        // A dark piece - no arc reaches it - adds nothing to the colour (its
        // samples' weights are 0) and has no arc field to scale, so without
        // segments it is skipped outright. That is what a partly lit ring
        // saves over a fully lit one.
        if (pl.w == 0.0 && uSegmentCount == 0) continue;
        int  first = int(pc.z + 0.5);
        vec2  cov;      // this piece's (arc, segment) coverage, colour-weighted
        float segTight; // its segment coverage again, weighted with 1/d^4 tails
        if (pc.w < -0.5) {
            // Borrowed: another piece's sample, so its coverage only, and
            // one sample has no tails to weigh. That sample's weight is a mean
            // over ITS cell, which an arc's end may light only partly; the
            // lit share of that cell (uPieceWhole .z here) takes it back to
            // what the lit part carries, so a lit corner reads 1, not the
            // share of a neighbour's cell that happened to be lit.
            float borrowed = texelFetch(uEmission, ivec2(first, 0), 0).a / uPieceWhole[k].z;
            cov      = vec2(pl.w < -0.5 ? borrowed : 0.0,
                            texelFetch(uEmission, ivec2(first, 1), 0).a);
            segTight = cov.y;
        } else if (uSegmentCount > 0) {
            // The whole run, since a segment can sit anywhere on the piece.
            int   end = first + int(pc.w + 0.5);
            float litC = 0.0, segC = 0.0, allG = 0.0, segT = 0.0, allT = 0.0;
            int   i   = first;
            for (; i + 2 <= end; i += 2) {
                vec2 d0 = vPos - uLoopSamples[i].xy;
                vec2 d1 = vPos - uLoopSamples[i + 1].xy;
                vec2 g2 = 1.0 / (vec2(dot(d0, d0), dot(d1, d1)) + kc2);
                // The TIGHT weight, from g with multiplies only: x = kc^2 * g is
                // 1 on the line and kc^2 / d^2 far away; two passes of
                // x * (2 - x) take that to 1 and 4x, and squaring leaves a
                // weight flat near the line and falling as (2 kc / d)^4 - the
                // tail of (kt^2 / (d^2 + kt^2))^2 at kt = 2 kc, without its
                // reciprocal.
                vec2 t2 = g2 * kc2;
                t2 *= 2.0 - t2;
                t2 *= 2.0 - t2;
                t2 *= t2;
                vec4 a0 = texelFetch(uEmission, ivec2(i, 0), 0);
                vec4 a1 = texelFetch(uEmission, ivec2(i + 1, 0), 0);
                vec4 b0 = texelFetch(uEmission, ivec2(i, 1), 0);
                vec4 b1 = texelFetch(uEmission, ivec2(i + 1, 1), 0);
                vec2 aa = vec2(a0.a, a1.a);
                vec2 ba = vec2(b0.a, b1.a);
                acc    += a0.rgb * g2.x + a1.rgb * g2.y;
                segAcc += b0.rgb * g2.x + b1.rgb * g2.y;
                litC   += dot(aa, g2);
                segC   += dot(ba, g2);
                allG   += g2.x + g2.y;
                segT   += dot(ba, t2);
                allT   += t2.x + t2.y;
            }
            for (; i < end; i++) {
                vec2  dv = vPos - uLoopSamples[i].xy;
                float g  = 1.0 / (dot(dv, dv) + kc2);
                float t  = g * kc2;
                t *= 2.0 - t;
                t *= 2.0 - t;
                t *= t;
                vec4  e0 = texelFetch(uEmission, ivec2(i, 0), 0);
                vec4  e1 = texelFetch(uEmission, ivec2(i, 1), 0);
                acc    += e0.rgb * g;
                segAcc += e1.rgb * g;
                litC   += e0.a * g;
                segC   += e1.a * g;
                allG   += g;
                segT   += e1.a * t;
                allT   += t;
            }
            wsumLit  += litC;
            wsumSegW += segC;
            wsumAll  += allG;
            // The arc average's denominator is the LIT run's, which only a
            // narrowed piece has to walk again - and only its own stretch.
            float litG = allG;
            vec4  pw   = uPieceWhole[k];
            if (pl.w > 0.5 && (pl.w != pc.w || pw.z < 1.0 || pw.w < 1.0)) {
                litG = litRunG(pl, pw, kc2);
            }
            cov      = vec2(litC / max(litG, WSUM_EPSILON), segC / max(allG, WSUM_EPSILON));
            segTight = segT / max(allT, WSUM_EPSILON);
        } else {
            // The lit run alone: past the extent every weight is 0, so the
            // sums lose nothing by not walking there.
            int   from = int(pl.z + 0.5);
            int   end  = from + int(pl.w + 0.5);
            float litC = 0.0, allG = 0.0;
            int   i    = from;
            for (; i + 4 <= end; i += 4) {
                vec2 d0 = vPos - uLoopSamples[i].xy;
                vec2 d1 = vPos - uLoopSamples[i + 1].xy;
                vec2 d2 = vPos - uLoopSamples[i + 2].xy;
                vec2 d3 = vPos - uLoopSamples[i + 3].xy;
                vec4 g4 = 1.0 / (vec4(dot(d0, d0), dot(d1, d1), dot(d2, d2), dot(d3, d3)) + kc2);
                vec4 a0 = texelFetch(uEmission, ivec2(i, 0), 0);
                vec4 a1 = texelFetch(uEmission, ivec2(i + 1, 0), 0);
                vec4 a2 = texelFetch(uEmission, ivec2(i + 2, 0), 0);
                vec4 a3 = texelFetch(uEmission, ivec2(i + 3, 0), 0);
                acc  += a0.rgb * g4.x + a1.rgb * g4.y + a2.rgb * g4.z + a3.rgb * g4.w;
                litC += dot(vec4(a0.a, a1.a, a2.a, a3.a), g4);
                allG += dot(g4, vec4(1.0));
            }
            for (; i < end; i++) {
                vec2  dv = vPos - uLoopSamples[i].xy;
                float g  = 1.0 / (dot(dv, dv) + kc2);
                vec4  e0 = texelFetch(uEmission, ivec2(i, 0), 0);
                acc  += e0.rgb * g;
                litC += e0.a * g;
                allG += g;
            }
            wsumLit += litC;
            vec4 pw = uPieceWhole[k];
            if (pw.z < 1.0 || pw.w < 1.0) {
                allG -= litEndsOutside(pl, pw, kc2);
            }
            cov      = vec2(litC / max(allG, WSUM_EPSILON), 0.0);
            segTight = 0.0;
        }
        // Arcs: the field of the LIT EXTENT, scaled by the coverage averaged
        // over that extent alone. A uniform arc reads exactly 1 there, so its
        // glow is the analytic field of the stretch it lights, and both of
        // its ends get the same cap whether or not they fall on a piece
        // boundary. Unnarrowed, uPieceLit's limits ARE the whole piece's.
        vec3 f = pieceField(k, halfSize, straight, kh, bw, reach, arcPedestal, pl.xy);
        halo.x  += cov.x * f.x;
        bloom.x += cov.x * f.y;

        // Segments: the look V14 had - a line that ends crisply past a
        // segment's tail, inside a broad dome that goes dark at its edges -
        // without V14's outline. See GLOW_CORE_TO_HALO in neon-tuning.h.
        // Uniform branch, and every term of it is per piece, not per sample.
        if (uSegmentCount > 0) {
            // A segment can sit anywhere on the piece, lit or not, so it takes
            // the WHOLE piece's field - a second evaluation only where the
            // arcs narrowed it.
            vec2 whole = uPieceWhole[k].xy;
            if (pl.x != whole.x || pl.y != whole.y) {
                f = pieceField(k, halfSize, straight, kh, bw, reach, arcPedestal, whole);
            }
            // The reach bound, max(arc, min(segment, 1)), on the TIGHT
            // average: a faint stretch glows as its square, so past the tail -
            // where the tight average falls fastest - the halo and the near
            // bloom feather off with the line instead of trailing along it.
            float segNear  = segTight * max(cov.x, min(segTight, 1.0));
            // How much of this piece's bloom is "far": 0 on its line, 0.5 at
            // coreR, -> 1 beyond.
            float farShare = f.z * f.z / (f.z * f.z + coreR2);
            halo.y   += segNear * f.x;
            bloom.y  += segNear * f.y * (1.0 - farShare);
            bloomFar += cov.y   * f.y * farShare;
        }
    }
    // The far share's reach bound is read from the WHOLE ring, as V14 read it
    // for the whole glow. That is what makes the dome around a segment fade
    // with distance: away from the segment the ring-wide average counts the
    // dark pieces too, so it falls and the bound falls faster. It is one
    // scalar at this fragment for every piece, so unlike V14's use of the same
    // number it cannot light an unlit piece - the far share is already the
    // piece's own average, and an unlit piece's is 0. Under a lit arc it is
    // max(arc, ...) = 1 and changes nothing.
    if (uSegmentCount > 0) {
        float ringDen = max(wsumAll, WSUM_EPSILON);
        bloom.y += bloomFar * max(wsumLit / ringDen, min(wsumSegW / ringDen, 1.0));
    }

    // Both are pure hues of unit magnitude; the magnitudes are the coverages.
    vec3 col       = acc    / max(wsumLit,  WSUM_EPSILON); // base perimeter hue
    vec3 segColHue = segAcc / max(wsumSegW, WSUM_EPSILON); // segment hue

    halo  *= HALO_NORM_FACTOR;
    bloom *= BLOOM_NORM_FACTOR;

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
    float bloomPeak = BLOOM_NORM_FACTOR * PI;
    float bloomPed  = BLOOM_NORM_FACTOR * PI * bw / sqrt(reach * reach + bw * bw);
    bloom = bloom * (bloomPeak / max(bloomPeak - bloomPed, 1e-6));

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
    // EACH SOURCE CARRIES ITS OWN COVERAGE. `col` and `filHue` are pure hues of
    // unit magnitude everywhere on the quad - neither decays with distance from
    // the lit arc, because the coverage that used to ride in the hue moved out
    // of it. So each must be multiplied by its coverage here. Summing the two
    // sources first and applying one shared gate (the old
    // `lightCol * filamentGate`) let the segment's gate lift the arc term on a
    // stretch NO arc covers: a blue arc over half the ring plus a red segment
    // on the other half rendered the segment magenta at ~2x brightness, and
    // violet - arc-dominant - on its shoulders.
    //
    // THE FILAMENT AND THE GLOW TAKE DIFFERENT COVERAGES, and that is the
    // point. The filament is an SDF-derived line: it lives ON the perimeter, so
    // it wants the POINTWISE pair - and the pointwise hue - exact at this
    // fragment's own perimeter position. The halo and bloom are integrals over
    // the emitter's pieces, so each piece's field already carries that piece's
    // GATHERED coverage (the .x / .y of the pairs above), and all that is left
    // here is the hue each half is lit in.
    //
    // filamentGate is the segment's SHARP gate (smoothstep 0.5..1) maxed with
    // emitCover, applied to the segment term only - the arc takes its own
    // coverage directly, since for an arc the gate was just that coverage.
    vec3 emitFil = filHue * (uIntensity * emitCover) + segCol * filamentGate;
    vec3 arcCol  = col * uIntensity;

    vec3 result  = emitFil * core * FILAMENT_GAIN * lineGate;
    result      += (arcCol * halo.x  + segColHue * halo.y)  * (HALO_GAIN      * glowGate);
    result      += (arcCol * bloom.x + segColHue * bloom.y) * (uBloomStrength * glowGate);

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
    // An upper bound on where the band's emission ends, not the exact point:
    // the cutoff ramp is centred on the boundary and so reaches outSoft/2 past
    // it, where this budgets outSoft. Erring outward is the safe direction -
    // it starts the quad fade LATER, which is what this floor exists to do.
    float cutEdge   = uOutsideCutoff + outSoft;
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
    // Ramps span inSoft / outSoft TOTAL, centred on the boundary - hence the
    // halves, and see where they are derived for what the doubled form cost.
    result *= smoothstep(-inHalf, inHalf, dIn);
    result *= 1.0 - smoothstep(-outHalf, outHalf, dOut);

    // Premultiplied-alpha output so the effect composites over arbitrary
    // background objects instead of only adding light. Coverage = brightest
    // channel: the hot filament core (alpha ~ 1) occludes the background and
    // reads as a solid tube; the dim halo/bloom (alpha ~ 0) stay additive; the
    // dark surround (alpha = 0) leaves the background untouched. Pairs with
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) in the renderer.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
