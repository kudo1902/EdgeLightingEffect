precision highp float;

// ---------------------------------------------------------------------------
// Rain-on-glass droplets, confined to a band along the rounded-rect perimeter.
//
// Rain falls DOWN. The droplet field is hashed in screen space with a single
// global gravity direction, exactly as real rain behaves - it does not flow
// around the perimeter loop, which would read as circulating water rather than
// rain. What the perimeter geometry controls is *where* the rain is allowed to
// show and *how big* the drops are:
//
//   * A rounded-box SDF masks everything outside a band of @c uBandWidth
//     pixels, on the side selected by @c uGlowSide.
//   * Droplet cell size is derived from @c uBandWidth rather than from the
//     viewport, so drops fit the band however thin it is. (Sizing off the
//     viewport was the original bug: at a 20px band you saw slivers of drops
//     tens of pixels across.)
//
// Because gravity is global and the grid is screen-space, the field never
// shears or tears - but that means the band's orientation matters, and the
// band is only @c uBandWidth pixels wide across. Two mechanisms keep the field
// from being guillotined by that, neither of which moves a drop:
//
//   * Whole-drop fade (@ref BandFade). A drop is faded by where its CENTRE
//     sits across the band, not by where the current fragment sits. Drops
//     therefore fade in and out as a whole while crossing, instead of being
//     sliced along a straight line with a flat, rim-less cut face. The fade
//     window scales with each drop's own radius, so it holds at any band width
//     or lane count.
//   * Trail LENGTH is orientation-gated. Down a vertical run a trail can
//     stretch the whole cell, because it runs along the band. Across a
//     horizontal run the only room available is the band's thickness, so the
//     tail is cut to a fraction of that: a short teardrop that fits, rather
//     than a full-length streak sheared flat into a rectangle. A trail also
//     inherits its own head's band fade, so a tail never outlives the drop
//     that drew it - but the discrete beads shed ALONG a trail are separate
//     bodies of water and are faded by their own centres, since a bead can be
//     the better part of a cell away from the head that shed it.
//
// Layer *amplitudes* are likewise modulated by how vertical the local edge is:
// trickles on the sides, condensation beads along the top and bottom. All of
// this is amplitude only - never position - precisely so the grid stays
// unsheared.
//
// Both mechanisms have to stay continuous through the corners, which rules out
// reading the SDF's GRADIENT. A box SDF creases along the medial diagonal
// inside every corner: the gradient flips 90 degrees over about a pixel there,
// so anything derived from it inherits a hard diagonal seam. @ref BandAcross
// samples the field itself at the point of interest and the orientation mix
// below reads per-axis face distances; both are continuous everywhere.
//
// @c uGlowSide selects which side of the edge the band occupies:
//   OUTSIDE -> band grows outward from the edge.
//   INSIDE  -> band grows inward.
//   BOTH    -> band straddles the edge, centred on it.
//
// Drops are self-lit: transparent body plus a crescent rim and a specular dot.
// There is no framebuffer capture and no refraction pass - refraction was
// invisible over the smooth neon gradient (the only backdrop this band ever
// sees), so the whole capture/lens/wet-glass path was removed and only this
// highlight-only shading remains.
//
// Droplet field adapted from the well-known Shadertoy rain technique
// (grid-hashed trickling drops with trails; see "Heartfelt" by Martijn
// Steinrucken / The Art of Code and its many forks, e.g. tdG3Rw).
// ---------------------------------------------------------------------------

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

/// One droplet cell spans this many uv units. The droplet grid inside
/// DropLayer is (12, 2) cells per uv unit with a 6:1 tall aspect, so dividing
/// screen pixels by (CELL_UV * cellPx) makes one cell exactly cellPx wide -
/// and 6 * cellPx tall, which is the room the trail needs.
#define CELL_UV 12.0

/// Trail length on a horizontal run, as a fraction of the band's thickness.
/// The band is all the room a tail has there, so this has to stay well under
/// 1.0.
#define TRAIL_FLAT_SPAN 0.7

/// ...and no longer than this many drop diameters. The band bound alone is
/// the real constraint, but on its own it makes the tail's character depend on
/// @c uLanes: at one lane a tail is shorter than a drop is wide, at four it
/// would be three times longer, which reads as a streak rather than a bead
/// being dragged. Taking the smaller of the two keeps it a bead at any count.
#define TRAIL_FLAT_DROPS 2.0

