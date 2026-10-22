precision highp float;

// The lens flare's output dither, shared by the two programs that write the
// layer to the caller's framebuffer: lens-flare.frag at resolutionScale 1.0,
// and lens-flare-blit.frag below it. Injected after @LENS_FLARE_TUNING@ (it
// reads FLARE_DITHER_STEPS), so it declares its own precision - a #version 300
// es fragment shader has no default float precision, and this lands ahead of
// the file's own precision line. Not a shader: no main().
//
// ONLY for a write to the caller's 8-bit target. Never to the reduced buffer:
// the blit reads that back bilinearly, which averages the noise into blotches
// one buffer texel wide - and the buffer holds ten bits so that it keeps the
// tail to a quarter level and the blit has something left to dither. The neon
// and the spotlight learned the same thing (R7, V27); see LensFlareRenderer's
// SCALED_FORMAT.
//
// COLOUR ONLY, as the neon's - not the coverage alpha, which the spotlight's
// dither moves too. The spotlight screens, so its alpha composites exactly as
// its colour does; this layer composites premultiplied over
// (`src + dst * (1 - a)`), where the same noise on both would cancel against a
// bright destination.

/// Interleaved gradient noise in [-0.5, 0.5), scaled to +/- FLARE_DITHER_STEPS
/// / 2 levels of an 8-bit target: strictly under half a level at 1.0, so GL's
/// round-to-nearest cannot take a channel the flare left at an exact level
/// anywhere but where it was.
///
/// @p p is in PIXELS of the target being written: one step of @p p must be one
/// destination pixel, or the noise is sampled at the wrong pitch and stops
/// being noise. lens-flare.frag hands it gl_FragCoord, which it reads anyway;
/// lens-flare-blit.frag the same pixel centre from its interpolated position.
float flareDitherOffset(vec2 p) {
    float n = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
    return n * (FLARE_DITHER_STEPS / 255.0);
}
