// ---------------------------------------------------------------------------
// The emitter's pieces, shared by neon.frag and neon-glow-cover.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header, into the two programs that have to agree on it to the texel:
// neon.frag, which sums the halo and bloom over the emitter's eight pieces and
// scales each by that piece's coverage from the glow coverage table, and
// neon-glow-cover.frag, which bakes the table. Three things live here because
// both sides need them:
//
//   - minimaxAtan, the atan both sides develop a corner with.
//   - arcTangentSegment, how a corner arc is developed onto a straight line.
//     neon.frag integrates a corner's halo and bloom along that line, and the
//     bake convolves the corner's coverage along the same line, so what scales
//     a corner's glow is the coverage of the extent that glow was integrated
//     over.
//   - the table's layout, as a forward map (where a piece's coverage is read,
//     for neon.frag) next to its inverse (what each texel holds, for the
//     bake). The two must be exact inverses, which is the reason they sit
//     side by side rather than one in each file.
//
// Every length here is in one unit chosen by the caller - px in neon.frag,
// perimeter fractions in the bake. The layout depends only on ratios of
// lengths (to the halo width kh, to a piece's own length), so the two agree
// whatever the unit and at every resolution scale.
// ---------------------------------------------------------------------------

precision highp float;

// Own names: neon-common.glsl, which neon.frag also includes, declares PI and
// HALF_PI as constants, and the bake does not include it.
#define PIECES_PI      3.141592653589793
#define PIECES_HALF_PI 1.5707963267948966

// --- A cheaper two-argument atan ----------------------------------------
// The shading's two hottest transcendentals are atans: bloomSegment's (every
// straight's bloom and every corner's, in neon.frag) and the corner
// development's `th` below. GLSL's atan(y, x) is the driver's, and measured on
// an AMD Radeon Pro 5300M at scale 1.0 this octant-reduced minimax polynomial
// in its place took 11.6% off a frame whose config animates (an intensity
// pulse) and 8.9% off an arc wipe, moving 46-69 of 8.3M channels by 1 level.
// Its error is at most 1.7e-6 rad over the whole range - a few float ulps of
// PI - so nothing downstream can tell it from the built-in.
//
// Here rather than in neon.frag so the coverage bake develops the corners
// through the same function as the read: the two have to agree on `th`.
// Undefined only where atan(y, x) is (both zero): it returns 0 there.
// Measure on each target GPU before trusting the gain - a cheaper atan is the
// most GPU-specific change this file has.
float minimaxAtan(float y, float x) {
    float ax = abs(x);
    float ay = abs(y);
    float t  = min(ax, ay) / max(max(ax, ay), 1e-30);
    float s  = t * t;
    float r  = t * (0.99997726 + s * (-0.33262347 + s * (0.19354346 + s * (-0.11643287 +
               s * (0.05265332 + s * -0.01172120)))));
    r = (ay > ax) ? PIECES_HALF_PI - r : r;
    r = (x < 0.0) ? PIECES_PI - r : r;
    return (y < 0.0) ? -r : r;
}

