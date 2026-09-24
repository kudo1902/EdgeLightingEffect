precision highp float;

// ---------------------------------------------------------------------------
// Rain-on-glass droplets, confined to a region of the rounded rect - either a
// band along its perimeter or the whole pane.
//
// Rain falls DOWN. The droplet field is hashed in screen space with a single
// global gravity direction, exactly as real rain behaves - it does not flow
// around the perimeter loop, which would read as circulating water rather than
// rain. What the geometry controls is *where* the rain is allowed to show:
//
//   * @ref RegionDepth masks everything outside the region, which the CPU
//     hands over as two concentric rounded rects (or one, for a pane).
//   * Droplet cell size comes from @c uDropPitch, a property of the RAIN -
//     not of the viewport, and no longer of the band either. (Sizing off the
//     viewport was the original bug: at a 20px band you saw slivers of drops
//     tens of pixels across. Sizing off the band fixed that but tied drop
//     size to a thickness a pane does not have.)
//
// Because gravity is global and the grid is screen-space, the field never
// shears or tears - but that means a BAND's orientation matters, and a band is
// only @c gSpan pixels wide across. Two mechanisms keep the field from being
// guillotined by that, neither of which moves a drop:
//
//   * Whole-drop fade (@ref RegionFade). A drop is faded by where its CENTRE
//     sits across the band, not by where the current fragment sits. Drops
//     therefore fade in and out as a whole while crossing, instead of being
//     sliced along a straight line with a flat, rim-less cut face. The fade
//     window scales with each drop's own radius, so it holds at any band width
//     or lane count.
//   * Trail LENGTH is orientation-gated. Down a vertical run a trail can
//     stretch the whole cell, because it runs along the band. Across a
//     horizontal run the only room available is the band's thickness, so the
//     tail is cut to a fraction of that: a short teardrop that fits, rather
//     than a full-length streak sheared flat into a rectangle. Trails also
//     inherit their own head's band fade, so a tail never outlives the drop
//     that drew it.
//
// Layer *amplitudes* are likewise modulated by how vertical the local edge is:
// trickles on the sides, condensation beads along the top and bottom. All of
// this is amplitude only - never position - precisely so the grid stays
// unsheared.
//
// Both mechanisms have to stay continuous through the corners, which rules out
// reading the SDF's GRADIENT. A box SDF creases along the medial diagonal
// inside every corner: the gradient flips 90 degrees over about a pixel there,
// so anything derived from it inherits a hard diagonal seam. @ref RegionDepth
// samples the field itself at the point of interest and the orientation mix
// below reads per-axis face distances; both are continuous everywhere.
//
// The region is this layer's OWN geometry - two rounded rects it is handed
// outright, with no relation to the rect the neon draws on. A filled shape is
// the same region with the inner one absent - see @ref RegionDepth.
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
/// the lane count: at one lane a tail is shorter than a drop is wide, at four it
/// would be three times longer, which reads as a streak rather than a bead
/// being dragged. Taking the smaller of the two keeps it a bead at any count.
#define TRAIL_FLAT_DROPS 2.0

in vec2 vPos; ///< Rect-local px from the band quad; we drive UVs off gl_FragCoord.
out vec4 fragColor;

uniform float uTime;
uniform float uAmount;
uniform float uSpeed;
uniform vec4  uTint;
uniform float uGlowSideSoftness;  ///< Region-boundary feather width in pixels.

// The region, as the two rounded rects bounding it - an outer shape minus an
// inner one. They are INDEPENDENT: different centres, extents and corner radii
// are all legal, which is what lets the region be a band of varying thickness,
// an off-centre hole, or a pane with a bite out of it. Two full distance
// fields is the price of that, over the one a concentric pair would share.
//
// The CPU resolves these from the geometry source, the glow side, the band
// width and offset and the shared rect, consuming all of them - see
// ResolveRegion in droplets-renderer.cpp. That is why none of them appears
// here, and why this stage does not know or care which source it came from.
uniform vec2  uOuterCenter;       ///< Bounding shape's centre in framebuffer px.
uniform vec2  uOuterHalf;         ///< ...its half extents.
uniform float uOuterRadius;       ///< ...its corner radius.
uniform vec2  uInnerCenter;       ///< The hole's, all three unread when uHasInner is 0.
uniform vec2  uInnerHalf;
uniform float uInnerRadius;
uniform int   uHasInner;          ///< 0 = a filled shape (one boundary), else a ring (two).
uniform float uDropPitch;         ///< Droplet cell pitch in px, lanes already divided out.

