precision highp float;

// ---------------------------------------------------------------------------
// Glow coverage pre-pass (V20, V21).
//
// Bakes, for every PIECE of the emitter - the four straights and the four
// corner arcs neon.frag sums the halo and bloom over - how lit that piece is
// as each layer sees it from a fragment: the piece's own coverage over its own
// extent, weighted by the layer's kernel at the fragment's distance,
//
//     INTEGRAL_piece cover(q) K(q - x) dq  /  INTEGRAL_piece K(q - x) dq,
//
// with q the arclength along the piece and x the fragment's foot on it.
// neon.frag scales each piece's halo and bloom - itself that piece's integral
// of K - by one filtered fetch of it, so the product is the piece's integral
// of cover * K. The table's layout, and where a fragment reads it, is in
// neon-pieces.glsl, injected ahead of this file.
//
// Why a table
// -----------
// Each piece's halo and bloom has to be scaled by the coverage of THAT piece,
// near ITS foot, under ITS kernel - not by one coverage gathered around the
// fragment, which is dominated by whichever piece is nearest. Scaled by the
// fragment's, a dark stretch borrowed light from lit ones far along the line
// (V19, a thin bright line along it) and the lit far edges were dimmed by the
// dark near one (V20, a groove along it). The closed form evaluated in
// neon.frag for eight pieces per fragment cost 2-3x on a partly lit ring, and
// none of it depends on the fragment except through two numbers per piece, so
// it is baked here once per config change and read with one fetch per piece.
//
// Why one per piece (V21)
// -----------------------
// V20 baked one table per PERIMETER, indexed by perimeter position and
// distance, which convolved the coverage along a straight line through the
// foot that ran on past the piece's ends as if the outline did not turn. So
// near a corner where a lit stretch met a dark one, a lit piece read a little
// dim and a dark one a little lit: 8-15 levels over most of a partly lit
// frame, 33 in one segment scene. Here each piece's integral stops at its own
// ends, and a corner's runs along the line arcTangentSegment develops it onto
// - the line its halo and bloom are integrated along.
//
// The kernels
// -----------
// Along a line at distance a the halo's kernel is c^2 / (t^2 + c^2)^1.5 and
// the bloom's c / (t^2 + c^2), c = sqrt(a^2 + k^2) - kh for the halo, bw for
// the bloom (haloSegment / bloomSegment in neon.frag are the same integrands
// up to a constant, which the ratio above cancels). In the angle
// theta = atan(t / c) both are elementary - the bloom's mass is d(theta), the
// halo's cos(theta) d(theta) - which is how every integral below is written:
// it keeps a difference of two nearly equal masses from cancelling when the
// fragment is far off to one side.
//
// A corner is developed at a rate other than 1 (see arcTangentSegment), so its
// arclength q sits at t1 + q / weight on that line. Along the arc, then, the
// kernel is the same kernel with width c * weight, centred on the foot
// q = -t1 * weight, and the corner's integral is a straight one in its own
// arclength.
//
// The coverage
// ------------
// An arc's coverage is a plateau of its intensity between two linear ramps -
// its feathers - so against either kernel it is closed form: each linear piece
// of it needs the kernel's mass and first moment over its span. A segment is a
// Gaussian bell, which has no closed form against either kernel, so it is
// integrated - see bellMass for the two quadratures and the accuracy they were
// picked for.
//
// Units and output
// ----------------
// Every length here is a FRACTION OF THE PERIMETER, so one table serves both
// resolution paths: neon.frag's pixel lengths all scale with
// uResolutionScale, the perimeter included, and their ratios do not move.
//
// Output: (.r halo, .g bloom) arc coverage x intensity and (.b halo, .a bloom)
// segment boost x bell, each encoded c / (1 + c) like the gather buffer -
// intensity and boost are unbounded, and the RGBA8 fallback would otherwise
// clamp them at 1.
// ---------------------------------------------------------------------------

out vec4 fragColor;

// Same layout and binding as neon.frag's - see NeonRenderer::packLightBlocks.
layout(std140) uniform ArcBlock
{
    int  uArcCount;
    vec4 uArcs[MAX_ARCS];
};

// Same layout and binding as neon-common.glsl's.
layout(std140) uniform SegmentBlock
{
    int  uSegmentCount;
    vec4 uSegments[MAX_SEGMENT_BOOSTS];
};

