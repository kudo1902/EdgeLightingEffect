precision highp float;

// ---------------------------------------------------------------------------
// Glow coverage pre-pass (V20).
//
// Bakes, for every perimeter position s and every distance a from the line,
// how lit the outline's ARCS are as the halo and the bloom see it from there:
// the arcs' coverage along the outline convolved with each layer's kernel
// along a line at that distance. neon.frag reads one texel of it per piece of
// the emitter, at the piece's foot of perpendicular and its distance, and
// scales that piece's halo and bloom by it.
//
// Why a table
// -----------
// Each piece's halo and bloom has to be scaled by the coverage of THAT piece,
// near ITS foot, under ITS kernel - not by one coverage gathered around the
// fragment, which is dominated by whichever piece is nearest. Scaled by the
// fragment's, a dark stretch borrowed light from lit ones far along the line
// (V19, a thin bright line along it) and the lit far edges were dimmed by the
// dark near one (V20, a groove along it). The exact per-piece coverage is
// closed form, but evaluated in neon.frag for eight pieces per fragment it
// cost 2-3x on a partly lit ring. None of it depends on the fragment except
// through (s, a), and all of it depends on the config only through the arcs,
// the perimeter and the glow radius, so it is baked here once per config
// change - GLOW_COVER_SAMPLES x GLOW_COVER_ROWS texels - and read with one
// filtered fetch per piece.
//
// The kernels
// -----------
// Along a line at distance a, the halo's kernel is k^2 / (t^2 + c^2)^1.5 and
// the bloom's k / (t^2 + c^2), both with c = sqrt(a^2 + k^2) - kh for the
// halo, bw for the bloom (haloSegment / bloomSegment in neon.frag are the
// same integrands over a finite segment). Normalised, their CDFs are
//
//     halo:  1/2 + x / (2 sqrt(x^2 + c^2))
//     bloom: 1/2 + atan(x / c) / PI
//
// An arc's coverage is a plateau of its intensity between two linear ramps -
// its feathers - so its convolution is a difference of two ramp-smoothed
// CDFs, each the CDF's integral differenced across the ramp. Every arc end is
// summed, so nothing switches between ends as a fragment moves.
//
// A segment is a Gaussian bell, which has no closed form against either
// kernel, so it is integrated - see segmentConvolved for the two quadratures
// and the accuracy they were picked for.
//
// Units and layout
// ----------------
// Every length here is a FRACTION OF THE PERIMETER, so one table serves both
// resolution paths: neon.frag's pixel lengths all scale with
// uResolutionScale, the perimeter included, and their ratios do not move.
//
// The perimeter is folded into GLOW_COVER_BANDS bands of GLOW_COVER_ROWS rows
// each, so it gets GLOW_COVER_BANDS x GLOW_COVER_SAMPLES samples inside a
// texture no wider than GLES 3.0 guarantees. In band b, column i holds
// s = (b + (i - 0.5) / GLOW_COVER_SAMPLES) / GLOW_COVER_BANDS - one guard
// texel at each end, holding the neighbouring band's (or, round the seam,
// the other end's) value, so a linear fetch is continuous across both with
// clamp-to-edge addressing. Row j of a band holds a = kh * v / (1 - v),
// v = (j + 0.5) / GLOW_COVER_ROWS: half the rows within one halo width of the
// line, where the halo's coverage moves fastest, the rest out to the far
// field. neon.frag keeps its vertical fetch inside one band.
//
// The line is taken as STRAIGHT through each piece's foot - the perimeter's
// turns are not followed - which is what makes the table a function of (s, a)
// alone. neon.frag compensates at a corner's centre of curvature, the one
// place that matters (see cornerLookup there).
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

uniform float uHeadFeather; ///< HEAD_FEATHER_PX as a fraction of the perimeter.
uniform float uTailFeather; ///< TAIL_FEATHER_PX as a fraction of the perimeter.
uniform float uHaloWidth;   ///< kh as a fraction of the perimeter; also the rows' distance scale.
uniform float uBloomWidth;  ///< bw as a fraction of the perimeter.

const float GLOW_COVER_PI = 3.14159265358979;

