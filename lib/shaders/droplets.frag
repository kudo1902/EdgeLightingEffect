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
// shears or tears - but that means the band's orientation matters. A vertical
// stretch of band runs parallel to gravity, so drops trickle down it with long
// trails. A horizontal stretch runs perpendicular, where real rain does not
// streak - it beads and sits. So the layer *amplitudes* are modulated by how
// vertical the local edge is: trickles on the sides, condensation beads along
// the top and bottom, smoothly blended round the corners. This is amplitude
// only - never position - precisely so the grid stays unsheared.
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

/// Normalised depth into the band for a point at signed distance @p sd:
/// 0 at the inner boundary, 1 at the outer. Shared by the per-pixel band mask
/// in main() and the per-drop fit below, so both read the same band.
float BandAcross(float sd, float bandWidth) {
    float depth;
    if (uGlowSide == GLOW_SIDE_INSIDE)
    {
        depth = -sd - uBandOffset;
    }
    else if (uGlowSide == GLOW_SIDE_OUTSIDE)
    {
        depth = sd - uBandOffset;
    }
    else
    {
        // Straddle the edge: the band is centred on sd = 0.
        depth = sd + bandWidth * 0.5 - uBandOffset;
    }
    return depth / bandWidth;
}

/// |x| of the rounded box's outward normal at rect-local @p q: 1 where the
/// edge runs vertically (rain can streak down it), 0 where it runs
/// horizontally (rain can only bead).
///
/// Analytic rather than a finite difference of the SDF. At cornerRadius 0 the
/// difference straddles the corner's discontinuity and reports a direction
/// wrong by up to 45 degrees, which is exactly where drops sit at a sharp
/// corner.
float EdgeHorizontality(vec2 q) {
    vec2 d = abs(q) - uRectSize * 0.5 + uCornerRadius;
    if (d.x > 0.0 && d.y > 0.0)
    {
        // Corner region - the normal runs radially out of the corner's centre.
        return d.x / max(length(d), 1e-6);
    }
    // Along an edge the nearest feature is whichever axis is further out.
    return d.x > d.y ? 1.0 : 0.0;
}

/// 1 where the local edge is vertical, 0 where it is horizontal.
float EdgeRuns(vec2 q) {
    return S(0.25, 0.8, EdgeHorizontality(q));
}

/// How much of one drop the band can hold, from where the DROP'S OWN CENTRE
/// sits in it.
///
/// The band mask in main() is per pixel, so a drop straddling a boundary is
/// sliced into a half-moon - the shape that made the top and bottom edges read
/// as cut off. Reconstructing each drop's centre from its cell id and testing
/// that instead lets the whole drop fade in and out, so what reaches the
/// screen is always a complete drop. A drop is at full strength once its
/// centre clears its own radius from either boundary.
///
/// @param centreQ  Drop centre in rect-local pixels.
/// @param radiusPx Drop radius in framebuffer pixels.
float DropFit(vec2 centreQ, float radiusPx, float bandWidth) {
    float across = BandAcross(sdRoundBox(centreQ, uRectSize * 0.5, uCornerRadius), bandWidth);
    // Clamped below 0.5 so the two smoothsteps stay a well-formed window when
    // the drop is wider than the band (lanes = 1 on a thin band).
    float m = clamp(radiusPx / bandWidth, 0.0, 0.49);
    // Zero well before the centre reaches the boundary, not at it. The gap
    // between the two edges is where partly-overhanging drops live, and a
    // wider gap leaves more of them faintly visible as cups.
    return S(m * 0.6, m, across) * S(1.0 - m * 0.6, 1.0 - m, across);
}

// ---------------------------------------------------------------------------
// Droplet field (screen space, gravity-aligned)
// ---------------------------------------------------------------------------
//
// Cells are 6:1 tall so each drop has room for the trail it leaves behind.
// uv.y increases upward (gl_FragCoord convention), so advancing the pattern
// along +uv.y makes drops travel downward.

