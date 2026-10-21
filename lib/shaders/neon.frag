// ---------------------------------------------------------------------------
// The neon's shading program: pass 1b (at resolutionScale, into the reduced
// buffer) and the edge ring (at full resolution, onto the target), two
// program objects from this one source (NeonRenderer::ensureGlowPrograms).
// The shading itself - every uniform and every term up to the grade - is
// neonShade, in neon-shade.glsl, injected ahead of this file. What is here
// is what follows the grade: the tone map, the one-sided cut and the
// cutoffs as coverage, the alpha and the dither - none of which the field
// bake (neon-field-bake.frag) runs.
// ---------------------------------------------------------------------------

void main() {
    NeonEdge edge;
    vec3 result = neonShade(false, edge);
    bool blitOwnsCut = (uBlitOwnsCut != 0);

    // --- Grade --------------------------------------------------------
    // neonToneMap (neon-grade.glsl, shared with the field composites'
    // fieldShade in neon-field-common.glsl).
    result = neonToneMap(result);

    // --- One-sided cut: mask the WHOLE layer at the line --------------
    // Anchored at the opaque fill's own edge and feathered INTO the lit side
    // (sideAA / sideSoft / sideBack at the top of neonShade).
    //
    // BELOW THE GRADE: the cut is COVERAGE, so it scales what reaches the
    // framebuffer. Above the tone map it was nearly annihilated - a
    // half-covered filament pixel came out at 94% - and the alpha (the peak
    // channel) occluded at 0.94 instead of 0.5. Below it, the cut is the 1 px
    // box filter and tracks the fill it has to register with.
    //
    // Written as `1.0 - smoothstep(lo, hi, d)` for the INSIDE arm rather than
    // smoothstep(hi, lo, d): edge0 > edge1 is UNDEFINED in GLSL. Same curve,
    // portably.
    //
    // THE RING ONLY: for pass 1b neon-blit.frag applies this exact expression
    // against ITS own pixel width, and the ring field's composite (neon-ring-field.frag)
    // applies it for the ring's field. Keep the three in step - one edge.
    if (!blitOwnsCut)
    {
        if (uGlowSide == GLOW_SIDE_INSIDE)       result *= 1.0 - smoothstep(edge.sideBack - edge.sideSoft, edge.sideBack, edge.d);
        else if (uGlowSide == GLOW_SIDE_OUTSIDE) result *= smoothstep(-edge.sideBack, edge.sideSoft - edge.sideBack, edge.d);
    }

    // --- Hard cutoff masks: close the band at its two boundaries ------
    // In the band means outside the shrunk rect (dIn >= 0) and inside the grown
    // one (dOut <= 0); each ramp spans its full softness, centred on inMid /
    // outMid. Disabled sides arrive as a huge sentinel, so the smoothstep
    // passes 1.0 through. BELOW THE GRADE, as coverage, like the cut: the floor
    // removes the staircase, the placement the brightness bias at half
    // coverage.
    //
    // THE RING ONLY: for pass 1b neon-blit.frag applies these same two ramps at
    // destination resolution, from BLIT_CUTOFF_GUARD_PX of lit texels the
    // discards leave it, and the ring field's composite applies them for the
    // ring's field. Keep the three in step - one edge.
    if (!blitOwnsCut)
    {
        result *= smoothstep(-edge.inHalf, edge.inHalf, edge.dIn);
        result *= 1.0 - smoothstep(-edge.outHalf, edge.outHalf, edge.dOut);
    }

    // Premultiplied-alpha output so the effect composites over arbitrary
    // background objects instead of only adding light. Coverage = brightest
    // channel: the hot filament core (alpha ~ 1) occludes the background and
    // reads as a solid tube; the dim halo/bloom (alpha ~ 0) stay additive; the
    // dark surround (alpha = 0) leaves the background untouched. Pairs with
    // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) in the renderer.
    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    // The output dither, on the ring's write to the caller's framebuffer only:
    // pass 1b writes the reduced buffer, which the blit dithers as it writes.
    fragColor = vec4(blitOwnsCut ? result : neonDither(result), alpha);
}
