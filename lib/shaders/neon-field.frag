// ---------------------------------------------------------------------------
// The hue-invariant field's composite (pass 1c). The factorisation it rests
// on, the uniforms it shares with the edge ring's composite
// (neon-ring-field.frag) and the shading itself, fieldShade, are in
// neon-field-common.glsl, injected ahead of this file.
//
// "Nothing after the grade": pass 1b leaves the one-sided cut and the cutoffs
// to the blit (uBlitOwnsCut), so the shading never applies them after its tone
// map and the field is one channel.
//
// This draws pass 1b's glow quad into the reduced buffer - full size at 1.0 -
// unblended, in pass 1b's place, under a rotating hue or a moving intensity
// (IsFieldEligible): the only frames that redraw pass 1b with nothing but the
// time, or the gain, moved.
//
// The texel a fragment wants is floor((vPos - uFieldOrigin) * uFieldTexelScale),
// rect-local, and so independent of where the host's viewport starts. The
// field is a window of whole texels of the reduced buffer's grid (the buffer
// covers the whole viewport; the field the glow quad's box), vPos is in scaled
// px, and the scale is that buffer's texels per scaled px - just under 1,
// since its texel count is the viewport's truncated. Not gl_FragCoord: a select
// between the two cost the old full-resolution composite ~30% on an AMD Radeon
// Pro 5300M. The bake discards nothing (V35): the culls below are the only
// ones the field's pixels get, and a texel the quad did not reach holds 0,
// which composites to nothing.
// ---------------------------------------------------------------------------

uniform vec2      uFieldOrigin; ///< vPos of the field's texel (0, 0) corner.
uniform vec2      uFieldTexelScale; ///< Field texels per vPos unit: the reduced buffer's texels per scaled px.
uniform float     uResolutionScale; ///< The reduced buffer's scale: converts its px to destination px, as neon.frag does.

void main() {
    // Pass 1b's discards, term for term (neon.frag with uBlitOwnsCut 1): the
    // cull BLIT_SIDE_GUARD_PX past the one-sided cut and BLIT_CUTOFF_GUARD_PX
    // past each cutoff's ramp. Without them the shared buffer carried a line
    // of glow past the edge where pass 1b leaves 0 (I76). They are the ONLY
    // culls: this draws pass 1b's quad through pass 1b's projection, so its
    // vPos and its decisions are pass 1b's to the bit, which the bake's, on a
    // grid of its own, are not - a bake that culled too left a 0 on a texel
    // right at the boundary that this kept, 200 levels off (V35). The blit
    // never reads past them, so no frame pixel moves. The derivative sits
    // above every discard.
    {
        vec2  halfSize = uRectSize * 0.5;
        float d        = sdRoundBox(vPos, halfSize, uCornerRadius);
        float sideAA   = max(sdRoundBoxFwidth(vPos, halfSize, uCornerRadius) * uResolutionScale, 1e-6);
        if (uGlowSide == GLOW_SIDE_INSIDE  && d >  BLIT_SIDE_GUARD_PX) discard;
        if (uGlowSide == GLOW_SIDE_OUTSIDE && d < -BLIT_SIDE_GUARD_PX) discard;
        float inHalf  = 0.5 * max(uInsideCutoffSoftness,  sideAA);
        float outHalf = 0.5 * max(uOutsideCutoffSoftness, sideAA);
        float inMid   = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
        float outMid  = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);
        float dOut = (uGlowSide == GLOW_SIDE_INSIDE)
                         ? -CUTOFF_NEUTRALISED
                         : bandOuterDistance(vPos, d, halfSize, uCornerRadius, outMid);
        float dIn  = (uGlowSide == GLOW_SIDE_OUTSIDE) ? CUTOFF_NEUTRALISED : bandInnerDistance(d, inMid);
        if (dOut >  outHalf + BLIT_CUTOFF_GUARD_PX ) discard;
        if (dIn  < -(inHalf + BLIT_CUTOFF_GUARD_PX)) discard;
    }
    vec2 field = texelFetch(uField, ivec2(floor((vPos - uFieldOrigin) * uFieldTexelScale)), 0).rg;
    vec3 result = fieldShade(field);

    // Not dithered: this writes the reduced buffer, which the blit reads
    // filtered and dithers itself.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    fragColor = vec4(result, alpha);
}