uniform float uHeadFeather;    ///< HEAD_FEATHER_PX as a fraction of the perimeter.
uniform float uTailFeather;    ///< TAIL_FEATHER_PX as a fraction of the perimeter.
uniform float uHaloWidth;      ///< kh as a fraction of the perimeter; also the layout's length scale.
uniform float uBloomWidth;     ///< bw as a fraction of the perimeter.
uniform vec2  uStraightSize;   ///< The horizontal and vertical straights' lengths, fractions of the perimeter.
uniform float uRadius;         ///< The corner radius as a fraction of the perimeter.
uniform int   uWinding;        ///< 0 = CLOCKWISE, 1 = COUNTER_CLOCKWISE, as neon.frag's.
uniform vec2  uGlowCoverSplit; ///< Each band's straight interior columns, as neon.frag's (glowCoverInner).

// See neon.frag's copies: the abutment bits pick each endpoint's feather
// direction. Values are 0..7, all exact in a float.
bool coverTailAbuts(float flags) { return mod(floor(flags * 0.5), 2.0) >= 0.5; }
bool coverHeadAbuts(float flags) { return mod(floor(flags * 0.25), 2.0) >= 0.5; }

// --- Where each piece is on the perimeter ------------------------------
// (.x) the perimeter position of a piece's t1 end - the end haloSegment's t1
// and arcTangentSegment's +x end measure from - and (.y) +1 if perimeter
// position grows toward its t2 end, -1 if it shrinks. `cwStart` / `cwSigma`
// are the CLOCKWISE values; neon.frag's perimeterPosition lays
// COUNTER_CLOCKWISE out over the same geometry the other way round from the
// top-left tangent point, which is s -> 1 - arcLen - s. Checked against
// perimeterPosition for every piece, both windings, at four shapes.
vec2 pieceStart(float cwStart, float cwSigma, float arcLen) {
    if (uWinding == 0) {
        return vec2(cwStart, cwSigma);
    }
    float s = 1.0 - arcLen - cwStart;
    return vec2(s - floor(s), -cwSigma);
}

// The straight in band `band` (GLOW_COVER_BAND_*), and the corner arc in
// quadrant `signs` (+y the top edge, as perimeterPosition has it), whose t1 end
// is the tangent point on the vertical edge.
vec2 straightStart(int band, float ws, float hs, float arcLen) {
    if (band == GLOW_COVER_BAND_NEG_X) {
        return pieceStart(2.0 * ws + 3.0 * arcLen + hs, 1.0, arcLen);
    }
    if (band == GLOW_COVER_BAND_POS_X) {
        return pieceStart(ws + arcLen + hs, -1.0, arcLen);
    }
    if (band == GLOW_COVER_BAND_NEG_Y) {
        return pieceStart(2.0 * ws + 2.0 * arcLen + hs, -1.0, arcLen);
    }
    return pieceStart(0.0, 1.0, arcLen);
}
vec2 cornerStart(vec2 signs, float ws, float hs, float arcLen) {
    float s = (signs.x > 0.0) ? ws + arcLen + ((signs.y > 0.0) ? 0.0 : hs)
                              : 2.0 * ws + 3.0 * arcLen + hs + ((signs.y > 0.0) ? hs : 0.0);
    return pieceStart(s, -signs.x * signs.y, arcLen);
}

// --- The kernels -------------------------------------------------------
// ln((t1^2 + c^2) / (t0^2 + c^2)), to full precision where the ratio is close
// to 1 - there it is 2 atanh(z), z = (t1 - t0)(t1 + t0) / (t1^2 + t0^2 +
// 2 c^2), formed from the t's so c^2 never cancels, by its series - and the
// plain log of the ratio elsewhere, where that is exact and z, near 1, is not.
float logRatio(float t0, float t1, float c) {
    float a = t0 * t0 + c * c;
    float b = t1 * t1 + c * c;
    float z = (t1 - t0) * (t1 + t0) / (a + b);
    if (abs(z) < 0.1) {
        float z2 = z * z;
        return 2.0 * z * (1.0 + z2 * (1.0 / 3.0 + z2 * (0.2 + z2 * (1.0 / 7.0))));
    }
    return log(b / a);
}