/// @param uvToPx Scale taking this layer's uv back to framebuffer pixels. It
///               differs per layer - the finer layer is handed @c uv * 1.85 -
///               and the per-drop fit needs the drop's screen position.
/// One column of the droplet grid, sampled at fragment x offset @p stx from
/// that column's centre.
///
/// Split out of DropLayer because a drop is wider than half a cell: with an
/// offset of up to 0.35 and a visible radius near 0.25 against a half-cell of
/// 0.5, a drop reaches past its own column. Evaluating only the column the
/// fragment falls in cuts every off-centre drop off square at the boundary,
/// so DropLayer evaluates the neighbouring column as well.
///
/// @param col    Column index to evaluate; not necessarily the fragment's own.
/// @param stx    Fragment x relative to that column's centre, in cell units.
/// @param baseY  Fragment y in this layer's uv, before the scroll and phase.
/// @param uvToPx Scale taking this layer's uv back to framebuffer pixels.
vec2 DropColumn(float col, float stx, float baseY, float t,
                vec2 a, vec2 grid, float uvToPx, float bandWidth) {
    float colShift = N(col); ///< Per-column phase so columns do not fall in lockstep.
    float uy = (baseY + t * 0.75 + colShift) * grid.y;
    float idy = floor(uy);
    float sty = fract(uy);

    vec3 n = N13(col * 35.2 + idy * 2376.1);
    vec2 st = vec2(stx, sty);

    float x0 = n.x - 0.5;
    float wy = baseY * 20.0;
    float wiggle = sin(wy + sin(wy));
    float x = (x0 + wiggle * (0.5 - abs(x0)) * (n.z - 0.5)) * 0.7;

    // Travel amplitude is 0.866, not 0.9, so the drop's y half-extent (0.4/6
    // of a cell) still clears the cell at both ends of the sweep. At 0.9 a
    // drop was born and died with a flat edge where the cell above or below
    // took over - the same square cut as in x, on the other axis.
    float ti = fract(t + n.z);
    float dropY = (Saw(0.85, ti) - 0.5) * 0.866 + 0.5;
    float d = length((st - vec2(x, dropY)) * a.yx);
    float mainDrop = S(0.4, 0.0, d);

    float r = sqrt(S(1.0, dropY, sty));
    float cd = abs(stx - x);
    float trail = S(0.23 * r, 0.15 * r * r, cd);
    float trailFront = S(-0.02, 0.02, sty - dropY);
    trail *= trailFront * r * r;

    // Beads shed along the trail.
    float trail2 = S(0.2 * r, 0.0, cd);
    float beadY = fract(baseY * 10.0) + (sty - 0.5);
    float dd = length(st - vec2(x, beadY));
    float droplets = S(0.3, 0.0, dd) * trail2;

    float m = mainDrop + droplets * r * trailFront;

    // --- Per-drop band fit -----------------------------------------------
    // Undo the scroll and the column phase to put the drop's centre back in
    // this layer's own uv, then in pixels. The wiggle is re-evaluated at the
    // DROP'S y rather than the fragment's, so the fit is one value for the
    // whole drop - the point of testing per drop instead of per pixel.
    float dropBaseY = (idy + dropY) / grid.y - (colShift + t * 0.75);
    float dwy = dropBaseY * 20.0;
    float dropWiggle = sin(dwy + sin(dwy));
    float dropX = (x0 + dropWiggle * (0.5 - abs(x0)) * (n.z - 0.5)) * 0.7;
    vec2 dropUV = vec2((col + dropX + 0.5) / grid.x, dropBaseY);
    vec2 dropQ = dropUV * uvToPx - uRectCenter; ///< Drop centre, rect-local px.
    // Drop radius is 0.4 of a cell and a cell is uvToPx / grid.x pixels wide.
    float fit = DropFit(dropQ, 0.4 * uvToPx / grid.x, bandWidth);

    // Orientation gating is per drop for the same reason the band fit is. At a
    // sharp corner the fade from vertical to horizontal sweeps through about
    // 38 degrees of angle, which 12 px out from the corner is some 8 px of arc
    // - narrower than a drop. Read per fragment it puts a steep gradient
    // across the drop and the S(0.3, 1.0) threshold turns that into a radial
    // cut: a crescent pinned to the corner. Read at the drop's centre it is
    // one weight for the whole drop, so the drop just fades.
    float trickle = EdgeRuns(dropQ);

    return vec2(m, trail) * (fit * trickle);
}

