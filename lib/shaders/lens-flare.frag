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
//                   distance / size along the sun axis.
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
uniform float uGhostOffset;   // Signed shift of every ghost's distance along the sun axis. 0 = reference (blooms at centre); negative pulls the cluster toward the sun/border.
uniform vec3  uGhostColor;    // Tint the ghosts lean toward when uGhostTint > 0.
uniform float uGhostTint;     // Blend from the procedural rainbow (0) to a single uGhostColor (1).
uniform float uGhostSpin;           // Accumulated ghost spin angle (radians). Host holds it at 0 while not following and grows it from 0 when enabled, so rotation starts at the offset with no jump.
uniform float uGhostRotationOffset; // Static starting angle (radians) measured from the screen X axis; makes the trail direction screen-absolute.

float rnd(vec2 p) { return fract(sin(dot(p, vec2(12.1234, 72.8392)) * 45123.2)); }
float rnd(float w) { return fract(sin(w) * 1000.0); }

// N-gon aperture shape (N=6 gives hex ghosts). Returns 0 inside, 1 outside
// via smoothstep at the polygon edge.
float regShape(vec2 p, float N)
{
    float a = atan(p.x, p.y) + 0.2;
    float b = 6.28319 / N;
    return smoothstep(0.5, 0.51, cos(floor(0.5 + a / b) * b - a) * length(p));
}

// One ghost group at parametric distance `dist` along the sun axis.
vec3 circle(vec2 p, float size, float dist, vec2 sunUV)
{
    float l  = length(p + sunUV * (dist * 4.0)) + size / 2.0;
    float c  = max(0.01 - pow(length(p + sunUV * dist), size * 1.4), 0.0) * 50.0;
    float c1 = max(0.001 - pow(l - 0.3, 1.0 / 40.0) + sin(l * 30.0), 0.0) * 3.0;
    float s  = max(0.01 - pow(regShape(p * 5.0 + sunUV * dist * 5.0 + 0.9, 6.0), 1.0), 0.0) * 5.0;

    // Procedural per-ghost palette; distance-modulated so no two ghosts read
    // the same colour. This overrode the reference's caller-supplied colour
    // params, so those params are dropped in this port.
    vec3 color = cos(vec3(0.44, 0.24, 0.2) * 8.0 + dist * 4.0) * 0.5 + 0.5;
    // Lean the per-ghost hue toward a caller tint. uGhostTint 0 keeps the
    // procedural rainbow; 1 makes every ghost the same uGhostColor. Brightness
    // still comes from the (c + c1 + s) shape terms, so ghosts keep their
    // falloff either way.
    color = mix(color, uGhostColor, uGhostTint);
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

    // --- 10 ghost groups scattered along the sun -> centre axis.
    // uGhostSpacing stretches the placement axis so ghosts spread further
    // apart without touching their per-ghost colour/size (both keyed off
    // ghostDist, which is left unscaled).
    //
    // Spin the whole cluster around the sun by rotating the ghost SAMPLE point
    // about the sun. uGhostSpin is the host-accumulated spin angle (0 while not
    // following, growing from 0 once enabled) so the motion starts at the
    // offset with no jump.
    //
    // The trail direction is measured absolutely in screen space. As dist
    // varies, a ghost moves along -R(-grot)*sunUV, whose angle is
    // (sunToCentre - grot). Setting grot = sunToCentre - offset + spin makes
    // the resulting trail angle = offset - spin, i.e. offset degrees from the
    // screen X axis (0 = horizontal, PI/2 = vertical), independent of where the
    // sun sits, spinning in the rays' direction from that start.
    float sunToCentre = atan(-sunUV.y, -sunUV.x);
    float grot = sunToCentre - uGhostRotationOffset + uGhostSpin;
    float gcos = cos(grot);
    float gsin = sin(grot);
    vec2  gd   = uv - sunUV;
    vec2  guv  = sunUV + vec2(gcos * gd.x - gsin * gd.y,
                              gsin * gd.x + gcos * gd.y);
    vec2 ghostAxis = sunUV * uGhostSpacing;
    for (float i = 0.0; i < 10.0; i++)
    {
        // uGhostSize replaces the reference's per-ghost random size so every
        // ghost reads the same size; only distance (placement) still varies.
        // uGhostOffset slides the whole cluster along the axis: dist 0 is the
        // screen centre and dist ~ -1 is the sun, so a negative offset pulls
        // the ghosts off centre and up against the sun / border edge.
        float ghostDist = rnd(i * 20.0) * 3.0 + 0.2 - 0.5 + uGhostOffset;
        color += circle(guv, uGhostSize, ghostDist, ghostAxis) * uSpread;
    }

    // --- Sun rays + core; tint governed by uSunColor. Size scale divides the
    // distance so uSize > 1 grows the disc/rays and uSize < 1 shrinks them.
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

    color += max(0.1 / pow(sDist * 5.0,  5.0), 0.0) * ray / 20.0 * sunTint;
    color += (max(0.1 / pow(sDist * 10.0, 1.0 / 20.0), 0.0)
              + ray / 8.0) * sunTint;
    color += max(0.1 / pow(sDist * 4.0, 0.5), 0.0) * 4.0
             * vec3(0.2, 0.21, 0.3) * 4.0 * sunTint;

    // --- Global falloff around the sun (kept from reference).
    color *= exp(1.0 - sunDist) / 5.0;

    color = max(color, 0.0) * uIntensity;

    // Premultiplied "over": alpha driven by the brightest channel so bright
    // cores occlude and thin ghost wings blend additively.
    float alpha = clamp(max(max(color.r, color.g), color.b), 0.0, 1.0);
    fragColor = vec4(color, alpha);
}
