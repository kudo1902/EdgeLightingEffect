precision highp float;

// Sun + hexagonal-aperture lens flare. Ported from the Shadertoy
// https://www.shadertoy.com/view/Xlc3D2 - see the LICENSE NOTE below.
//
// LICENSE NOTE
//   Shadertoy shaders default to CC BY-NC-SA 3.0 when the author does not
//   grant a more permissive licence in the source. The referenced shader
//   contains no explicit licence, so this port inherits that restriction.
//   If this library needs to ship in a commercial product, treat this file as
//   a reference implementation and rewrite the elements you keep, OR obtain a
//   permissive re-license from the original author. The prior version in git
//   history (based on Shadertoy 4sX3Rs, explicit Unlicense) is safe to ship.
//
// Structure (matches the reference so the visual reads the same):
//   10x ghosts    - `circle()` called in a loop. Each iteration is a group of
//                   {big pow-based ghost, ring, hex sprite} at a pseudo-random
//                   distance / size along the sun axis. The distance and the
//                   group's colour are read from uGhosts, which the renderer
//                   bakes; the reference derived them here, per fragment.
//                   Hex shape comes from regShape(N=6).
//   sun rays+core - three layered terms on top of the ghosts, tinted by
//                   uSunColor so the sun's temperature is controllable.
//   post          - global exp() vignette away from the sun.
//
// Cleanups vs the reference: dead sun() function removed, dead per-iteration
// `circColor` params dropped (the function overrides them with a procedural
// palette anyway), noise-texture reads replaced with hash so no atlas needed,
// sky-gradient background removed (this renderer's single responsibility is
// the flare itself - use a dedicated background renderer for atmosphere).
//
// Three departures from the reference are performance, not looks. The
// reference's hex distance went through atan(); it is a max of three dot
// products instead. Its two compactly-supported ghost terms were evaluated at
// every fragment; they are now gated on the exact radius outside which each is
// zero. Both are value-preserving - see hexCoverage and circle below. And its
// per-ghost distance and palette, pure functions of the ghost index and the
// config, were re-derived in every fragment; they now arrive baked in uGhosts.
// That last one shifts ghost placement in the third decimal, for the reason
// the renderer's BakeGhostTable spells out. The ghost loop was 88% of this
// shader's cost, and hexCoverage alone 43%.

out vec4 fragColor;

uniform vec2  uResolution;   // Viewport in pixels; used for aspect + normalisation.
uniform vec2  uSunPos;       // Sun centre in gl_FragCoord pixels (y-up).
uniform vec4  uSunColor;     // Sun-core / rays tint. Ghosts stay procedural.
uniform float uIntensity;    // Master brightness multiplier.
uniform float uSpread;       // Ghost strength (0 = suppress ghosts, 1 = reference look).
uniform float uSize;         // Sun-core / rays size scale (1 = reference); ghosts unaffected.
uniform float uRotation;     // Ray-angle offset in radians; drives sun/ray spin.
uniform float uRayDensity;   // Angular density of the ray pattern (see the doc block on the ray recipe).
uniform float uGhostSpacing;  // Ghost placement stretch along the sun axis (1 = reference). Scales spread only, not colour/size.
uniform float uGhostSize;     // Uniform per-ghost size/falloff exponent, shared by every ghost so they all read the same size.
uniform vec2  uFlareCenter;   // Ghost convergence point in normalised screen coords (0..1, y-down). (0.5, 0.5) = screen centre.
uniform float uBloomRadius;   // Radius outside which a ghost's bloom term is exactly zero. Derived from uGhostSize on the CPU; see lens-flare-tuning.h.
uniform float uRingFloor;     // Lower bound sin(l * 30) must clear before a ghost's ring term can be non-zero. Same derivation.

// Per-ghost table as a std140 uniform block, baked by the renderer: xyz is a
// ghost's final colour, w its distance along the sun axis.
//
// Both were derived here, per fragment, from the ghost index and three
// uniforms - LensFlareConfig's ghostOffset, ghostColor and ghostTint, which is
// why those no longer appear above. They were the same ten values in every
// fragment of the viewport, so they are the renderer's work, not the shader's.
// Same split the neon emission pre-pass is built on, one tier cheaper: one
// 160-byte block, no texture and no pass.
//
// A BLOCK, not a bare `uniform vec4 uGhosts[N]`: every per-index array in this
// tree is packed into a std140 block, because the bare array form is not
// available on the restricted GL targets this library ships against. Same
// pattern as neon.frag's LoopSamplesBlock / SegmentBlock / ArcBlock.
layout(std140) uniform GhostBlock
{
    vec4 uGhosts[FLARE_GHOST_COUNT];
};

