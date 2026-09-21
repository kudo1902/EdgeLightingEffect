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
    // keeps occluding. At BOTH this is a multiply by an exact 1.0, so that path
    // stays bit-identical to the plain texture read this shader used to be.
    fragColor = src * cut;
}
