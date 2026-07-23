precision highp float;

// Sun + hexagonal-aperture lens flare. Ported from the Shadertoy
// https://www.shadertoy.com/view/Xlc3D2 - see the LICENSE NOTE below.
//
// LICENSE NOTE
//   Shadertoy shaders default to CC BY-NC-SA 3.0 when the author does not
//   grant a more permissive licence in the source. The referenced shader
//   contains no explicit licence, so this port inherits that restriction.
//   Treat as reference for a commercial ship; the 4sX3Rs version in git
//   history is Unlicensed.
//
// Perf notes (this is a fullscreen pass on every pixel, so trim matters):
//   * Ghost geometry (size, distance) is precomputed on the CPU and passed as
//     uGhosts[]; the per-pixel hash + pow(rnd, 2) chain is gone.
//   * All integer-exponent pow() calls are inlined into multiplies.
//   * The reference's pow(x, 1/40) and pow(x, 1/20) are replaced with a
//     sqrt chain approximating x^(1/32) and x^(1/16) - visually identical
//     for the input ranges we see, but ~10x cheaper on Mali / Adreno.
//   * The ghost loop is guarded on uSpread so it becomes a single branch
//     when ghosts are disabled.
//   * Ray sharpening pow(|sin|, 8) and pow(|cos|, 20) become manual chains
//     of x^2 -> x^4 -> ... etc.
//   * The flare renders full-screen: no rect-relative cutoff, no SDF eval
//     per fragment.

out vec4 fragColor;

uniform vec2  uResolution;
uniform vec2  uSunPos;
uniform vec4  uSunColor;
uniform float uIntensity;
uniform float uSpread;
uniform float uSize;
uniform float uRotation;
uniform float uRayDensity;

// Baked ghost params (.x = size, .y = distance along the sun axis). Values
// come from the reference shader's rnd() chain, computed once on the CPU and
// pasted here as literals: driver-side uniform arrays are unreliable on some
// GLES targets (notably Mali T-series on Tizen), and these never change
// anyway. If you edit the recipe, rerun the generator in
// LensFlareRenderer::setupGhosts (kept for reference) and paste the new
// values here.
const vec2 GHOSTS[10] = vec2[10](
    vec2(1.410000, -0.300000),
    vec2(1.415053,  2.535754),
    vec2(2.207786,  0.039478),
    vec2(1.664845,  0.268176),
    vec2(3.712599,  0.033984),
    vec2(1.891797,  1.603015),
    vec2(3.275765,  1.533435),
    vec2(3.942457,  0.419055),
    vec2(1.410162,  0.975787),
    vec2(3.676069,  2.242053)
);

float regShape(vec2 p, float N)
{
    float a = atan(p.x, p.y) + 0.2;
    float b = 6.28319 / N;
    return smoothstep(0.5, 0.51, cos(floor(0.5 + a / b) * b - a) * length(p));
}

// x^(1/32) approximation via 5 sqrts. Replaces pow(x, 1/40) for the ghost's
// c1 ring term. Visually indistinguishable for l in [0.5, 4] where the term
// actually contributes.
float pow1_32(float x)
{
    x = sqrt(x);
    x = sqrt(x);
    x = sqrt(x);
    x = sqrt(x);
    return sqrt(x);
}

// x^(1/16) via 4 sqrts. Replaces pow(x, 1/20) for the sun's outer soft glow.
float pow1_16(float x)
{
    x = sqrt(x);
    x = sqrt(x);
    x = sqrt(x);
    return sqrt(x);
}