/// @param uvToPx Scale taking this layer's uv back to framebuffer pixels. It
///               differs per layer - the finer layer is handed @c uv * 1.85 -
///               and the per-drop fit needs the drop's screen position.
vec2 DropLayer(vec2 uv, float t, float uvToPx, float bandWidth) {
    vec2 a = vec2(6.0, 1.0);
    vec2 grid = a * 2.0;

    float gx = uv.x * grid.x;
    float col = floor(gx);
    float stx = gx - col - 0.5; ///< [-0.5, 0.5)

    // A fragment can only be reached by its own column's drop or by the one it
    // is nearest to - a drop in any further column is over a full cell away
    // and cannot span that. So two columns is exact here, not an approximation.
    float side = stx < 0.0 ? -1.0 : 1.0;

    vec2 own = DropColumn(col, stx, uv.y, t, a, grid, uvToPx, bandWidth);
    vec2 next = DropColumn(col + side, stx - side, uv.y, t, a, grid, uvToPx, bandWidth);

    // max, not sum: these are coverage masks, and where two drops do overlap
    // adding them would read as a bright seam along the column boundary.
    return max(own, next);
}


/// Static condensation beads - they fade in and out in place rather than
/// running, which is what rain actually does on a near-horizontal surface.
///
/// These beads are the ONLY thing that shows on horizontal runs of the band
/// (rain cannot streak down a horizontal edge), so the cell size is tuned to
/// give a comfortable per-band count rather than the sparse "condensation on
/// a windowpane" look the original tuning aimed for.
///
/// The cell is smaller than a droplet cell (1.3 rather than 0.85 of CELL_UV)
/// for two reasons that are really one: a bead about a quarter of the band
/// deep both fits inside it whole - so DropFit keeps most of them instead of
/// culling them - and leaves room for the band feather without the feather
/// reaching the bead and flattening it.
float StaticDrops(vec2 uv, float t, float uvToPx, float bandWidth) {
    float scale = CELL_UV * 1.3;
    uv *= scale;
    vec2 id = floor(uv);
    uv = fract(uv) - 0.5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    // The bead is only ever evaluated inside its OWN cell - fract() above wraps
    // the coordinate, and the neighbouring cells hash to different beads - so
    // anything that reaches past the cell boundary is cut off square. Keeping
    // offset + radius under half a cell is what keeps a bead round: at radius
    // 0.3 (which the S(0.3, 1.0) threshold in Drops() fattens further at high
    // condensation weights) the offset has to stay inside +/-0.19.
    vec2 p = (n.xy - 0.5) * 0.38;
    float d = length(uv - p);
    float fade = Saw(0.025, fract(t + n.z));

    // Beads sit still, so a bead that overhangs the band would be a permanent
    // half-moon pinned to the boundary - the most visible slice of all now
    // that condensation carries the horizontal runs. Fit it as a whole bead.
    vec2 beadQ = (id + 0.5 + p) * (uvToPx / scale) - uRectCenter;
    float fit = DropFit(beadQ, 0.3 * uvToPx / scale, bandWidth);

    // Per bead, not per fragment - see the note in DropColumn. The ratio here
    // is 14:1, so a bead straddling a corner's orientation fade was the most
    // brutally cut thing in the shader.
    float weight = mix(7.0, 0.5, EdgeRuns(beadQ));

    return S(0.3, 0.0, d) * fract(n.z * 10.0) * fade * fit * weight;
}

/// @param l0     Static condensation weight.
/// @param l1,l2  Trickling layer weights.
/// @param lTrail Weight for the streak channel. Kept separate from @p l0 -
///               they used to share one value, which meant boosting
///               condensation on the horizontal runs also amplified the
///               trails that are being suppressed there.
vec2 Drops(vec2 uv, float t, float l0, float l1, float l2, float lTrail,
           float uvToPx, float bandWidth) {
    float s = StaticDrops(uv, t, uvToPx, bandWidth) * l0;
    vec2 m1 = DropLayer(uv, t, uvToPx, bandWidth) * l1;
    vec2 m2 = DropLayer(uv * 1.85, t, uvToPx / 1.85, bandWidth) * l2;

    float c = s + m1.x + m2.x;
    c = S(0.3, 1.0, c);
    return vec2(c, max(m1.y * lTrail, m2.y * l1));
}

// ---------------------------------------------------------------------------