// Mass (.x) and first moment (.y) of one kernel over t in [t0, t1], both up to
// the kernel's constant factor. In theta = atan(t / c):
//
//     bloom:  mass = theta1 - theta0     moment = c ln(cos theta0 / cos theta1)
//     halo:   mass = sin1 - sin0         moment = c (cos theta0 - cos theta1)
//
// each written so a span far off to one side, where both angles are near
// +-PI/2, keeps its precision: the bloom's angle difference in one atan, the
// halo's two terms algebraically (cos theta = c / R). The bloom's moment
// is the same care: as a plain log of (t1^2 + c^2) / (t0^2 + c^2) it lost
// three digits wherever c dwarfs the span - a corner seen from near its centre
// of curvature, where the developed kernel is 10^5 px wide - and linearMass
// subtracts it from a near-equal term. logRatio keeps them.
vec2 kernelMoments(float t0, float t1, float c, bool halo) {
    if (t1 <= t0) {
        return vec2(0.0);
    }
    if (halo) {
        // sin theta = t / R, R = sqrt(t^2 + c^2). A difference of two of them
        // cancels when both are near +-1 - a span far off to one side - so
        // there each is taken as its distance from +-1, c^2 / (R (R + |t|)),
        // and a span over the foot sums two positive terms. No atan: this pass
        // re-bakes every frame under an animation, and three atans here were
        // most of an arc's cost.
        float c2     = c * c;
        float r0     = sqrt(t0 * t0 + c2);
        float r1     = sqrt(t1 * t1 + c2);
        float mass   = (t0 >= 0.0) ? c2 / (r0 * (r0 + t0)) - c2 / (r1 * (r1 + t1))
                     : (t1 <= 0.0) ? c2 / (r1 * (r1 - t1)) - c2 / (r0 * (r0 - t0))
                                   : t1 / r1 - t0 / r0;
        return vec2(mass, c2 * (t1 - t0) * (t1 + t0) / (r0 * r1 * (r0 + r1)));
    }
    return vec2(atan(c * (t1 - t0), c * c + t0 * t1), 0.5 * c * logRatio(t0, t1, c));
}

// One kernel's density at offset t, in the same units.
float kernelDensity(float t, float c, bool halo) {
    float d = t * t + c * c;
    return halo ? c * c * inversesqrt(d) / d : c / d;
}

// Both kernels at once - halo (.x, width cH) and bloom (.y, width cB) - over
// the same span, so each integral below has one copy in the program. Every
// copy is inlined, and the program's compile time follows how many there are:
// see main().
vec2 kernelMass2(float t0, float t1, float cH, float cB) {
    return vec2(kernelMoments(t0, t1, cH, true).x, kernelMoments(t0, t1, cB, false).x);
}

// INTEGRAL over q in [q0, q1] of v(q) K(q - x), v linear from v0 at q0 to v1
// at q1 - one straight piece of an arc's coverage - written about the span's
// own midpoint, so the moment term is the small centred one.
vec2 linearMass2(float q0, float q1, float v0, float v1, float x, float cH, float cB) {
    vec2  kh = kernelMoments(q0 - x, q1 - x, cH, true);
    vec2  kb = kernelMoments(q0 - x, q1 - x, cB, false);
    float tm = 0.5 * (q0 + q1) - x;
    float vm = 0.5 * (v0 + v1);
    float s  = (v1 - v0) / (q1 - q0);
    return vec2(vm * kh.x + s * (kh.y - tm * kh.x), vm * kb.x + s * (kb.y - tm * kb.x));
}