in vec2 vPos; ///< Fullscreen NDC ([-1,+1]); we drive UVs off gl_FragCoord.
out vec4 fragColor;

uniform vec2  uRectSize;          ///< Rect size (px).
uniform vec2  uRectCenter;        ///< Rect centre (px) in framebuffer space.
uniform float uCornerRadius;
uniform float uTime;
uniform float uAmount;
uniform float uSpeed;
uniform int   uLanes;             ///< Droplet lanes across the band (>= 1).
uniform vec4  uTint;
uniform int   uGlowSide;          ///< GLOW_SIDE_BOTH / INSIDE / OUTSIDE.
uniform float uGlowSideSoftness;  ///< Band-boundary feather width in pixels.
uniform float uBandWidth;         ///< Band thickness in pixels; also sets droplet size.
uniform float uBandOffset;        ///< Gap in pixels between the rect edge and the band's inner boundary.

// Rect frame for the current fragment. Set once at the top of main() and read
// by the droplet field, which would otherwise need these threaded through
// three functions that are each evaluated three times.
float gBandWidth;   ///< Band thickness in px (>= 1).
vec2  gHalfSize;    ///< Rect half-extents in px.
vec2  gRectLocal;   ///< This fragment's position in rect-local px.
float gRuns;        ///< 1 where the band runs vertically, 0 where horizontal.

#define S(a, b, t) smoothstep(a, b, t)

// ---------------------------------------------------------------------------
// Hash helpers
// ---------------------------------------------------------------------------

