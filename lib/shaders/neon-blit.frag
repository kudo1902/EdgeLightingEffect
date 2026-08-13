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
    if (r > 1e-4) { return d - cut; }
    vec2 b = halfSize + vec2(cut);
    return sdRoundBox(p, b, 0.0);
}

void main() {
    vec2 uv = vPos * 0.5 + 0.5; // NDC [-1,1] (identity MVP) -> UV
    vec4 src = texture(uSource, uv); // premultiplied colour + coverage alpha

    vec2  local    = gl_FragCoord.xy - uRectCenter;
    vec2  halfSize = uRectSize * 0.5;
    float d        = sdRoundBox(local, halfSize, uCornerRadius);

    // Feather floor of HALF a pixel each way, i.e. a one-pixel-wide ramp. That
    // is what the full-res path's hard cut plus pixel sampling produces, so the
    // re-imposed edge is anti-aliased without being any softer than the base
    // renderer's. A full fwidth() here would span two pixels and leave the
    // light reaching one pixel further out than the base does.
    float aa      = 0.5 * fwidth(d);
    float inSoft  = max(uInsideCutoffSoftness,  aa);
    float outSoft = max(uOutsideCutoffSoftness, aa);
    float sideSoft = max(uGlowSideSoftness, aa);

    float dOut = bandOuterDistance(local, d, halfSize, uCornerRadius, uOutsideCutoff);
    float dIn  = d + uInsideCutoff;

    float mask = smoothstep(-inSoft, inSoft, dIn);
    mask      *= 1.0 - smoothstep(-outSoft, outSoft, dOut);
    if (uGlowSide == GLOW_SIDE_INSIDE)       { mask *= smoothstep( sideSoft, -sideSoft, d); }
    else if (uGlowSide == GLOW_SIDE_OUTSIDE) { mask *= smoothstep(-sideSoft,  sideSoft, d); }

    fragColor = src * mask; // premultiplied: scale colour and coverage together
}
