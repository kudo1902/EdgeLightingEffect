precision highp float;

// Opaque background pass drawn behind the neon when
// config.neon.opaqueMode != NONE. Fills a rounded-rect band with
// `uOpaqueColor` (default black). Drawn as a fullscreen NDC quad so the
// fragment shader sees every pixel in the viewport; the silhouette comes
// from an analytic rounded-box SDF read off gl_FragCoord (NOT an
// interpolated varying). highp + gl_FragCoord keeps the SDF exact on
// mobile GLES (Mali/Tizen), where a mediump varying carrying pixel
// coordinates would lose precision and a vertex(highp)->fragment(mediump)
// varying mismatch can even fail to link.
//
// Bounds are positive pixel distances measured from the rect edge along
// their respective sides, resolved CPU-side by GetOpaqueFillParams
// (renderer/neon-cutoff.h) from NeonConfig::insideCutoff / outsideCutoff.
// An unbounded interior still arrives as the huge disabled-cutoff sentinel
// (the emission has no interior bound either, so the fill must not stop
// short of it); an unbounded exterior arrives as the draw-quad margin.
//
//   NONE    - discarded before this shader is dispatched (CPU-side guard).
//   OUTSIDE - fill 0 <= d <= outsideBound.
//   INSIDE  - fill -insideBound <= d <= 0.
//   BOTH    - fill -insideBound <= d <= outsideBound (the whole glow band).
//   ALL     - fill the whole viewport (coverage = 1 everywhere).
//
// Boundaries are SDF-anti-aliased over ~1-2 px via fwidth(d).

#define OPAQUE_MODE_NONE    0
#define OPAQUE_MODE_OUTSIDE 1
#define OPAQUE_MODE_INSIDE  2
#define OPAQUE_MODE_BOTH    3
#define OPAQUE_MODE_ALL     4

out vec4 fragColor;

uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform vec2  uRectCenter;     // rect centre in window pixels (gl_FragCoord space, y-up)
uniform int   uOpaqueMode;
uniform float uInsideBound;    // positive distance INSIDE the edge (d = -uInsideBound at boundary).
uniform float uOutsideBound;   // positive distance OUTSIDE the edge (d = +uOutsideBound at boundary).
uniform float uInsideSoftness; // feather width in px at the inside boundary.
uniform float uOutsideSoftness;// feather width in px at the outside boundary.
uniform vec4  uOpaqueColor;    // premultiplied fill: .rgb is the colour, .a scales coverage (1 = fully occluding, 0.5 = half-dim the background, 0 = no fill)

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    if (uOpaqueMode == OPAQUE_MODE_NONE) discard; // guard - the CPU already
                                                  // skips this pass, but be
                                                  // explicit for safety.

    vec2  localPos = gl_FragCoord.xy - uRectCenter;
    vec2  halfSize = uRectSize * 0.5;
    float d        = sdRoundBox(localPos, halfSize, uCornerRadius);

    // Feather widths.
    //   aa      = pixel-crisp AA at boundaries where no soft join is required
    //             (the geometric d=0 rect edge under OUTSIDE/INSIDE modes: the
    //             neon emission is at its brightest there and fully occludes
    //             the fill, so a wider feather would only bleed the fill
    //             sideways for no visual gain).
    //   inSoft / outSoft = the fill's own feather at each far boundary. These
    //             are per-side so the fill can taper with the emission on a
    //             side that has a Cutoff and much more gradually on a side
    //             that doesn't (see GetOpaqueFillParams). Floored to fwidth so
    //             the fade is never sharper than 1 px on curves.
    float aa      = fwidth(d);
    float inSoft  = max(uInsideSoftness, aa);
    float outSoft = max(uOutsideSoftness, aa);

    float coverage = 0.0;
    if (uOpaqueMode == OPAQUE_MODE_ALL) {
        coverage = 1.0;
    } else if (uOpaqueMode == OPAQUE_MODE_OUTSIDE) {
        // 0 <= d <= outsideBound
        float rise = smoothstep(-aa, aa, d);
        float fall = 1.0 - smoothstep(uOutsideBound - outSoft, uOutsideBound + outSoft, d);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_INSIDE) {
        // -insideBound <= d <= 0
        float rise = smoothstep(-uInsideBound - inSoft, -uInsideBound + inSoft, d);
        float fall = 1.0 - smoothstep(-aa, aa, d);
        coverage   = rise * fall;
    } else if (uOpaqueMode == OPAQUE_MODE_BOTH) {
        // -insideBound <= d <= +outsideBound (the full glow band)
        float rise = smoothstep(-uInsideBound - inSoft, -uInsideBound + inSoft, d);
        float fall = 1.0 - smoothstep(uOutsideBound - outSoft, uOutsideBound + outSoft, d);
        coverage   = rise * fall;
    }

    // uOpaqueColor.a scales how much of the background the fill takes out:
    // 1 occludes it completely, 0.5 halves it (the neon then still composites
    // over a partly visible background, so the halo keeps the extra brightness
    // it had with OPAQUE_MODE_NONE), 0 is a no-op.
    coverage *= clamp(uOpaqueColor.a, 0.0, 1.0);

    if (coverage <= 0.0) discard;

    // Coverage-weighted premultiplied output for
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA): uOpaqueColor.rgb where
    // coverage == 1, smooth AA at boundaries, off-side untouched.
    fragColor = vec4(uOpaqueColor.rgb * coverage, coverage);
}