// Hex aperture sprite. Returns anti-aliased *inside* coverage: 1 within the
// hexagon, 0 outside, with the edge feathered over the pixel footprint of the
// polygon distance.
//
// The reference sliced the sprite out with `max(0.01 - regShape, 0)`, where
// regShape was a 0.5..0.51 smoothstep. That threshold lands at the flat bottom
// of the smoothstep, so the *visible* fill boundary was razor-thin (sub-pixel)
// no matter how wide the smoothstep band was - it aliased into stair-steps,
// and worse in the half-res optimized pass where a pixel spans ~2x the width.
// Building coverage straight from the polygon distance with an fwidth-sized
// edge keeps the boundary ~1px wide at whatever resolution the pass runs at,
// so the hex ghosts resolve cleanly at full and half res alike.
//
// WHY THERE IS NO atan HERE
//   The distance being measured is the hexagon's support function: p projected
//   onto whichever of the six edge normals it lies nearest. The reference found
//   that normal by angle - atan(p.x, p.y), snapped to the nearest multiple of
//   2 PI / N, then cos(snapped - a) * length(p). But for a convex regular
//   polygon the nearest normal is also the one with the LARGEST projection:
//   every other normal sits further round, so its cosine is smaller. That makes
//   the whole construction a plain max over the six dot products, with no
//   transcendental in it. Opposite normals differ only in sign, so three
//   abs()-ed dots cover all six.
//
//   Same value to within float noise (max deviation 5e-6 over the plane, which
//   is the reference's own 6.28319 truncation), at roughly a third of the cost.
//   atan alone was ~43% of this shader.
//
// HEX_AXIS_* are dir(k * 2 PI / 6 - 0.2) for k = 0, 1, 2, in the reference's
// (sin, cos) convention and carrying its +0.2 radian aperture roll.
const vec2 HEX_AXIS_0 = vec2(-0.198669331,  0.980066578);
const vec2 HEX_AXIS_1 = vec2( 0.749428406,  0.662085390);
const vec2 HEX_AXIS_2 = vec2( 0.948096722, -0.317982085);

float hexCoverage(vec2 p)
{
    float d = max(max(abs(dot(p, HEX_AXIS_0)), abs(dot(p, HEX_AXIS_1))),
                  abs(dot(p, HEX_AXIS_2)));
    float w = max(fwidth(d), 1e-4);
    return 1.0 - smoothstep(0.5 - w, 0.5 + w, d);
}

// One ghost group at parametric distance `dist` along the sun axis.
//
// The bloom and ring terms are compactly supported - each is exactly zero
// outside a radius that depends only on `size` - so each is gated on the bound
// the renderer solved for it. A ghost covers a few percent of the viewport but
// used to be evaluated in full at every fragment of it. lens-flare-tuning.h
// carries the derivation of both bounds and the constants they share with the
// terms below.
//
// The hex sprite is deliberately NOT gated the same way: hexCoverage calls
// fwidth, and a derivative taken in non-uniform control flow is undefined in
// GLSL. It stays at the top level of the function, where the whole quad
// evaluates it or none of it does.
//
// `color` arrives from uGhosts rather than being derived here; `dist` still
// shapes all three terms, so it is still a parameter.
vec3 circle(vec2 p, float size, float dist, vec2 sunUV, vec3 color)
{
    // Bloom. `pow` is strictly increasing, so lq < uBloomRadius is exactly the
    // condition for a non-zero term, not an approximation of it.
    float lq = length(p + sunUV * dist);
    float c  = 0.0;
    if (lq < uBloomRadius)
    {
        c = max(FLARE_BLOOM_CUT - pow(lq, size * FLARE_BLOOM_EXP), 0.0) * 50.0;
    }

    // Ring. The sin is the cheap half of the comparison and uRingFloor bounds
    // the expensive half from below, so test the sin first and only pay for the
    // pow on the thin annuli where the term can actually be non-zero.
    float l  = length(p + sunUV * (dist * 4.0)) + size * FLARE_RING_L_BIAS;
    float sl = sin(l * 30.0);
    float c1 = 0.0;
    if (sl > uRingFloor)
    {
        c1 = max(FLARE_RING_BIAS - pow(l - FLARE_RING_SHIFT, FLARE_RING_EXP) + sl, 0.0) * 3.0;
    }

    // Flat-filled hex sprite (magnitude 0.05, matching the reference's
    // 0.01 * 5) with an anti-aliased edge from hexCoverage.
    float s  = hexCoverage(p * 5.0 + sunUV * dist * 5.0 + 0.9) * 0.05;

    // Brightness comes from the (c + c1 + s) shape terms and hue from the
    // baked table, so ghosts keep their falloff whatever the tint.
    return (c + c1 + s) * color - 0.01;
}