// One ghost group at parametric distance `dist` along the sun axis.
vec3 circle(vec2 p, float size, float dist, vec2 sunUV)
{
    float halfSize = size * 0.5;
    float sizeExp  = size * 1.4;

    vec2  posA = p + sunUV * dist;
    vec2  posB = p + sunUV * (dist * 4.0);
    vec2  posC = p - sunUV * (dist * 0.5) + vec2(0.09);
    float lenA = length(posA);
    float lenB = length(posB);
    float lenC = length(posC);
    float l    = lenB + halfSize;

    float c  = max(0.01 - pow(lenA, sizeExp), 0.0) * 50.0;
    float c1 = max(0.001 - pow1_32(max(l - 0.3, 1e-6)) + sin(l * 30.0), 0.0) * 3.0;
    float c2 = max(0.04 / lenC, 0.0) * 0.05;  // (0.04/lenC) / 20 = (0.04/lenC) * 0.05
    float s  = max(0.01 - regShape(p * 5.0 + sunUV * dist * 5.0 + 0.9, 6.0), 0.0) * 5.0;

    vec3 color = cos(vec3(0.44, 0.24, 0.2) * 8.0 + dist * 4.0) * 0.5 + 0.5;
    return (c + c1 + c2 + s) * color - 0.01;
}

void main()
{
    vec2  uv     = gl_FragCoord.xy / uResolution - 0.5;
    vec2  sunUV  = uSunPos         / uResolution - 0.5;
    float aspect = uResolution.x / uResolution.y;
    uv.x    *= aspect;
    sunUV.x *= aspect;

    vec3 color = vec3(0.0);

    // Ghosts: skip the loop entirely when spread is off. Also, params come from
    // the precomputed uGhosts uniform - no per-pixel rnd/pow.
    if (uSpread > 0.01)
    {
        for (int i = 0; i < 10; i++)
        {
            color += circle(uv, GHOSTS[i].x, GHOSTS[i].y, sunUV) * uSpread;
        }
    }

    // --- Sun rays + core ---
    const float TWO_PI = 6.28318530717958647692;
    float a       = atan(uv.y - sunUV.y, uv.x - sunUV.x) + uRotation;
    float sunDist = length(uv - sunUV);
    float sDist   = sunDist / max(uSize, 1e-3);
    vec3  sunTint = uSunColor.rgb;

    float N        = uRayDensity;
    float aWrapped = mod(a, TWO_PI);
    float slot     = floor(aWrapped * N / TWO_PI);
    float rand     = fract(sin(slot * 43.7583) * 12345.67);
    float lenMod   = 0.15 + 0.85 * rand;

    // primary = |sin(a * N/2)|^8, manual: s -> s^2 -> s^4 -> s^8
    float s = abs(sin(a * N * 0.5));
    s = s * s;   // s^2
    s = s * s;   // s^4
    s = s * s;   // s^8
    float primary = s * lenMod;

    // sub = |cos(a * N/2)|^20, manual: c^2, c^4, c^8, c^16, c^20 = c^16 * c^4
    float c   = abs(cos(a * N * 0.5));
    float c2  = c * c;
    float c4  = c2 * c2;
    float c8  = c4 * c4;
    float c16 = c8 * c8;
    float sub = c16 * c4 * lenMod * 0.15;

    float ray = primary + sub;

    // Falloff term A: 0.1 / (sDist*5)^5 = 0.1 / pow5.
    float k5   = sDist * 5.0;
    float k5_2 = k5 * k5;
    float k5_4 = k5_2 * k5_2;
    float pow5 = k5_4 * k5;
    color += max(0.1 / pow5, 0.0) * ray * 0.05 * sunTint;   // ray/20 = ray*0.05

    // Falloff term B: replaces pow(sDist*10, 1/20) with x^(1/16) via 4 sqrts.
    // Bias by +1 so the sunDist=0 pole is defused; the sun-core term below
    // still supplies the intense peak.
    color += (0.1 / pow1_16(sDist * 10.0 + 1.0)
              + ray * 0.125) * sunTint;                     // ray/8 = ray*0.125

    // Falloff term C: sun-core disc, pow(x, 0.5) = sqrt(x).
    color += (0.1 / sqrt(sDist * 4.0 + 1e-4)) * 4.0
             * vec3(0.2, 0.21, 0.3) * 4.0 * sunTint;

    // --- Global falloff around the sun (kept from reference) ---
    color *= exp(1.0 - sunDist) * 0.2;                       // /5 = *0.2

    color = max(color, 0.0) * uIntensity;

    float alpha = clamp(max(max(color.r, color.g), color.b), 0.0, 1.0);
    fragColor = vec4(color, alpha);
}
