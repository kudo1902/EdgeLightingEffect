precision highp float;

// Pass 2b of the scaled neon path: bilinear composite of the reduced-resolution
// neon FBO (premultiplied colour + coverage alpha) onto the backbuffer. The
// opaque-mode silhouette is handled entirely by the black-rect fullscreen pass
// drawn just before this blit in NeonRenderer::Render - the black quad's
// analytic SDF anti-aliasing lands cleanly on rounded corners regardless of
// softness, whereas the old per-fragment discard here stair-stepped at any
// corner radius > 0.
//
// THE ONE-SIDED CUT IS APPLIED HERE, not in neon.frag, whenever this pass runs
// at all. That is the whole reason this shader knows anything about geometry.
//
// neon.frag rasterises into a buffer at NeonConfig::resolutionScale and this
// pass bilinearly upsamples it, so a cut made in there is a cut made in the
// wrong units: each destination pixel is rebuilt from the 2x2 buffer texels
// around it, and a texel whose centre is on the lit side contributes to
// destination pixels 1/scale away in BOTH directions. Measured at scale 0.5,
// glowSide OUTSIDE, softness 0, with the cut made in neon.frag at d = 0: the
// destination pixel at d = -0.5 - on the DARK side of the line, over backdrop
// an OpaqueMode fill never covers - came back at 59/255. At scale 0.25 the
// same wash ran four destination pixels deep, 105 / 75 / 45 / 15 going inward
// from the line.
//
// No placement inside the buffer fixes that, because the buffer does not
// contain a destination-resolution edge to place. This pass does: it runs
// full-res on the caller's framebuffer, exactly like black-rect.frag, so
// fwidth(d) here is one DESTINATION pixel and the cut lands where the direct
// path puts it. Measured on the same scene after the move, first lit pixel
// against the direct path's 239: scale 0.5 gives 237, scale 0.25 gives 230,
// and the dark side is 0 at both.
//
// The emission it cuts is real rather than reconstructed-from-black because
// neon.frag culls BLIT_SIDE_GUARD_PX past the cut instead of at it, leaving a
// lit guard band for the filter to rebuild the boundary from. See
// neon-tuning.h. The hard step where that cull finally bites sits well inside
// the region this mask zeroes, so it is never visible.
//
// It also puts glowSideSoftness back on a destination-pixel footing, which is
// what makes the feather scale-invariant. Sized in buffer px it lost its whole
// bottom range: at scale 0.25 a softness of 0, 2 and 4 rendered BYTE-IDENTICAL,
// because 4 px is one buffer texel there and the filter is wider than that.
// The first lit pixel now reads 37 / 37 / 36 at softness 4 across scales
// 1.0 / 0.5 / 0.25, against 37 / 56 / 29 with the cut made in the gather and
// everything else held here. That sequence is the point: it tracks neither the
// scale nor the 4 px the caller asked for, it just wanders with where the
// boundary falls between buffer texels.

#define GLOW_SIDE_BOTH    0
#define GLOW_SIDE_INSIDE  1
#define GLOW_SIDE_OUTSIDE 2

in vec2 vPos;
out vec4 fragColor;

uniform sampler2D uSource;

// Geometry of the cut, all in FULL-RES px - this pass is never scaled. The
// centre is in gl_FragCoord space (y up), mirrored CPU-side out of Config's
// y-down convention, exactly as black-rect.frag takes it.
uniform vec2  uRectSize;
uniform float uCornerRadius;
uniform vec2  uRectCenter;
uniform int   uGlowSide;
uniform float uGlowSideSoftness; // NOT pre-multiplied by the resolution scale.

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 uv = vPos * 0.5 + 0.5;      // NDC [-1,1] (identity MVP) -> UV
    vec4 src = texture(uSource, uv); // premultiplied colour + coverage alpha

    // GlowSide::BOTH pays nothing, and the branch that arranges that is safe
    // BECAUSE uGlowSide IS A UNIFORM: every invocation in the draw takes the
    // same side, helper invocations included, which is the "uniform control
    // flow" a derivative is allowed to sit inside. The rule it must not break
    // is a derivative under control flow that varies BETWEEN fragments.
    //
    // Not the same shape as the early returns black-rect.frag's main() warns
    // about at length, and measured not to behave like them. Those sat ABOVE
    // the derivative and cost 35% on this driver whichever mode ran; this one
    // CONTAINS it, and skipping the block is a straight saving: scaled BOTH at
    // 1920x1320 runs 1.255 ms branched against 1.396 ms with the SDF hoisted
    // unconditionally, which is back to what the pass cost before it grew a cut
    // at all. One-sided is unmoved either way. Re-measure BOTH as well as a
    // one-sided scene if this block is ever restructured.
    float cut = 1.0;
    if (uGlowSide != GLOW_SIDE_BOTH)
    {
        float d    = sdRoundBox(gl_FragCoord.xy - uRectCenter, uRectSize * 0.5, uCornerRadius);
        float aa   = max(fwidth(d), 1e-6);
        float soft = max(uGlowSideSoftness, aa);
        float back = 0.5 * aa;

        // Same anchor, same floor, same curve as neon.frag's post-grade cut -
        // see the derivation there. Keep the two in step; they are one edge,
        // written twice because only one of them ever runs.
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            cut = 1.0 - smoothstep(back - soft, back, d);
        }
        else if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            cut = smoothstep(-back, soft - back, d);
        }
    }

    // Applied to the premultiplied sample, so colour and coverage scale
    // together and the layer thins out as a whole rather than dimming while it
    // keeps occluding. At BOTH this is a multiply by an exact 1.0, so the cut
    // costs that path nothing.
    vec4 outColor = src * cut;

    // --- Output dither ------------------------------------------------
    // The scaled path's SECOND quantisation. neon.frag dithers the first one
    // (its write into the reduced buffer, which is what preserves the sub-LSB
    // information this shader's bilinear fetch then averages back into real
    // intermediate values); this is the write that would otherwise re-band
    // those recovered values on the way to 8 bits.
    //
    // Both are needed, and neither alone is enough. Mean plateau in px on a
    // glowRadius 30 scene, against an undithered baseline of 3.27:
    //
    //   gather only   scale 0.5 -> 2.53   scale 0.25 -> 3.07
    //   blit only               -> 3.35              -> 3.27  (no change)
    //   both                    -> 1.91              -> 1.86
    //
    // Blit-only changing nothing is the informative row: by the time this
    // shader runs on an undithered buffer it is filtering exact multiples of
    // 1/255, and dithering those only randomises them by +-1. See the long
    // note at neon.frag's matching block, and OUTPUT_DITHER_LSB in
    // neon-tuning.h. Keep the expression identical to that one - the two are
    // the same noise field and both run on this path.
    //
    // Unconditional because this pass runs only on the scaled path. The
    // smoothstep gate is the same footprint guard neon.frag uses, applied to
    // the composited alpha so the cut cannot be undone by the noise.
    //
    // NOTE this means GlowSide::BOTH is no longer bit-identical to the plain
    // texture read this shader used to be. That is intended; set
    // OUTPUT_DITHER_LSB to 0.0 and it returns.
    if (OUTPUT_DITHER_LSB > 0.0)
    {
        float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        outColor += (ign - 0.5) * (OUTPUT_DITHER_LSB / 255.0) *
                    smoothstep(DITHER_FADE_LO, DITHER_FADE_HI, outColor.a);
    }

    fragColor = outColor;
}