// --- Corner arcs, developed onto their tangent -------------------------
// The four straights cover the rect's flat runs. Above cornerRadius 0 the
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
// in neon.frag warns - that includes the cornerRadius 0 path, which never
// executes a line of this and still pays 1.026x for the register pressure.
// Re-time both after touching it. An inversesqrt formulation that trades the
// sqrt and the divide for two inversesqrts was measured and came out inside
// the noise, so the readable form stays.
//
// Returns (a, t1, t2, weight) for haloSegment / bloomSegment; the caller
// multiplies the segment by .w. The coverage bake reads the same four numbers
// to lay the arc's coverage along that line: arclength q from the +x end sits
// at t1 + q / weight.
//
// `u` is the direction of the arc point the arc is developed about, which
// arcTangentSegment below picks; the bake also calls this with each of the two
// fallback directions, behind the centre where the pick flips between them.
vec4 arcTangentSegmentAbout(vec2 w, float r, vec2 u) {
    // Arclength from the +x tangent point to the nearest arc point. u is a unit
    // vector in the first quadrant whenever it came from wq, so th is in
    // [0, HALF_PI] and needs no clamp of its own.
    float th  = minimaxAtan(u.y, u.x);
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
    return vec4(a, -lam * th - off, lam * (PIECES_HALF_PI - th) - off, r / lam);
}
// The direction arcTangentSegment develops about: the nearest arc point's.
vec2 arcTangentDirection(vec2 w) {
    vec2  wq = max(w, vec2(0.0));
    float ql = length(wq);
    return (ql > ARC_FRAME_EPSILON) ? wq / ql : ((w.x >= w.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
}
vec4 arcTangentSegment(vec2 w, float r) {
    return arcTangentSegmentAbout(w, r, arcTangentDirection(w));
}

// --- The glow coverage table's layout (V21) ----------------------------
// One table per PIECE of the emitter, because the coverage that scales a
// piece's halo and bloom is that piece's own: its coverage over its own extent,
// weighted by the layer's kernel at the fragment. V20's table was one per
// PERIMETER, indexed by the foot's perimeter position, and convolved along a
// straight line through the foot that ran on past the piece's ends - so near a
// corner where a lit stretch met a dark one, each piece borrowed its
// neighbour's coverage (V21 in docs/review-findings.md).
//
// GLOW_COVER_WIDTH texels across, four bands of GLOW_COVER_ROWS rows down.
// Band b holds straight b at its left and corner b at its right:
//
//   straight   OVERHANG columns, the straight's own `inner` columns, OVERHANG
//              columns: across, the fragment's projection along the straight -
//              uniform over the piece and rational past each end; down, its
//              distance a from the line, a / (a + kh), as V20's table had it. A
//              straight's clipped extent follows from the projection, so two
//              variables still describe it, and its table is symmetric about
//              the line.
//   corner     a guard texel, CORNER_OVERHANG columns, the corner's own
//              SHARED - inner columns, CORNER_OVERHANG, a guard: across, the
//              fragment's direction from the arc's centre - uniform over the
//              arc's own quadrant, then on either side of it to the diagonal
//              behind the centre, where the two sides meet; down, its
//              distance from the centre, the arc's inside in the top half and
//              its outside in the bottom. Polar position determines everything
//              arcTangentSegment returns, inside, outside and behind the centre
//              alike, so two variables describe a corner too.
//
// `inner`, how many of a band's SHARED columns its straight gets, is the
// uniform uGlowCoverSplit (.x the vertical straights' bands, .y the
// horizontal ones'), whole columns, set by NeonRenderer::GetGlowCoverSplit in
// proportion to the straight's and the arc's lengths. It comes from the CPU
// rather than from each shader's own arithmetic because the two shaders work
// in different units, and a split that rounded differently in one of them
// would read every texel of the band from the wrong place. So a circle's
// corners get the columns its zero-length straights do not need, and a long
// rect's straights get its small corners': at a fixed size, every piece gets
// a column density in proportion to its length.
//
// THE READ HAS TO BE CHEAP, AND THAT IS WHY EVERY MAP HERE IS A DIVISION.
// neon.frag computes these coordinates for up to eight pieces, and at scale 1.0
// the program also carries the gather loop, which makes it sensitive to the
// size of everything else in it - including code that a frame never runs.
// Measured on an AMD Radeon Pro 5300M, against the build before V21, on a fully
// lit ring that skips every read: with the corner direction an atan, the
// spacing past each end a log and the inside of the arc spaced by two more
// logs, the whole neon pass was 1.16x slower - 1.163x on a SHARP-cornered ring,
// which never enters the corner block at all - and stubbing the corner
// coordinates out took it to 1.04x. So the direction is a diamond angle (one
// division, see glowCoverDiamond), the spacing past the ends rational, and the
// inside a ratio of square roots, which took it to ~1.08x; the rest came from
// running neon.frag's corner block one arc at a time (addCornerPiece there).
// The bake inverts the maps, where cost does not matter. Do not put an atan or
// a log back in the forward maps without re-timing a fully lit ring at scale
// 1.0.
//
// Which straight is which band, by the side of the rect in vPos it lies on.
// The vertical straights (x = -/+ halfSize.x) have length 2 * straight.y, the
// horizontal ones 2 * straight.x. Corner block b is in band b too.
#define GLOW_COVER_BAND_NEG_X  0
#define GLOW_COVER_BAND_POS_X  1
#define GLOW_COVER_BAND_NEG_Y  2
#define GLOW_COVER_BAND_POS_Y  3

// How far a corner's direction coordinate runs past each end of its arc: the
// arc is [0, 1] in it, and both sides reach the diagonal behind the centre at
// 1.5 past an end - 3/8 of a turn.
#define GLOW_COVER_CORNER_REACH 1.5

// Which corner block - and so which band - a corner's table is in: `signs` is
// the corner's quadrant in vPos, +-1 on each axis.
#ifndef NEON_GLOW_COVER_BAKE
int glowCoverCornerBlock(vec2 signs) {
    return (signs.x > 0.0 ? 1 : 0) + (signs.y > 0.0 ? 2 : 0);
}
#endif

// Band `band`'s straight interior columns, from uGlowCoverSplit.
float glowCoverInner(int band, vec2 split) {
    return (band < 2) ? split.x : split.y;
}

// A distance from the line as a row coordinate in [0, 1]: half the rows within
// one halo width, the rest out to the far field. V20's spacing.
#ifndef NEON_GLOW_COVER_BAKE
float glowCoverDistanceRow(float a, float kh) {
    return a / (a + kh);
}
#else
float glowCoverRowDistance(float v, float kh) {
    return kh * v / (1.0 - v);
}
#endif

// Past a straight's end, `e` the distance past it: half the overhang's columns
// within a quarter of them in halo widths, so a column is kh / 4 at the end
// and the spacing grows in proportion to e beyond. Rational rather than
// logarithmic for the reason above.
#ifndef NEON_GLOW_COVER_BAKE
float glowCoverStraightOver(float e, float kh) {
    return e / (e + 0.25 * float(GLOW_COVER_OVERHANG) * kh);
}
#else
float glowCoverStraightPast(float over, float kh) {
    return 0.25 * float(GLOW_COVER_OVERHANG) * kh * over / (1.0 - over);
}
#endif

// A direction as a "diamond angle": monotone in the true angle and continuous
// all the way round, without an atan. The arc's own quadrant is [0, 1] - 0 at
// its +x end, 1 at +y - and the rest of the turn runs on from either end to
// -1.5 / 2.5, which are both the diagonal behind the centre. `w` need not be
// normalised; at the centre itself any direction is as good as another.
#ifndef NEON_GLOW_COVER_BAKE
float glowCoverDiamond(vec2 w) {
    float l = abs(w.x) + abs(w.y);
    float p = (l > 0.0) ? w.x / l : 1.0;
    return (w.y >= 0.0) ? 1.0 - p : ((p < -0.5) ? 3.0 + p : p - 1.0);
}
#else
vec2 glowCoverDiamondDirection(float d) {
    float q = (d < 0.0) ? d + 4.0 : d;
    float p = (q <= 2.0) ? 1.0 - q : q - 3.0;
    return normalize(vec2(p, ((q <= 2.0) ? 1.0 : -1.0) * (1.0 - abs(p))));
}
#endif

// Past a corner's end, `d` the diamond angle past it: half the columns spaced
// evenly to the diagonal behind the centre, which is what a fragment far from
// the corner needs, and half concentrated at the end, a column about kh / 4 of
// arc there, which is what one beside the arc needs.
#ifndef NEON_GLOW_COVER_BAKE
float glowCoverCornerOver(float d, float r, float kh) {
    float k = float(GLOW_COVER_CORNER_OVERHANG) * kh / (8.0 * max(r, 1e-30));
    float R = GLOW_COVER_CORNER_REACH;
    return 0.5 * d / R + 0.5 * d * (R + k) / (R * (d + k));
}
#endif
// Its inverse: A d + B d / (d + k) = t is a quadratic in d, solved by whichever
// form of its positive root does not cancel.
#ifdef NEON_GLOW_COVER_BAKE
float glowCoverCornerPast(float t, float r, float kh) {
    float k    = float(GLOW_COVER_CORNER_OVERHANG) * kh / (8.0 * max(r, 1e-30));
    float R    = GLOW_COVER_CORNER_REACH;
    float A    = 0.5 / R;
    float b    = A * k + 0.5 * (R + k) / R - t;
    float disc = sqrt(b * b + 4.0 * A * t * k);
    return (b >= 0.0) ? 2.0 * t * k / max(b + disc, 1e-30) : (disc - b) / (2.0 * A);
}
#endif

// Inside a corner's arc the coverage changes fast in two places - beside the
// arc, where the halo's width is kh, and near the centre, where the developed
// arc's width r / lam grows without bound - and slowly between. A ratio of
// square roots spaces the rows finely at both ends: `rho` the distance from the
// centre, 0 on the arc and 1 at the centre. Its inverse is closed form.
#ifndef NEON_GLOW_COVER_BAKE
float glowCoverInsideRow(float rho, float r) {
    float fromArc = sqrt(max(r - rho, 0.0));
    float fromCentre = sqrt(max(rho, 0.0));
    return fromArc / max(fromArc + fromCentre, 1e-30);
}
#else
float glowCoverInsideRadius(float v, float r) {
    return r * (1.0 - v) * (1.0 - v) / (v * v + (1.0 - v) * (1.0 - v));
}
#endif

// Texture coordinate of straight `band`'s coverage: `x` is the fragment's
// projection along it from its t1 end (haloSegment's), UNCLAMPED, `len` the
// straight's length, `a` the distance from its line, `kh` the halo width,
// `inner` glowCoverInner. Clamped to the straight's own texels, since its
// corner's guard is the next one along. The inverse is glowCoverStraightAt.
#ifndef NEON_GLOW_COVER_BAKE
vec2 glowCoverStraightUV(int band, float x, float len, float a, float kh, float inner) {
    float over  = float(GLOW_COVER_OVERHANG) * glowCoverStraightOver(max(-x, 0.0) + max(x - len, 0.0), kh);
    float along = (len > 0.0) ? clamp(x / len, 0.0, 1.0) : 0.5;
    float col   = float(GLOW_COVER_OVERHANG) + inner * along + (x < 0.0 ? -over : over);
    col         = clamp(col, 0.5, float(2 * GLOW_COVER_OVERHANG) + inner - 0.5);
    float row   = clamp(glowCoverDistanceRow(a, kh) * float(GLOW_COVER_ROWS), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2(col / float(GLOW_COVER_WIDTH),
                (float(band * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Texture coordinate of a corner's coverage: `w` is the fragment's offset from
// the arc centre in arcTangentSegment's frame, `block` from
// glowCoverCornerBlock, `inner` its band's straight's glowCoverInner. The
// inverse is glowCoverCornerAt. The seam is the diagonal behind the centre,
// where the two overhangs meet; the guard column at each end of the block
// holds the other end's value, so a fetch is continuous across it.
#ifndef NEON_GLOW_COVER_BAKE
vec2 glowCoverCornerUV(int block, vec2 w, float r, float kh, float inner) {
    float arcCols = float(GLOW_COVER_SHARED) - inner;
    float d       = glowCoverDiamond(w);
    float over    = float(GLOW_COVER_CORNER_OVERHANG) *
                    glowCoverCornerOver(max(-d, 0.0) + max(d - 1.0, 0.0), r, kh);
    float col     = float(GLOW_COVER_CORNER_OVERHANG) + arcCols * clamp(d, 0.0, 1.0) + (d < 0.0 ? -over : over);
    float rho     = length(w);
    float side    = (rho >= r) ? glowCoverDistanceRow(rho - r, kh) : -glowCoverInsideRow(rho, r);
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float row     = clamp(halfRows * (1.0 + side), 0.5, float(GLOW_COVER_ROWS) - 0.5);
    return vec2((float(2 * GLOW_COVER_OVERHANG) + inner + 1.0 + col) / float(GLOW_COVER_WIDTH),
                (float(block * GLOW_COVER_ROWS) + row) / float(GLOW_COVER_HEIGHT));
}
#endif

// Inverse of glowCoverStraightUV for one texel: the projection `x` and the
// distance `a` its centre holds, from its column and row within the band.
#ifdef NEON_GLOW_COVER_BAKE
vec2 glowCoverStraightAt(float col, float row, float len, float kh, float inner) {
    float over = float(GLOW_COVER_OVERHANG);
    float x;
    if (col < over) {
        x = -glowCoverStraightPast((over - col) / over, kh);
    } else if (col > over + inner) {
        x = len + glowCoverStraightPast((col - over - inner) / over, kh);
    } else {
        x = (col - over) / max(inner, 1.0) * len;
    }
    return vec2(x, glowCoverRowDistance(row / float(GLOW_COVER_ROWS), kh));
}
#endif

// Inverse of glowCoverCornerUV for one texel: the offset `w` from the arc
// centre its centre holds, from its column within the corner's block (guards
// included, so 0 and the block's last are the guards) and its row.
#ifdef NEON_GLOW_COVER_BAKE
vec2 glowCoverCornerAt(float col, float row, float r, float kh, float inner) {
    float arcCols = float(GLOW_COVER_SHARED) - inner;
    float over    = float(GLOW_COVER_CORNER_OVERHANG);
    // Past the guard, the column is the same direction the other end's last
    // texel holds: the seam is one direction, approached from both sides.
    float cols    = arcCols + 2.0 * over;
    float c       = col - 1.0;
    c            -= cols * floor(c / cols);
    float d;
    if (c < over || c > over + arcCols) {
        float past = glowCoverCornerPast(((c < over) ? over - c : c - over - arcCols) / over, r, kh);
        d = (c < over) ? -past : 1.0 + past;
    } else {
        d = (c - over) / arcCols;
    }
    float halfRows = 0.5 * float(GLOW_COVER_ROWS);
    float rho = (row >= halfRows) ? r + glowCoverRowDistance((row - halfRows) / halfRows, kh)
                                  : glowCoverInsideRadius((halfRows - row) / halfRows, r);
    return rho * glowCoverDiamondDirection(d);
}
#endif