void main()
{
    vec2  uv     = gl_FragCoord.xy / uResolution - 0.5;
    vec2  sunUV  = uSunPos         / uResolution - 0.5;
    float aspect = uResolution.x / uResolution.y;
    uv.x    *= aspect;
    sunUV.x *= aspect;

    vec3 color = vec3(0.0);

    // Ghost convergence point ("optical centre"). Converts the normalised
    // screen coord (0..1, y-down) into this shader's aspect-corrected, y-up,
    // centre-origin space. (0.5, 0.5) maps to (0, 0) = the screen centre, which
    // is the reference the ghosts historically pivoted about.
    vec2 flareCentre = vec2((uFlareCenter.x - 0.5) * aspect,
                            0.5 - uFlareCenter.y);

    // --- Ghost groups scattered along the sun -> flare-centre axis.
    // Working relative to flareCentre re-anchors the whole cluster: ghostP puts
    // dist 0 at flareCentre, and ghostAxis is the flareCentre -> sun direction,
    // so the ghost line runs through flareCentre and the sun. uGhostSpacing
    // stretches the placement axis without touching per-ghost colour/size.
    //
    // uGhostSize replaces the reference's per-ghost random size so every ghost
    // reads the same size; only distance (placement) still varies, and that
    // distance now arrives baked in uGhosts[i].w. dist 0 is the flare centre
    // and dist ~ -1 is the sun, which is the axis LensFlareConfig::ghostOffset
    // slides the cluster along before the bake.
    //
    // Every term the loop produces is multiplied by uSpread, so at zero the
    // whole block contributes exactly nothing and is skipped outright - and
    // zero is a documented setting (LensFlareConfig::spread, "suppress
    // ghosts"), not a degenerate one. uSpread is a uniform, so this is uniform
    // control flow: every invocation in the draw takes the same side, which is
    // what keeps circle()'s fwidth call legal inside it, and what makes the
    // branch itself free.
    if (uSpread != 0.0)
    {
        vec2 ghostP    = uv    - flareCentre;
        vec2 ghostAxis = (sunUV - flareCentre) * uGhostSpacing;
        for (int i = 0; i < FLARE_GHOST_COUNT; i++)
        {
            color += circle(ghostP, uGhostSize, uGhosts[i].w, ghostAxis, uGhosts[i].xyz) * uSpread;
        }
    }

    // --- Sun rays + core; tint governed by uSunColor. Size scale divides the
    // distance so uSize > 1 grows the disc and uSize < 1 shrinks it. Ray extent
    // is NOT scaled by uSize: the `ray / 8.0` term below has no distance
    // falloff, so how far the rays reach is set by the global exp(1 - sunDist)
    // envelope, which is in unscaled units.
    //
    // Ray recipe for a "sunburst" look:
    //   primary  = pow(|sin(a * N/2)|, 8)  -> N very thin countable spikes.
    //   sub      = pow(|cos(a * N/2)|, 20) -> even thinner "sparkle" needles
    //              at the half-angles between primaries. Fainter (0.15 weight).
    //   lenMod   = per-ray hash-based length scale in [0.15, 1.0]. Slots are
    //              indexed off @c mod(a, 2 PI) so the hash is stable across
    //              rotation, and slot boundaries land at primary valleys
    //              (primary = 0) so hash discontinuities are invisible.
    //              Applied to both primary and sub, so short rays are short
    //              in ALL their layers - avoids "flower" uniformity.
    const float TWO_PI = 6.28318530717958647692;
    float a       = atan(uv.y - sunUV.y, uv.x - sunUV.x) + uRotation;
    float sunDist = length(uv - sunUV);
    float sDist   = sunDist / max(uSize, 1e-3);
    vec3  sunTint = uSunColor.rgb;

    float N        = float(uRayDensity);
    float aWrapped = mod(a, TWO_PI);
    float slot     = floor(aWrapped * N / TWO_PI);
    float rand     = fract(sin(slot * 43.7583) * 12345.67);
    float lenMod   = 0.15 + 0.85 * rand;
    float primary  = pow(abs(sin(a * N * 0.5)), 8.0)  * lenMod;
    float sub      = pow(abs(cos(a * N * 0.5)), 20.0) * lenMod * 0.15;
    float ray      = primary + sub;

    // Softening floors give each falloff an intrinsic scale, so its peak is
    // finite and size-invariant. Without them the terms are pure power laws -
    // scale free, so dividing the radius by uSize is algebraically identical to
    // multiplying the term by uSize^k, and `size` acted as a second, non-linear
    // brightness knob (uSize^5 on the core term). The floor only bites near
    // sDist = 0, so far-field values - and the reference look - are unchanged.
    // It also removes the divide-by-zero inf the reference had at the exact sun
    // centre, which is why the max(..., 0.0) guards are gone.
    const float CORE_SOFT = 0.35; // core term, k = 5
    const float HAZE_SOFT = 0.02; // haze term, k = 1/20
    const float GLOW_SOFT = 0.10; // glow term, k = 1/2

    float core = 0.1 / (pow(sDist * 5.0,  5.0)        + CORE_SOFT);
    float haze = 0.1 / (pow(sDist * 10.0, 1.0 / 20.0) + HAZE_SOFT);
    float glow = 0.1 / (pow(sDist * 4.0,  0.5)        + GLOW_SOFT);

    color += core * ray / 20.0 * sunTint;
    color += (haze + ray / 8.0) * sunTint;
    color += glow * 4.0 * vec3(0.2, 0.21, 0.3) * 4.0 * sunTint;

    // --- Global falloff around the sun (kept from reference).
    color *= exp(1.0 - sunDist) / 5.0;

    color = max(color, 0.0) * uIntensity;

    // Premultiplied "over": alpha driven by the brightest channel so bright
    // cores occlude and thin ghost wings blend additively.
    float alpha = clamp(max(max(color.r, color.g), color.b), 0.0, 1.0);
    fragColor = vec4(color, alpha);
}