vec3 N13(float p) {
    vec3 p3 = fract(vec3(p) * vec3(0.1031, 0.11369, 0.13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}

float N(float t) {
    return fract(sin(t * 12345.564) * 7658.76);
}

float Saw(float b, float t) {
    return S(0.0, b, t) * S(1.0, b, t);
}

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// ---------------------------------------------------------------------------
// Band
// ---------------------------------------------------------------------------

/// Band coordinate of an arbitrary rect-local point: 0 at the band's inner
/// boundary, 1 at its outer. The band mask and the per-drop fade both go
/// through here, so the two can never disagree about where the band is.
float BandAcross(vec2 localPx) {
    float sdl = sdRoundBox(localPx, gHalfSize, uCornerRadius);
    float depth; ///< Into the band, measured from its inner boundary outward.
    if (uGlowSide == GLOW_SIDE_INSIDE)
    {
        depth = -sdl - uBandOffset;
    }
    else if (uGlowSide == GLOW_SIDE_OUTSIDE)
    {
        depth = sdl - uBandOffset;
    }
    else
    {
        // Straddle the edge: the band is centred on sd = 0.
        depth = sdl + gBandWidth * 0.5 - uBandOffset;
    }
    return depth / gBandWidth;
}

/// Fade a whole drop by where its CENTRE sits across the band, rather than
/// letting the band mask slice its body.
///
/// @param offsetPx fragment-minus-centre, in pixels.
/// @param radiusPx the drop's radius, in pixels.
///
/// The centre's band coordinate is sampled from the field at the centre
/// itself, not linearised from this fragment along the SDF normal. Linearising
/// is cheaper but wrong at the corners: the box SDF creases along the medial
/// diagonal there, so the normal flips 90 degrees over about a pixel and the
/// two halves of one drop resolve to completely different centres - which put
/// a hard straight cut through every drop straddling that diagonal.
///
/// A drop is at full brightness once it clears both boundaries by its own
/// radius and gone by the time its centre reaches one, so the window scales
/// with drop size - correct for both trickle layers, the static beads, and any
/// @c uLanes setting. The radius ratio is clamped below 0.5 so the two ramps
/// cannot overlap; without it a drop wider than the band could never reach
/// full brightness.
///
/// Closing the window ON the boundary rather than a radius past it is what
/// lets @c bandMask go back to being a thin guard. A drop still overhangs by
/// up to its radius while fading, but only its outermost rim - where the drop
/// mask is already near zero, and below the @c S(0.3, 1.0, c) threshold in
/// @ref Drops - so the mask has nothing left to cut off.
float BandFade(vec2 offsetPx, float radiusPx) {
    float aC = BandAcross(gRectLocal - offsetPx);
    float r = clamp(radiusPx / gBandWidth, 0.02, 0.45);
    return S(0.0, r, aC) * S(1.0, 1.0 - r, aC);
}

// ---------------------------------------------------------------------------
// Droplet field (screen space, gravity-aligned)
// ---------------------------------------------------------------------------
//
// Cells are 6:1 tall so each drop has room for the trail it leaves behind.
// uv.y increases upward (gl_FragCoord convention), so advancing the pattern
// along +uv.y makes drops travel downward.
//
// @c uvToPx is how many pixels one uv unit spans for this call. It is what
// ties the field back to the band, and it differs per layer - the fine layer
// is sampled at uv * 1.85, so its cells are 1.85x smaller on screen.

vec2 DropLayer(vec2 uv, float t, float uvToPx) {
    vec2 baseUV = uv;
    uv.y += t * 0.75;
    vec2 a = vec2(6.0, 1.0);
    vec2 grid = a * 2.0;
    vec2 id = floor(uv * grid);

    float colShift = N(id.x); ///< Per-column phase so columns do not fall in lockstep.
    uv.y += colShift;
    id = floor(uv * grid);

    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv * grid) - vec2(0.5, 0.0);

    float x = n.x - 0.5;
    float y = baseUV.y * 20.0;
    float wiggle = sin(y + sin(y));
    x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
    x *= 0.7;

    float ti = fract(t + n.z);
    y = (Saw(0.85, ti) - 0.5) * 0.9 + 0.5;
    vec2 p = vec2(x, y);
    float d = length((st - p) * a.yx);
    // A cell is cellWidthPx wide and 6x that tall, so (st - p) * a.yx is the
    // offset from the centre in units of cell WIDTHS - isotropic, which is what
    // makes `d` a true radius and what lets one scalar convert it to pixels.
    // The uv.y translations above are pure translations, so they leave this
    // centre-to-fragment offset untouched.
    float cellWidthPx = uvToPx / grid.x;
    // BandFade resolves the drop's centre from this offset, so evaluating it
    // anywhere in the cell - including far up the trail - yields the HEAD's
    // fade. The trail reuses it, which is what stops a tail outliving its drop
    // when the head fades out at a boundary.
    float headFade = BandFade((st - p) * a.yx * cellWidthPx, 0.4 * cellWidthPx);
    float mainDrop = S(0.4, 0.0, d) * headFade;

    // Trail length. Along a vertical run the tail can run to the top of the
    // cell. Across a horizontal run it only has the band's thickness to live
    // in, so it is cut to TRAIL_FLAT_SPAN of that - in pixels, which makes it
    // identical for both trickle layers despite their different cell sizes.
    // Shortening rather than deleting is the point: `r` also drives the tail's
    // width, so a short tail is a narrow one and tapers to a teardrop instead
    // of ending in the flat-topped rectangle a sheared full-length trail left.
    float flatPx = min(TRAIL_FLAT_SPAN * gBandWidth, TRAIL_FLAT_DROPS * 0.8 * cellWidthPx);
    float flatLen = min(flatPx / (6.0 * cellWidthPx), 1.0 - y);
    float trailLen = max(mix(flatLen, 1.0 - y, gRuns), 0.02);
    float r = sqrt(S(y + trailLen, y, st.y));
    float cd = abs(st.x - x);
    float trail = S(0.23 * r, 0.15 * r * r, cd);
    float trailFront = S(-0.02, 0.02, st.y - y);
    trail *= trailFront * r * r * headFade;

    // Beads shed along the trail.
    y = baseUV.y;
    float trail2 = S(0.2 * r, 0.0, cd);
    y = fract(y * 10.0) + (st.y - 0.5);
    float dd = length(st - vec2(x, y));
    float droplets = S(0.3, 0.0, dd) * trail2;

    // A bead gets its OWN band fade, not the head's. It can sit most of a cell
    // away from the head, so borrowing the head's fade left it at full
    // amplitude out at a boundary, where the band mask cut it - the exact
    // slicing @ref BandFade exists to remove.
    //
    // The bead series repeats every 0.1 uv down the trail rather than once per
    // cell, so one unit of (st.y - y) is uvToPx / 10 pixels of screen, not the
    // 6 * cellWidthPx the head uses - and it runs the other way, since y here
    // rises as the fragment descends. Across x the bead sits on the trail, so
    // that offset is the head's.
    float beadPitchPx = uvToPx / 10.0;
    float beadFade = BandFade(vec2((st.x - x) * cellWidthPx, -(st.y - y) * beadPitchPx),
                              0.3 * cellWidthPx);

    float m = mainDrop + droplets * r * trailFront * beadFade;
    return vec2(m, trail);
}

/// Static condensation beads - they fade in and out in place rather than
/// running, which is what rain actually does on a near-horizontal surface.
///
/// These beads are the ONLY thing that shows on horizontal runs of the band
/// (rain cannot streak down a horizontal edge), so the cell size is tuned to
/// give a comfortable per-band count rather than the sparse "condensation on
/// a windowpane" look the original tuning aimed for.
float StaticDrops(vec2 uv, float t, float uvToPx) {
    // Cells are square here, so one scalar converts cell units to pixels.
    float cellSizePx = uvToPx / (CELL_UV * 0.85);
    uv *= CELL_UV * 0.85;
    vec2 id = floor(uv);
    uv = fract(uv) - 0.5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    vec2 p = (n.xy - 0.5) * 0.7;
    float d = length(uv - p);
    float fade = Saw(0.025, fract(t + n.z));
    return S(0.3, 0.0, d) * fract(n.z * 10.0) * fade *
           BandFade((uv - p) * cellSizePx, 0.3 * cellSizePx);
}

vec2 Drops(vec2 uv, float t, float l0, float l1, float l2, float uvToPx) {
    float s = StaticDrops(uv, t, uvToPx) * l0;
    vec2 m1 = DropLayer(uv, t, uvToPx) * l1;
    vec2 m2 = DropLayer(uv * 1.85, t, uvToPx / 1.85) * l2;

    float c = s + m1.x + m2.x;
    c = S(0.3, 1.0, c);
    // m1/m2 are already scaled by l1/l2. The trail channel used to be weighted
    // a second time by l0/l1, which meant trails picked up the static-bead
    // amplitude - and that is boosted 1.6x on horizontal runs, exactly where
    // the band shears trails into flat rectangles.
    return vec2(c, max(m1.y, m2.y));
}

// ---------------------------------------------------------------------------

void main() {
    vec2 p = gl_FragCoord.xy - uRectCenter; ///< Rect-local pixels.
    vec2 halfSize = uRectSize * 0.5;
    float bandWidth = max(uBandWidth, 1.0);

    // Rect frame for the droplet field, which has to evaluate the band at
    // arbitrary points (drop centres), not just at this fragment.
    gBandWidth = bandWidth;
    gHalfSize = halfSize;
    gRectLocal = p;

    float across = BandAcross(p); ///< 0 at the inner boundary, 1 at the outer.

    // Early bail. A thin band is a small slice of the viewport, so rejecting
    // before any droplet work is where most of this pass's cost goes away.
    if (across < -0.25 || across > 1.25)
    {
        discard;
    }

    // Band boundary feather, expressed in the same normalised units. This is a
    // guard, not the fade: every layer already fades its own drops out by the
    // time their centres reach a boundary, so the mask only has to keep the
    // band's own edge from being a hard line.
    //
    // It is therefore floored at a twentieth of the band, not a quarter. A
    // quarter pinned the value above anything uGlowSideSoftness could ask for
    // at usual settings - the uniform did nothing below bandWidth / 4, and the
    // mask only reached 1.0 across the middle half of the band, dimming every
    // drop a second time on top of its own fade.
    float soft = clamp(uGlowSideSoftness / bandWidth, 0.05, 0.5);
    float bandMask = S(0.0, soft, across) * S(1.0, 1.0 - soft, across);
    if (bandMask <= 0.0)
    {
        discard;
    }

    // --- Droplet grid ----------------------------------------------------
    // Cell size comes from the band, not the viewport, so drops fit the band
    // at any thickness. `lanes` is how many drops sit side by side across it.
    float lanes = float(max(uLanes, 1));
    float cellPx = bandWidth / lanes;
    float uvToPx = CELL_UV * cellPx;
    vec2 uv = gl_FragCoord.xy / uvToPx;

    float t = uTime * 0.2 * uSpeed;

    // --- Orientation-aware layer mix -------------------------------------
    // How vertical the local run of band is. `q` is the per-axis distance to
    // the rounded rect's faces, so q.x - q.y says which face is nearer:
    // strongly positive down the left/right runs, where rain can streak;
    // strongly negative along the top/bottom, where it can only bead; crossing
    // zero at the corners.
    //
    // This deliberately does not use the SDF gradient. The gradient is exactly
    // what creases along a corner's medial diagonal, and a trail term built on
    // it snapped between 0 and 1 across that line. Face distances are
    // continuous everywhere, and the transition scales with the band, so trail
    // length eases down over roughly one band width approaching a corner
    // instead of ending at a seam.
    vec2 q = abs(p) - halfSize + uCornerRadius;
    float runs = S(-bandWidth, bandWidth, q.x - q.y); ///< 1 where the band is vertical.
    gRuns = runs;

    float rain = clamp(uAmount, 0.0, 1.0);
    // Gravity is global: the trickling layers are active everywhere, not just
    // on vertical runs. On horizontal runs the drops just cross the band
    // vertically instead of streaking along its length, which is what falling
    // rain looks like passing through a narrow slit - BandFade is what makes
    // that crossing read as a drop fading through rather than a sliced one,
    // and the shortened tail keeps it a drop rather than a streak.
    // Static condensation stays as a mild base density and is still weighted a
    // little higher on the horizontal runs, where drops pass through quickly
    // and beads sit longer.
    float staticDrops = S(-0.5, 1.0, rain) * mix(1.6, 0.5, runs);
    float layer1 = S(0.25, 0.75, rain);
    float layer2 = S(0.0, 0.5, rain);

    vec2 c = Drops(uv, t, staticDrops, layer1, layer2, uvToPx);

    // Height-field gradient in cell space - independent of viewport size.
    vec2 e = vec2(0.001, 0.0);
    float cx = Drops(uv + e.xy, t, staticDrops, layer1, layer2, uvToPx).x;
    float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2, uvToPx).x;
    vec2 normal = vec2(cx - c.x, cy - c.x);
    vec2 nrm = normal / max(length(normal), 1e-5);

    // --- Water shading ---------------------------------------------------
    // Water has no pigment. A drop is legible only through what it does to the
    // light behind it, so its body stays mostly transparent and the two things
    // that actually read as water are drawn on top:
    //
    //   rim  - a crescent at the drop's edge, derived from the drop MASK
    //          (c.x * (1 - c.x) * 4), not from the height-field gradient. The
    //          gradient saturates to 1 across the whole drop at any usable
    //          gain, which is what made older passes fill drops solid white.
    //          The mask-based form stays a thin outline at any drop size.
    //          Because BandFade attenuates the MASK rather than the finished
    //          shading, the rim re-forms around a fading drop's shrinking
    //          silhouette instead of leaving a flat, rim-less cut face.
    //   spec - one tight hotspot per drop, exponent 16 so it's a dot, not a
    //          broad sheen.
    float rim = pow(clamp(c.x * (1.0 - c.x) * 4.0, 0.0, 1.0), 1.5);
    vec2 lightDir = normalize(vec2(-0.4, 0.8));
    // Weight the rim toward the lit side. An evenly bright ring reads as a
    // soap bubble; a real drop catches the light as a crescent.
    float facing = dot(nrm, lightDir) * 0.5 + 0.5;
    rim *= 0.35 + 0.65 * facing;
    float spec = pow(max(0.0, dot(nrm, lightDir)), 16.0) * c.x;

    float bright = clamp(rim * 0.5 + spec * 0.6 + c.y * 0.25, 0.0, 1.0);

    // Composite premultiplied: a faint tinted body suggests refraction the
    // drop doesn't actually do, then self-lit highlights add on top.
    vec3 bodyColor = uTint.rgb;
    float bodyAlpha = c.x * 0.10;

    vec3 premul = bodyColor * bodyAlpha + vec3(1.0) * bright;
    float dropAlpha = clamp(bodyAlpha + bright, 0.0, 1.0);

    // uTint.a scales the whole drop's visibility. Because we output
    // premultiplied, both the RGB and the alpha have to be scaled by it
    // uniformly - scaling alpha alone would darken drops without fading them.
    // bandMask stays as a guaranteed-zero guard at the boundary; the drops
    // themselves have already faded by the time it bites.
    float alpha = bandMask * dropAlpha * uTint.a;
    if (alpha <= 0.0)
    {
        discard;
    }

    fragColor = vec4(premul * bandMask * uTint.a, alpha);
}