// Rect frame for the current fragment. Set once at the top of main() and read
// by the droplet field, which would otherwise need these threaded through
// three functions that are each evaluated three times.
float gSpan;        ///< Region width in px at THIS fragment (>= 1); see RegionSpan.
bool  gHasInner;    ///< Whether the region has a second boundary.
vec2  gFragPx;      ///< This fragment in framebuffer px (gl_FragCoord.xy).
float gRuns;        ///< 1 where the region runs vertically, 0 where horizontal.

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

/// Depth of an arbitrary rect-local point INTO the wet region, in PIXELS:
/// positive inside, 0 on its boundary, negative outside. The mask, the early
/// bail and the per-drop fade all go through here, so none of them can
/// disagree about where the region is.
///
/// The region is the outer shape minus the inner one - the standard SDF
/// subtraction max(sdOuter, -sdInner), negated - so this is the distance to
/// whichever boundary is nearer. A pane has only the one.
/// Both of the region's distances at a point, in px: @c x is IN from the outer
/// boundary, @c y is OUT from the inner one.
///
/// Every question the region answers - how deep, how wide, which boundary is
/// nearer - comes from this one pair, so it is evaluated ONCE per point of
/// interest and never re-derived. Two full distance fields is the price of
/// letting the two shapes be independent; main() used to pay it twice over by
/// asking for the depth and the span separately.
///
/// @param fbPx the point in FRAMEBUFFER px (gl_FragCoord space), since the two
///        shapes have their own centres and neither is the origin.
/// @c y is meaningless when @c gHasInner is false; @ref RegionDepth and
///    @ref RegionSpan are its only readers and both ignore it there.
vec2 RegionDistances(vec2 fbPx) {
    float dOut = -sdRoundBox(fbPx - uOuterCenter, uOuterHalf, uOuterRadius);
    if (!gHasInner)
    {
        return vec2(dOut, 0.0);
    }
    return vec2(dOut, sdRoundBox(fbPx - uInnerCenter, uInnerHalf, uInnerRadius));
}

/// Depth INTO the region from its NEAREST boundary, in px: positive inside,
/// 0 on the boundary, negative outside.
///
/// The region is the outer shape minus the inner one - the standard SDF
/// subtraction max(sdOuter, -sdInner), negated - so this is the distance to
/// whichever boundary is nearer. A filled shape has only the one.
float RegionDepth(vec2 dist) {
    return gHasInner ? min(dist.x, dist.y) : dist.x;
}