// INTEGRAL over q in [0, len] of an arc image's coverage times K(q - x): a
// trapezoid rising from 0 at `a` to 1 at `b`, holding 1 to `c`, falling to 0 at
// `d`. Each of its three straight pieces is clipped to the piece of outline and
// skipped if that leaves nothing, so an arc that only reaches a piece with one
// end costs one or two spans. The feather caps (ARC_FEATHER_MAX_SHARE 0.4 of the
// arc each) keep b <= c, so the three never overlap.
//
// ONE linearMass2 in a loop, deliberately: the compiler builds every inlined
// copy of it, and this bake is compiled on the first frame that needs it, so
// its size is a first-frame cost a host sees. The loop's bound comes from the
// data (`spans` is 3 whenever the trapezoid has any width) so it is not
// unrolled back into three copies - measured, three inlined copies of the
// spans cost ~20 ms of compile on an AMD Radeon Pro 5300M.
vec2 trapezoidMass2(float a, float b, float c, float d, float len, float x, float cH, float cB) {
    vec2 mass  = vec2(0.0);
    int  spans = (d > a) ? 3 : 0;
    for (int i = 0; i < spans; i++) {
        float p0 = (i == 0) ? a : ((i == 1) ? b : c);
        float p1 = (i == 0) ? b : ((i == 1) ? c : d);
        float q0 = max(p0, 0.0);
        float q1 = min(p1, len);
        if (q1 <= q0) {
            continue;
        }
        // The coverage at each clipped end: rising, flat at 1, or falling.
        float v0 = (i == 0) ? (q0 - a) / (b - a) : ((i == 1) ? 1.0 : (d - q0) / (d - c));
        float v1 = (i == 0) ? (q1 - a) / (b - a) : ((i == 1) ? 1.0 : (d - q1) / (d - c));
        mass += linearMass2(q0, q1, v0, v1, x, cH, cB);
    }
    return mass;
}

// --- The arcs ----------------------------------------------------------
// The arcs' coverage x intensity along a piece that starts at perimeter
// position `place.x` and runs `len` in direction `place.y`, weighted by the
// halo's kernel (width cH) and the bloom's (cB) about the foot `x` - the ratio
// in the header, (.x halo, .y bloom). `total` is each kernel's mass over the
// whole piece, the ratio's denominator.
vec2 arcsOnPiece(vec2 place, float len, float x, float cH, float cB, vec2 total) {
    vec2  lit  = vec2(0.0);
    float most = 0.0;
    for (int i = 0; i < uArcCount; i++) {
        vec4 arc = uArcs[i];
        if (arc.z <= 0.0 || arc.y <= 1e-6) {
            continue;
        }
        most = max(most, arc.z);
        if (arc.y >= 1.0 - 1e-6) {
            lit += arc.z * total;
            continue;
        }
        // Each end's ramp, as arcCoverContinuous draws it: a free end feathers
        // inward, an abutting one outward, each capped at a share of the arc.
        // c0 / c1 are the ramps' centres, in perimeter position.
        float cap = arc.y * ARC_FEATHER_MAX_SHARE;
        float fT  = min(uTailFeather, cap);
        float fH  = min(uHeadFeather, cap);
        float c0  = arc.x + (coverTailAbuts(arc.w) ? -0.5 * fT : 0.5 * fT);
        float c1  = arc.x + arc.y + (coverHeadAbuts(arc.w) ? 0.5 * fH : -0.5 * fH);
        // The same, along the piece: the trapezoid's four corners, its rising
        // end first, swapped when the piece runs against the perimeter.
        float lo  = (place.y > 0.0) ? c0 - place.x : place.x - c1;
        float hi  = (place.y > 0.0) ? c1 - place.x : place.x - c0;
        float wLo = (place.y > 0.0) ? fT : fH;
        float wHi = (place.y > 0.0) ? fH : fT;
        float k   = floor(lo + 0.5);
        float qa  = lo - k - 0.5 * wLo;
        float qb  = lo - k + 0.5 * wLo;
        float qc  = hi - k - 0.5 * wHi;
        float qd  = hi - k + 0.5 * wHi;
        // Every image of the arc a whole perimeter along that reaches the
        // piece - qd + j > 0 and qa + j < len. Bounds from the data rather than
        // a fixed -1..1, so the compiler keeps this a loop instead of three
        // inlined copies; there is seldom more than one.
        int j0 = int(floor(-qd)) + 1;
        int j1 = int(ceil(len - qa)) - 1;
        for (int j = j0; j <= j1; j++) {
            float f = float(j);
            lit += arc.z * trapezoidMass2(qa + f, qb + f, qc + f, qd + f, len, x, cH, cB);
        }
    }
    // Overlapping arcs sum here where the emission takes their max.
    return min(max(lit / total, vec2(0.0)), vec2(most));
}