// See neon.frag's copies: the abutment bits pick each endpoint's feather
// direction. Values are 0..7, all exact in a float.
bool coverTailAbuts(float flags) { return mod(floor(flags * 0.5), 2.0) >= 0.5; }
bool coverHeadAbuts(float flags) { return mod(floor(flags * 0.25), 2.0) >= 0.5; }

// One kernel's CDF at offset x, and its integral (up to a constant).
float kernelCdf(float x, float c, bool halo) {
    return halo ? 0.5 + 0.5 * x * inversesqrt(x * x + c * c) : 0.5 + atan(x, c) / GLOW_COVER_PI;
}
float kernelCdfIntegral(float x, float c, bool halo) {
    return halo ? 0.5 * (x + sqrt(x * x + c * c))
                : 0.5 * x + (x * atan(x, c) - 0.5 * c * log(x * x + c * c)) / GLOW_COVER_PI;
}

// The CDF smoothed by a linear ramp of width w centred on the step: how much
// of the kernel lies past a feathered end, x the fragment's offset past it.
float rampCdf(float x, float w, float c, bool halo) {
    if (w < 1e-7) {
        return kernelCdf(x, c, halo);
    }
    return (kernelCdfIntegral(x + 0.5 * w, c, halo) - kernelCdfIntegral(x - 0.5 * w, c, halo)) / w;
}

// The arcs' coverage x intensity at perimeter position s, under a kernel of
// width c, all in perimeter fractions.
float arcsConvolved(float s, float c, bool halo) {
    float cover = 0.0;
    float most  = 0.0;
    for (int i = 0; i < uArcCount; i++) {
        vec4 arc = uArcs[i];
        if (arc.z <= 0.0 || arc.y <= 1e-6) {
            continue;
        }
        most = max(most, arc.z);
        if (arc.y >= 1.0 - 1e-6) {
            cover += arc.z;
            continue;
        }
        // Each end's ramp, as arcCoverContinuous draws it: a free end feathers
        // inward, an abutting one outward, each capped at a share of the arc.
        float cap = arc.y * ARC_FEATHER_MAX_SHARE;
        float fT  = min(uTailFeather, cap);
        float fH  = min(uHeadFeather, cap);
        float c0  = arc.x + (coverTailAbuts(arc.w) ? -0.5 * fT : 0.5 * fT);
        float c1  = arc.x + arc.y + (coverHeadAbuts(arc.w) ? 0.5 * fH : -0.5 * fH);
        float len = c1 - c0;
        // The shorter of the arc and the gap it leaves, taken at the image
        // nearest s - the other images are past half the perimeter away, out
        // in the kernel's tail. The gap is the complement, ramps reversed.
        float v;
        if (len <= 0.5) {
            float d = s - 0.5 * (c0 + c1);
            d -= floor(d + 0.5);
            v = rampCdf(d + 0.5 * len, fT, c, halo) - rampCdf(d - 0.5 * len, fH, c, halo);
        } else {
            float gap = 1.0 - len;
            float d   = s - (c1 + 0.5 * gap);
            d -= floor(d + 0.5);
            v = 1.0 - (rampCdf(d + 0.5 * gap, fH, c, halo) - rampCdf(d - 0.5 * gap, fT, c, halo));
        }
        cover += arc.z * v;
    }
    // Overlapping arcs sum here where the emission takes their max.
    return min(cover, most);
}

// One kernel's normalised density at offset t, and its mass beyond |t| > r.
float kernelDensity(float t, float c, bool halo) {
    return halo ? 0.5 * c * c * pow(t * t + c * c, -1.5) : c / (GLOW_COVER_PI * (t * t + c * c));
}
float kernelTail(float r, float c, bool halo) {
    return halo ? 1.0 - r * inversesqrt(r * r + c * c) : 1.0 - 2.0 * atan(r, c) / GLOW_COVER_PI;
}

