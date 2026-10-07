// ---------------------------------------------------------------------------
// The neon's grade: the tone map from linear light to the output, shared by
// neon.frag and neon-field.frag.
//
// Not a shader: it has no main(). shaders.h.in injects it, after the tuning
// header (TONE_MAP_SHOULDER, GAMMA_EXPONENT), into the glow and the
// hue-invariant field's composite, which tone-maps col * Fa where neon.frag
// tone-maps its own result - the two have to agree to the bit for the field
// to stand in for pass 1 (I46).
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