// --- The segments ------------------------------------------------------
// 16-point Gauss-Legendre on [-1, 1], positive half; the rule is symmetric.
const vec4 GL16_X0 = vec4(0.0950125098376374, 0.2816035507792589, 0.4580167776572274, 0.6178762444026438);
const vec4 GL16_X1 = vec4(0.7554044083550030, 0.8656312023878318, 0.9445750230732326, 0.9894009349916499);
const vec4 GL16_W0 = vec4(0.1894506104550685, 0.1826034150449236, 0.1691565193950025, 0.1495959888165767);
const vec4 GL16_W1 = vec4(0.1246289712555339, 0.0951585116824928, 0.0622535239386479, 0.0271524594117541);

// INTEGRAL over q in [q0, q1] of bell(q) K(q - x), the bell exp(-((q - qb)
// sy)^2), in the kernel's units, for one kernel of width c. Gauss-Legendre in
// theta = atan((q - x) / c), where the kernel is flat (the bloom) or a cosine
// (the halo) and the bell is smooth whatever the two widths: a narrow kernel's
// peak, which no rule in q resolves, is spread over the whole range, and a wide
// kernel leaves the bell a smooth bump in a range a few radians across at
// most, [q0, q1] being the bell's support.
//
// The 16 nodes run in a loop whose bound comes from the data, so the compiler
// keeps one copy of the node rather than unrolling sixteen - the same
// first-frame compile cost as trapezoidMass2's.
float bellMass(float q0, float q1, float qb, float sy, float x, float c, bool halo) {
    float th0   = atan(q0 - x, c);
    float th1   = atan(q1 - x, c);
    float m     = 0.5 * (th0 + th1);
    float h     = 0.5 * (th1 - th0);
    float sum   = 0.0;
    int   nodes = (h > 0.0) ? 16 : 0;
    for (int i = 0; i < nodes; i++) {
        int   k  = i / 2;
        float n  = (k < 4) ? GL16_X0[k] : GL16_X1[k - 4];
        float wt = (k < 4) ? GL16_W0[k] : GL16_W1[k - 4];
        float th = m + (((i & 1) == 0) ? -n : n) * h;
        float d  = (x + c * tan(th) - qb) * sy;
        sum += wt * exp(-d * d) * (halo ? cos(th) : 1.0);
    }
    return sum * h;
}

// The segments' boost x bell along a piece, as arcsOnPiece has the arcs.
// Summed, as neon.frag's segCoverPt sums them, each from its nearest image as
// segCoverPt takes it: the bell is cut half a perimeter from its centre.
vec2 segmentsOnPiece(vec2 place, float len, float x, float cH, float cB, vec2 total) {
    vec2 lit = vec2(0.0);
    for (int i = 0; i < uSegmentCount; i++) {
        vec4 seg = uSegments[i];
        if (seg.z <= 0.0) {
            continue;
        }
        if (seg.y <= 1e-6) {
            lit += seg.z * total;
            continue;
        }
        float reach = min(5.0 * 0.7071067811865476 / seg.y, 0.5);
        float qb    = (place.y > 0.0) ? seg.x - place.x : place.x - seg.x;
        qb -= floor(qb + 0.5);
        // Every image whose support reaches the piece, bounds from the data as
        // in arcsOnPiece.
        int j0 = int(floor(-reach - qb)) + 1;
        int j1 = int(ceil(len + reach - qb)) - 1;
        for (int j = j0; j <= j1; j++) {
            float centre = qb + float(j);
            float q0     = max(centre - reach, 0.0);
            float q1     = min(centre + reach, len);
            if (q1 <= q0) {
                continue;
            }
            // Both kernels through one call site, for the reason above.
            vec2 mass = vec2(0.0);
            int  kernels = (q1 > q0) ? 2 : 0;
            for (int k = 0; k < kernels; k++) {
                mass[k] = bellMass(q0, q1, centre, seg.y, x, (k == 0) ? cH : cB, k == 0);
            }
            lit += seg.z * mass;
        }
    }
    return max(lit / total, vec2(0.0));
}

// One piece's four numbers, in the output's channel order. Zero where the
// piece has no length - a straight of a circle - whose halo and bloom are zero
// in neon.frag anyway.
vec4 pieceCover(vec2 place, float len, float x, float cH, float cB) {
    vec2 total = kernelMass2(-x, len - x, cH, cB);
    if (total.x <= 0.0 || total.y <= 0.0) {
        return vec4(0.0);
    }
    vec2 arcs = arcsOnPiece(place, len, x, cH, cB, total);
    vec2 segs = segmentsOnPiece(place, len, x, cH, cB, total);
    return vec4(arcs.x, arcs.y, segs.x, segs.y);
}

