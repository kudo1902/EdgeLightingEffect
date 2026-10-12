// ---------------------------------------------------------------------------
// The neon's grade: the tone map from linear light to the output, shared by
// neon.frag and neon-field.frag, and the output dither, shared by those two and
// neon-blit.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header (TONE_MAP_SHOULDER, GAMMA_EXPONENT, OUTPUT_DITHER_LSB), into the glow,
// the hue-invariant field's composite, which tone-maps col * Fa where neon.frag
// tone-maps its own result - the two have to agree to the bit for the field
// to stand in for pass 1 (I46) - and the blit, which only dithers.
// ---------------------------------------------------------------------------

precision highp float;

// Hue-preserving Reinhard: tonemap the peak channel and scale the others by
// the same ratio. Per-channel tonemap desaturates warm mixes (orange -> peach)
// because R saturates while G/B are still linear; scaling by the peak's
// compression preserves the original R:G:B ratio.
vec3 neonToneMap(vec3 result) {
    float peak = max(max(result.r, result.g), result.b);
    float mapped = peak / (peak + TONE_MAP_SHOULDER);
    result = result * (mapped / max(peak, 1e-6));
    return pow(result, vec3(GAMMA_EXPONENT));
}

// The output dither (R7): interleaved gradient noise in +/- OUTPUT_DITHER_LSB / 2
// levels added to a colour about to be written to the caller's 8-bit target,
// so the rounding there breaks the glow's slow gradients into noise instead of
// contour rings. A pure function of the pixel - no time term - so a frame is
// reproducible, and the blit and the edge ring, which tile one frame, draw one
// noise field across their seam. Colour only: the blend is premultiplied, and
// the same noise on alpha would cancel it over a bright background. Only for
// a write to the caller's framebuffer; an offscreen buffer is read back
// filtered, which averages the noise into blotches.
vec3 neonDither(vec3 c) {
    // A constant test, so OUTPUT_DITHER_LSB 0 compiles the noise out rather
    // than computing it and multiplying it by 0.
    if (OUTPUT_DITHER_LSB <= 0.0) {
        return c;
    }
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    return c + (n - 0.5) * (OUTPUT_DITHER_LSB / 255.0);
}