// 8-point Gauss-Hermite (weight exp(-u^2)) and Gauss-Legendre nodes, positive
// half; both rules are symmetric.
const vec4 GH_X = vec4(0.3811869902073221, 1.1571937124467802, 1.9816567566958429, 2.9306374202572440);
const vec4 GH_W = vec4(6.611470125582413e-01, 2.078023258148919e-01, 1.707798300741348e-02, 1.996040722113676e-04);
const vec4 GL_X = vec4(0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363);
const vec4 GL_W = vec4(0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763);

// One segment's bell, exp(-(x * sy)^2), convolved with one kernel of width c:
// x is the offset from the bell's centre, all in perimeter fractions. The
// integrand has two features at two scales - the bell (sigma = 1 / (sqrt 2 sy))
// and the kernel's peak (c) - and no single quadrature resolves both, so:
//
//   - c >= 2 sigma: Gauss-Hermite over the bell. The kernel is smooth across
//     it, and the bell is exactly the rule's weight.
//   - c < 2 sigma: the bell's own value plus a correction,
//         bell(x) (1 - tail(R)) + INTEGRAL_{-R..R} (bell(x - t) - bell(x)) K(t) dt,
//     R = |x| + 5 sigma, Gauss-Legendre over [-R, 0] and [0, R]. The
//     correction vanishes at the kernel's peak, so the narrow feature barely
//     contributes, and the panels split at it.
//
// Against a brute-force integral over kernel widths from 0.005 to 100 sigma
// and offsets out to 100 sigma, the worst error is 1.05e-3 of the boost, and
// the switch between the two moves the value by less than that.
float segmentConvolved(float x, float sy, float c, bool halo) {
    if (sy <= 1e-6) {
        return 1.0;
    }
    float sigma = 0.7071067811865476 / sy;
    float sum   = 0.0;
    if (c >= 2.0 * sigma) {
        for (int i = 0; i < 4; i++) {
            float u = GH_X[i] / sy;
            sum += GH_W[i] * (kernelDensity(x - u, c, halo) + kernelDensity(x + u, c, halo));
        }
        return sum / sy;
    }
    float r  = abs(x) + 5.0 * sigma;
    float bx = exp(-(x * sy) * (x * sy));
    float h  = 0.5 * r;
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 4; k++) {
            float t  = (k < 2 ? -h : h) + ((k & 1) == 0 ? -1.0 : 1.0) * GL_X[i] * h;
            float d  = (x - t) * sy;
            sum += GL_W[i] * h * (exp(-d * d) - bx) * kernelDensity(t, c, halo);
        }
    }
    return bx * (1.0 - kernelTail(r, c, halo)) + sum;
}

// The segments' boost x bell at perimeter position s, under a kernel of
// width c, in perimeter fractions. Summed, as neon.frag's segCoverPt sums
// them.
float segmentsConvolved(float s, float c, bool halo) {
    float cover = 0.0;
    for (int i = 0; i < uSegmentCount; i++) {
        vec4 seg = uSegments[i];
        if (seg.z <= 0.0) {
            continue;
        }
        float x = s - seg.x;
        x -= floor(x + 0.5);
        cover += seg.z * segmentConvolved(x, seg.y, c, halo);
    }
    return cover;
}

void main() {
    // Which band this texel is in, and its perimeter position and distance.
    float y     = floor(gl_FragCoord.y);
    float band  = floor(y / float(GLOW_COVER_ROWS));
    float s     = (band + (floor(gl_FragCoord.x) - 0.5) / float(GLOW_COVER_SAMPLES)) / float(GLOW_COVER_BANDS);
    s          -= floor(s);
    float v     = (y - band * float(GLOW_COVER_ROWS) + 0.5) / float(GLOW_COVER_ROWS);
    float a     = uHaloWidth * v / (1.0 - v);
    float cHalo  = sqrt(a * a + uHaloWidth * uHaloWidth);
    float cBloom = sqrt(a * a + uBloomWidth * uBloomWidth);
    vec4  cover = vec4(arcsConvolved(s, cHalo, true), arcsConvolved(s, cBloom, false),
                       segmentsConvolved(s, cHalo, true), segmentsConvolved(s, cBloom, false));
    cover       = max(cover, vec4(0.0));
    fragColor   = cover / (1.0 + cover);
}