void main() {
    float kh     = uHaloWidth;
    float bw     = uBloomWidth;
    float ws     = uStraightSize.x;
    float hs     = uStraightSize.y;
    float r      = uRadius;
    float arcLen = PIECES_HALF_PI * r;
    float y      = floor(gl_FragCoord.y);
    int   band   = int(y) / GLOW_COVER_ROWS;
    float row    = y - float(band * GLOW_COVER_ROWS) + 0.5;
    float inner  = glowCoverInner(band, uGlowCoverSplit);
    float split  = float(2 * GLOW_COVER_OVERHANG) + inner;

    // This texel's piece, and up to two developments of it to weight
    // together. pieceCover - and, for a corner, the development - is called
    // from ONE place, a loop over them, and that is load-bearing: everything
    // below it is inlined at each call, the compiler builds every copy, and
    // this program is compiled on the first frame that needs it.
    vec2  place  = vec2(0.0);
    float len    = 0.0;
    int   n      = 0;
    bool  corner = false;
    vec2  xa     = vec2(0.0);
    vec2  w      = vec2(0.0);
    vec2  weight = vec2(1.0, 0.0);
    // The two directions a corner is developed about, when there are two.
    vec2  about0 = vec2(0.0);
    vec2  about1 = vec2(0.0);
    if (gl_FragCoord.x < split) {
        // A straight: the projection and the distance this texel holds.
        len = (band == GLOW_COVER_BAND_NEG_X || band == GLOW_COVER_BAND_POS_X) ? hs : ws;
        if (len > 0.0) {
            xa    = glowCoverStraightAt(gl_FragCoord.x, row, len, kh, inner);
            place = straightStart(band, ws, hs, arcLen);
            n     = 1;
        }
    } else if (r > 0.0) {
        // A corner: the offset from its centre this texel holds, in
        // arcTangentSegment's frame.
        vec2 signs = vec2((band == 1 || band == 3) ? 1.0 : -1.0, (band >= 2) ? 1.0 : -1.0);
        w          = glowCoverCornerAt(gl_FragCoord.x - split, row, r, kh, inner);
        place      = cornerStart(signs, ws, hs, arcLen);
        len        = arcLen;
        corner     = true;
        about0     = arcTangentDirection(w);
        n          = 1;
        if (w.x < 0.0 && w.y < 0.0) {
            // Behind the centre arcTangentSegment develops the arc about
            // whichever end is nearer, which flips across the diagonal, and with
            // it which end of the arc the foot is at. Its halo and bloom are
            // symmetric across that flip; the coverage is not. So both
            // developments are taken and swept between: all of the +y one where
            // this region meets w.x < 0 < w.y (which develops about +y alone),
            // all of the +x one where it meets w.y < 0 < w.x, and half each on
            // the diagonal - neon.frag's cornerFootAngle, before V21, swept the
            // same way for the same reason.
            float toX = 0.5 - 0.5 * (w.y - w.x) / max(-w.x - w.y, ARC_FRAME_EPSILON);
            about0    = vec2(0.0, 1.0);
            about1    = vec2(1.0, 0.0);
            weight    = vec2(1.0 - toX, toX);
            n         = 2;
        }
    }
    vec4 cover = vec4(0.0);
    for (int i = 0; i < n; i++) {
        float foot;
        float cH;
        float cB;
        if (corner) {
            // Arclength q sits at t1 + q / weight along the developed line, so
            // the foot is at -t1 * weight and each kernel is weight times as wide.
            vec4 seg = arcTangentSegmentAbout(w, r, (i == 0) ? about0 : about1);
            foot     = -seg.y * seg.w;
            cH       = seg.w * sqrt(seg.x * seg.x + kh * kh);
            cB       = seg.w * sqrt(seg.x * seg.x + bw * bw);
        } else {
            foot = xa.x;
            cH   = sqrt(xa.y * xa.y + kh * kh);
            cB   = sqrt(xa.y * xa.y + bw * bw);
        }
        cover += weight[i] * pieceCover(place, len, foot, cH, cB);
    }
    cover     = max(cover, vec4(0.0));
    fragColor = cover / (1.0 + cover);
}
