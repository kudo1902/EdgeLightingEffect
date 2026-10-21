// ---------------------------------------------------------------------------
// The edge ring's field composite (pass 2r): at every scale, onto the caller's
// framebuffer, in pass 2c's place. The factorisation it rests on, the uniforms
// it shares with pass 1c's composite (neon-field.frag) and the shading itself,
// fieldShade, are in neon-field-common.glsl, injected ahead of this file.
//
// Its field is full resolution but holds only the ring, PACKED: the ring is an
// axis-aligned annulus - a box minus a hole (setupRingGeometry) - so its
// pixels are four strips, stored one above the other on the viewport's pixel
// grid: the bottom and the top as they are, the left and the right
// transposed, so a side strip lies along the atlas's width rather than
// wasting a column of it. A fragment finds its box pixel q from vPos, its
// strip from q against the hole, and its texel from the strip's row. The hole
// is CONSERVATIVE - only box pixels the ring can never draw - so every ring
// fragment lands in a strip. The ring shades with scale-1.0 uniforms, so
// unlike pass 1b it multiplies the one-sided cut and the cutoffs in after its
// tone map; this composite does the same, from the pixel's own distance to
// the edge, so a ring with either still takes its field.
// ---------------------------------------------------------------------------

uniform vec2      uRingLo;      ///< vPos (full-res px) of the ring box's lower-left pixel corner.
uniform vec4      uRingHole;    ///< Box pixels the ring never draws: [x0, x1) x [y0, y1), as x0, y0, x1, y1.
uniform vec2      uRingRows;    ///< First atlas row of the left strip and of the right strip.

void main() {
    // Bottom strip, then the top stacked on it, then the left and the right
    // transposed - the layout computeRingFieldLayout builds and
    // renderRingFieldPass bakes. Keep the three together.
    ivec2 q    = ivec2(floor(vPos - uRingLo));
    ivec4 hole = ivec4(uRingHole);
    ivec2 texel;
    if (q.y < hole.y)
    {
        texel = q;
    }
    else if (q.y >= hole.w)
    {
        texel = ivec2(q.x, q.y - hole.w + hole.y);
    }
    else if (q.x < hole.x)
    {
        texel = ivec2(q.y - hole.y, int(uRingRows.x) + q.x);
    }
    else
    {
        texel = ivec2(q.y - hole.y, int(uRingRows.y) + q.x - hole.z);
    }
    vec2 field = texelFetch(uField, texel, 0).rg;
    vec3 result = fieldShade(field);

    // The one-sided cut and the cutoffs, which the ring's own shading applies
    // after its tone map as coverage (neon.frag at scale 1.0, where
    // blitOwnsCut is false): the same expressions, term for term - keep them
    // in step with neon.frag's, and with neon-blit.frag's below 1.0. The field
    // holds Fa from before them (neon-field-bake.frag stops at the grade) and
    // from before the ring's culls, which the bake does not make (V35): each
    // cull lies where one of these factors is exactly 0, so the product
    // composites to nothing wherever the ring would have discarded, decided
    // from this pixel's own vPos. Skipped on a uniform when neither is set:
    // every factor is then exactly 1 (a disabled side's sentinel saturates
    // its smoothstep). The
    // derivative is in uniform control flow, so defined.
    if (uGlowSide != GLOW_SIDE_BOTH || uInsideCutoff < 0.5 * CUTOFF_NEUTRALISED ||
        uOutsideCutoff < 0.5 * CUTOFF_NEUTRALISED)
    {
        vec2  halfSize = uRectSize * 0.5;
        float d        = sdRoundBox(vPos, halfSize, uCornerRadius);
        float sideAA   = max(sdRoundBoxFwidth(vPos, halfSize, uCornerRadius), 1e-6);
        float sideSoft = max(uGlowSideSoftness, sideAA);
        float sideBack = 0.5 * sideAA;
        float inHalf   = 0.5 * max(uInsideCutoffSoftness,  sideAA);
        float outHalf  = 0.5 * max(uOutsideCutoffSoftness, sideAA);
        float inMid    = uInsideCutoff  + 0.5 * max(uInsideCutoffSoftness,  0.0);
        float outMid   = uOutsideCutoff + 0.5 * max(uOutsideCutoffSoftness, 0.0);
        float dOut;
        float dIn;
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            dOut = -CUTOFF_NEUTRALISED;
        }
        else
        {
            dOut = bandOuterDistance(vPos, d, halfSize, uCornerRadius, outMid);
        }
        if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            dIn = CUTOFF_NEUTRALISED;
        }
        else
        {
            dIn = bandInnerDistance(d, inMid);
        }
        if (uGlowSide == GLOW_SIDE_INSIDE)
        {
            result *= 1.0 - smoothstep(sideBack - sideSoft, sideBack, d);
        }
        else if (uGlowSide == GLOW_SIDE_OUTSIDE)
        {
            result *= smoothstep(-sideBack, sideSoft - sideBack, d);
        }
        result *= smoothstep(-inHalf, inHalf, dIn);
        result *= 1.0 - smoothstep(-outHalf, outHalf, dOut);
    }

    float alpha = clamp(max(result.r, max(result.g, result.b)), 0.0, 1.0);
    // The output dither, as the ring's own shading writes it (neon.frag): this
    // is a write to the caller's framebuffer. Pass 1c's to the reduced buffer
    // is not dithered; the blit is.
    result = neonDither(result);
    fragColor = vec4(result, alpha);
}
