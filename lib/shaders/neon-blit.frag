precision highp float;

// Pass 2 of the optimized neon: bilinear composite of the scaled neon FBO
// (premultiplied colour + coverage alpha) onto the backbuffer.
//
// The band's hard boundaries are re-applied HERE, at full resolution, rather
// than being inherited from Pass 1. Pass 1 renders at `resolutionScale`, so a
// cutoff baked into it comes back through this bilinear upscale as a ramp one
// FBO texel wide - half inside the cutoff, half outside. The outer half is
// light drawn beyond the requested cutoff, and it crosses the full-res opaque
// fill's edge as a visible fringe. Pulling Pass 1's cutoff inward cannot fix
// that cleanly either: the cut can only land on a texel boundary, so it moves
// in whole-texel jumps (2 px at scale 0.5) and overshoots or undershoots.
//
// Masking here instead gives an edge exactly at the requested cutoff, as sharp
// as the full-res renderer's, and costs no glow. Pass 1 deliberately cuts one
// texel WIDER so the blurred region extends past this boundary and the mask
// has real emission to cut into.
//
// The opaque-mode silhouette is still handled by the black-rect fullscreen
// pass drawn just before this blit.

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

// All in FULL-RES pixels - this pass runs on the backbuffer.
uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform vec2  uRectCenter;            // rect centre in gl_FragCoord space (y-up)
uniform float uInsideCutoff;          // positive distance inside the edge; huge sentinel when disabled
uniform float uInsideCutoffSoftness;
uniform float uOutsideCutoff;         // positive distance outside the edge; huge sentinel when disabled
uniform float uOutsideCutoffSoftness;
uniform int   uGlowSide;
uniform float uGlowSideSoftness;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Same band geometry as neon.frag / black-rect.frag: Euclidean parallel curve
// for a rounded rect, per-axis offset for a sharp one so cornerRadius 0 keeps
// square outer corners.
float bandOuterDistance(vec2 p, float d, vec2 halfSize, float r, float cut) {
    if (r > BAND_SHARP_CORNER_EPSILON) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    vec4 src = texture(uSource, uv); // premultiplied colour + coverage alpha

    vec2  local    = gl_FragCoord.xy - uRectCenter;
    vec2  halfSize = uRectSize * 0.5;
    float d        = sdRoundBox(local, halfSize, uCornerRadius);

    // HARD clamp at the band's mathematical end, nothing more.
    //
    // Pass 1 already applied the soft cutoff ramps before its tone map, which
    // is where they have to be to match the full-res renderer. What Pass 1
    // cannot do is end them crisply: at resolutionScale < 1 the upscale smears
    // its final edge across a texel, so light lands beyond the band and
    // fringes past the opaque fill.
    //
    // So this only zeroes what lies outside cutoff + softness - the point
    // where the emission is mathematically already nil. Being a 0-or-1 factor
    // it commutes with the tone map, so it cannot alter any pixel the base
    // renderer would have drawn; it only removes upscale spill.
    //
    // Half-pixel feather so the clamp is anti-aliased rather than stair-stepped.
    float aa = 0.5 * fwidth(d);

    float dOut = bandOuterDistance(local, d, halfSize, uCornerRadius,
                                   uOutsideCutoff + max(uOutsideCutoffSoftness, 0.0));
    float dIn  = d + uInsideCutoff + max(uInsideCutoffSoftness, 0.0);

    float mask = smoothstep(-aa, aa, dIn);
    mask      *= 1.0 - smoothstep(-aa, aa, dOut);
    // One-sided cut, owned entirely by this pass (Pass 1 leaves the emission
    // continuous across d == 0).
    //
    // Floored to the SAME epsilon neon.frag uses - literally the shared
    // SIDE_SOFT_EPSILON from neon-tuning.h, not a copy of its value - and NOT
    // to a pixel. The full-res renderer makes this a hard step and lets the
    // smooth emission either side supply the apparent anti-aliasing; flooring
    // to half a pixel here instead widens the edge relative to the base and
    // leaves a 1 px line of difference all the way round the perimeter.
    float sideSoft = max(uGlowSideSoftness, SIDE_SOFT_EPSILON);
    if (uGlowSide == GLOW_SIDE_INSIDE)       { mask *= smoothstep( sideSoft, -sideSoft, d); }
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) { mask *= smoothstep(-sideSoft,  sideSoft, d); }

    fragColor = src * mask; // premultiplied: scale colour and coverage together
}