void main() {
    vec2 p = gl_FragCoord.xy - uRectCenter; ///< Rect-local pixels.
    vec2 halfSize = uRectSize * 0.5;

    float sd = sdRoundBox(p, halfSize, uCornerRadius);
    float bandWidth = max(uBandWidth, 1.0);

    float across = BandAcross(sd, bandWidth); ///< 0 at the inner boundary, 1 at the outer.

    // Early bail. A thin band is a small slice of the viewport, so rejecting
    // before any droplet work is where most of this pass's cost goes away.
    if (across < -0.25 || across > 1.25)
    {
        discard;
    }

    // --- Edge orientation -------------------------------------------------
    // Needed before the band mask because the feather width depends on it.
    // This is the only PER-FRAGMENT use of the orientation left: the feather
    // is a property of the boundary, so reading it at the fragment is right.
    // Everything that shades a drop reads it at the drop's centre instead.
    float runs = EdgeRuns(p); ///< 1 where the band is vertical.

    // Band boundary feather, expressed in the same normalised units.
    //
    // On a vertical run the feather is the configured softness: drops are
    // sized to fit across the band, so a crisp boundary is correct there.
    // A horizontal run cuts ACROSS the droplet grid's tall axis - one cell is
    // 6 * cellPx tall against a band only bandWidth deep - so a hard boundary
    // slices drops and trails. A little extra feather softens what is left
    // over after DropFit.
    //
    // It stays SMALL on purpose. The feather eats inward from both boundaries,
    // and a drop is a large fraction of the band's depth (lanes = 1 means
    // "drops as wide as the band"), so a wide feather dims the top and bottom
    // of even a perfectly centred drop - which is a circle rendered as a
    // flat-topped squircle. Keep this below the gap between a centred drop's
    // edge and the boundary: at the bead radius below that gap is ~0.27.
    float softPx = max(uGlowSideSoftness, 0.5) / bandWidth;
    float soft = clamp(mix(0.12, softPx, runs), 0.001, 0.5);
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
    float uvToPx = CELL_UV * cellPx; ///< uv -> pixels; the per-drop fit needs the inverse.
    vec2 uv = gl_FragCoord.xy / uvToPx;

    float t = uTime * 0.2 * uSpeed;

    // --- Layer weights ----------------------------------------------------
    // Gravity is global and the grid is screen space, so a trickling drop is
    // 6 * cellPx tall - taller than the band is deep on a horizontal run - and
    // 0.8 * cellPx across, which at lanes = 1 is 0.8 of the band's whole
    // depth. On a horizontal run it cannot fit however well DropFit culls it,
    // and whatever survives is a circle clipped flat top and bottom. There is
    // no amplitude at which it stops being a squircle, so it is gated to ZERO
    // there rather than merely down, and condensation carries those runs
    // instead - weighted up to pay back what the trickles stop contributing.
    //
    // Both of those weightings are applied at the DROP'S centre, inside
    // DropColumn and StaticDrops, not here: see the note in DropColumn. What
    // is left here is only the response to uAmount.
    float rain = clamp(uAmount, 0.0, 1.0);
    float wet = S(-0.5, 1.0, rain);
    float staticDrops = wet;
    float trailWeight = wet * 0.5;
    float layer1 = S(0.25, 0.75, rain);
    float layer2 = S(0.0, 0.5, rain);

    vec2 c = Drops(uv, t, staticDrops, layer1, layer2, trailWeight, uvToPx, bandWidth);

    // Trails run along the grid's tall axis and are the first thing the band
    // boundary chops. Taper them toward both boundaries so a trail thins out
    // before it reaches the edge of the band instead of ending on a hard line.
    // c.y feeds only the streak sheen, so this leaves the drop bodies alone.
    c.y *= S(1.0, 0.55, abs(across * 2.0 - 1.0));

    // Height-field gradient in cell space - independent of viewport size.
    vec2 e = vec2(0.001, 0.0);
    float cx = Drops(uv + e.xy, t, staticDrops, layer1, layer2, trailWeight, uvToPx, bandWidth).x;
    float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2, trailWeight, uvToPx, bandWidth).x;
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
    float alpha = bandMask * dropAlpha * uTint.a;
    if (alpha <= 0.0)
    {
        discard;
    }

    fragColor = vec4(premul * bandMask * uTint.a, alpha);
}
