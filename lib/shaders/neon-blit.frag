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

// THREE RENDERERS COMPILE THIS SHADER, and only one of them wants the cut.
// NeonRenderer owns it; SpotlightRenderer and LensFlareRenderer reuse this
// pass as a plain premultiplied composite of their own reduced-resolution
// buffers and have no rect to cut against. Both therefore upload
// GLOW_SIDE_BOTH explicitly to take the branch below - see the note at each
// call site for why relying on GL's zero-initialised uniforms was not good
// enough. Anything added to this shader OUTSIDE that branch lands on all
// three layers.
//
// These three values are the ordinals of EdgeLighting::GlowSide, which
// neon-renderer.cpp casts straight to an int. The two numberings are one
// numbering, kept in step by hand: renumber the enum and these follow.
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

// Amplitude of the output dither in 8-bit destination steps, 0 = off.
//
// THE SECOND THING IN THIS SHADER THAT ONLY ONE OF ITS THREE RENDERERS WANTS,
// and it is a uniform rather than a #define for exactly that reason: this file
// takes no tuning header, and NeonRenderer uploads NEON_DITHER_STEPS here
// while SpotlightRenderer and LensFlareRenderer upload 0 explicitly - the
// spotlight because spotlight.frag has already dithered its own writes into
// the buffer this pass reads, and the flare because nobody has asked it to.
//
// Explicitly, and not left to GL's zero-initialised uniforms, on the same
// terms as uGlowSide above: relying on the zero means a future non-zero
// default silently dithers all three layers at once.
uniform float uDitherSteps;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Interleaved gradient noise in [-0.5, 0.5] - the same function, the same
// constants, as neon.frag's neonDither and spotlight.frag's spotDither. See
// NEON_DITHER_STEPS in neon-tuning.h for why the glow needs it at all.
//
// gl_FragCoord here, where the other two take a geometry-local coordinate:
// this pass is a fullscreen quad with no frame of its own, and vPos is NDC
// rather than pixels, so screen space is the only pixel-valued thing on hand.
// The consequence is that the pattern is anchored to the SCREEN - a moving
// rect slides under a stationary dither - which is the one property the other
// two avoid. It is acceptable here and nowhere else: this is the final
// composite of a layer that is already resolved, so the noise has nothing to
// correlate with.
float blitDither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
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
    vec4 outColor = src * cut;

    // THE SCALED PATH'S SECOND DITHER. neon.frag dithered the value that went
    // INTO the reduced buffer; this is the write the destination actually
    // rounds, and a rounding without a dither re-quantises the gradient into
    // plateaus however smooth the thing being sampled was. Both are needed and
    // neither is redundant - the measurements are in neon-tuning.h under
    // NEON_DITHER_STEPS, which also explains why the glow bands at all.
    //
    // Same offset on colour and coverage so the premultiplication holds, and
    // max() against 0 so a negative offset cannot separate them. The amplitude
    // is under half a step by construction, so a destination pixel this layer
    // left at zero stays at zero. See NEON_DITHER_STEPS in neon-tuning.h.
    //
    // A uniform branch, so the two layers that upload 0 pay nothing and keep
    // their pre-dither output bit for bit.
    if (uDitherSteps > 0.0)
    {
        float dither = blitDither(gl_FragCoord.xy) * (uDitherSteps / 255.0);
        outColor = max(outColor + dither, vec4(0.0));
    }

    fragColor = outColor;
}