/// Local width of the region at the point @p dist was taken from, in px.
///
/// Constant, and equal to the band width, whenever the two shapes are
/// concentric dilations of one rect - a plain perimeter band. Otherwise it
/// genuinely varies, which is the point: a band can be thicker at the corners
/// than on the straights. Read by the orientation mix, the trail cut and the
/// feather's guard - but NOT by the feather itself, which is sized to the drop.
///
/// A filled shape has no far boundary; the pitch stands in as a finite value
/// for the few terms that are inert there anyway.
float RegionSpan(vec2 dist) {
    return gHasInner ? max(dist.x + dist.y, 1.0) : uDropPitch;
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
/// radius and gone once its centre is a radius outside, so the window scales
/// with drop size - correct for both trickle layers, the static beads, and any
/// lane count. The radius is clamped below half the span so the two ramps
/// cannot overlap; without it a drop wider than the band could never reach
/// full brightness. See @ref RegionFade - that same clamp is what lets one
/// ramp replace the two this once multiplied.
float RegionFade(vec2 offsetPx, float radiusPx) {
    vec2 dist = RegionDistances(gFragPx - offsetPx);
    // Clamped into the span AT THE DROP'S OWN CENTRE - `dist`, not the
    // fragment's `gSpan`. The two are the same number for any concentric pair,
    // so a plain band cannot tell the difference; once the shapes are
    // independent the span varies, and clamping by the fragment gave ONE drop
    // different fade windows across its own body - exactly the sliced-drop
    // artefact this function exists to prevent.
    //
    // The clamp itself keeps a band's two boundaries from both biting: past
    // half the span a drop wider than the band could never reach full
    // brightness. It is also what lets ONE ramp on the nearer boundary stand
    // for the two this used to multiply - below the overlap threshold a product
    // of two monotonic ramps IS their min, because the far one is exactly 1.
    // A pane has one boundary and nothing to clamp against.
    float span = RegionSpan(dist);
    float r = gHasInner ? clamp(radiusPx, 0.02 * span, 0.45 * span) : radiusPx;
    return S(-r, r, RegionDepth(dist));
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
    // RegionFade resolves the drop's centre from this offset, so evaluating it
    // anywhere in the cell - including far up the trail - yields the HEAD's
    // fade. The trail reuses it, which is what stops a tail outliving its drop
    // when the head fades out at a boundary.
    float headFade = RegionFade((st - p) * a.yx * cellWidthPx, 0.4 * cellWidthPx);
    float mainDrop = S(0.4, 0.0, d) * headFade;

    // Trail length. Along a vertical run the tail can run to the top of the
    // cell. Across a horizontal run it only has the band's thickness to live
    // in, so it is cut to TRAIL_FLAT_SPAN of that - in pixels, which makes it
    // identical for both trickle layers despite their different cell sizes.
    // Shortening rather than deleting is the point: `r` also drives the tail's
    // width, so a short tail is a narrow one and tapers to a teardrop instead
    // of ending in the flat-topped rectangle a sheared full-length trail left.
    float flatPx = min(TRAIL_FLAT_SPAN * gSpan, TRAIL_FLAT_DROPS * 0.8 * cellWidthPx);
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

    float m = mainDrop + droplets * r * trailFront * headFade;
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
           RegionFade((uv - p) * cellSizePx, 0.3 * cellSizePx);
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
    vec2 p = gl_FragCoord.xy - uOuterCenter; ///< Outer-local pixels, for `runs`.

    // Frame for the droplet field, which has to evaluate the region at
    // arbitrary points (drop centres), not just at this fragment.
    gFragPx = gl_FragCoord.xy;
    gHasInner = (uHasInner != 0);
    // The region's width HERE. Constant and equal to the band width whenever
    // the two shapes are concentric - every SHARED region - and genuinely
    // varying otherwise. A filled shape has no far bound at all; the pitch
    // stands in as a finite value for the few terms that are inert there.
    // ONE region evaluation: the depth and the local width both come out of
    // the same pair of distances, so the two distance fields are computed once
    // rather than once each.
    vec2 dist = RegionDistances(gFragPx);
    gSpan = RegionSpan(dist);

    float depth = RegionDepth(dist); ///< Px into the region from its nearest boundary.

    // Early bail. A thin band is a small slice of the viewport, so rejecting
    // before any droplet work is where most of this pass's cost goes away.
    //
    // Zero margin is EXACT, not a tightening: the feather below is a hard zero
    // at depth 0, so nothing outside the region is ever written. This used to
    // keep a quarter of the band's width on each side, which cost geometry and
    // bought nothing - verified byte-identical when it went.
    if (depth < 0.0)
    {
        discard;
    }

    // Boundary feather, in px. A ring's is floored at a quarter of the DROP
    // PITCH: whatever still overhangs after the per-drop fade has to vignette
    // out, not be cut off - and an overhang is DROP-sized. A pane's boundary is
    // a hard geometric edge on a smooth gradient, so a single pixel - enough to
    // antialias it and nothing more - is all it wants.
    //
    // This floored at a quarter of the BAND's width until the region became two
    // shapes, and that was the same number: drop size was derived from band
    // width (cellPx = bandWidth / lanes), so at one lane the band was an exact
    // stand-in for the drop it was really sizing against. It still is, whenever
    // a host keeps them equal - nine of twelve band scenes are byte-identical
    // across the change, and the three that are not are exactly the ones with
    // more than one lane, where the drops are 1/lanes the size and their
    // overhang is too.
    //
    // What the band was NOT is independent of the INNER shape: gSpan is
    // dOut + dIn, so moving the hole re-scaled the feather at the OUTER edge as
    // well - measured at 11.7x. See section 1.6 of
    // docs/droplets-region-comparison.md.
    //
    // The one thing the region still gets a say in is the guard: you cannot
    // feather over more room than there is, so it never exceeds half the local
    // width. That engages only on a band thinner than twice the drop feather.
    float softPx = gHasInner
        ? min(max(uGlowSideSoftness, uDropPitch * 0.25), gSpan * 0.5)
        : max(uGlowSideSoftness, 1.0);
    float bandMask = S(0.0, softPx, depth);
    if (bandMask <= 0.0)
    {
        discard;
    }

    // --- Droplet grid ----------------------------------------------------
    // Cell size comes from the rain, not the viewport and not the region. It
    // has to be one GLOBAL scalar: everything region-relative below is an
    // amplitude, never a position, precisely so the screen-space grid never
    // shears, and a pitch that tracked the region would shear it.
    float cellPx = uDropPitch;
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
    //
    // A PANE has no run direction - orientation is a property of a curve, and
    // an interior fragment of an area is not on one. So it is all verticals:
    // rain streaks down a windowpane everywhere, top to bottom.
    // Face distances of the OUTER shape - the one whose edges the region runs
    // along. Under an independent pair the inner shape may be anywhere, so the
    // outer is the only one that can speak for the region's orientation.
    vec2 q = abs(p) - uOuterHalf + uOuterRadius;
    float runs = gHasInner ? S(-gSpan, gSpan, q.x - q.y) : 1.0; ///< 1 where it runs vertically.
    gRuns = runs;

    float rain = clamp(uAmount, 0.0, 1.0);
    // Gravity is global: the trickling layers are active everywhere, not just
    // on vertical runs. On horizontal runs the drops just cross the band
    // vertically instead of streaking along its length, which is what falling
    // rain looks like passing through a narrow slit - RegionFade is what makes
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
    //
    // Gated on c.x, which makes this the single most expensive thing the
    // shader can skip: the two taps are two more FULL evaluations of the
    // droplet field, so an ungated fragment pays for the field three times
    // over. Most in-band fragments are the gaps BETWEEN drops, where c.x is
    // exactly 0 (`Drops` closes with S(0.3, 1.0, ...), which returns a hard
    // zero below the threshold) and the two taps are pure waste.
    //
    // Skipping there is EXACT, not an approximation. nrm reaches the output
    // through exactly two terms, and c.x = 0 annihilates both:
    //
    //   rim  = pow(clamp(c.x * (1 - c.x) * 4, 0, 1), 1.5)  -> 0, and the
    //          `facing` weight built from nrm only scales that zero.
    //   spec = pow(max(0, dot(nrm, lightDir)), 16) * c.x    -> 0.
    //
    // So with c.x = 0 the fragment's colour and alpha do not depend on nrm at
    // all, and vec2(0) is as good as the real gradient. It has to stay finite
    // rather than undefined for that: dot(vec2(0), lightDir) is 0, so `facing`
    // lands at 0.5 and nothing downstream sees a NaN to propagate.
    //
    // Note this gates on c.x alone, NOT on the trail channel c.y. A fragment
    // with c.x = 0 and c.y > 0 still emits light through `bright`, but that
    // path never reads nrm either, so it is correctly served by the skip.
    vec2 nrm = vec2(0.0);
    if (c.x > 0.0)
    {
        vec2 e = vec2(0.001, 0.0);
        float cx = Drops(uv + e.xy, t, staticDrops, layer1, layer2, uvToPx).x;
        float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2, uvToPx).x;
        vec2 normal = vec2(cx - c.x, cy - c.x);
        nrm = normal / max(length(normal), 1e-5);
    }

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
    //          Because RegionFade attenuates the MASK rather than the finished
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
