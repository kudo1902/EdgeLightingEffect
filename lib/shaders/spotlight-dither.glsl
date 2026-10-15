precision highp float;

// The spotlight's output dither, shared by the two programs that write the
// layer to the caller's framebuffer: spotlight.frag at resolutionScale 1.0, and
// spotlight-blit.frag below it. Injected after @SPOTLIGHT_TUNING@ (it reads
// SPOT_DITHER_STEPS), so it declares its own precision - a #version 300 es
// fragment shader has no default float precision, and this lands ahead of the
// file's own precision line. Not a shader: no main().
//
// ONLY for a write to the caller's 8-bit target. Never to the reduced buffer:
// the blit reads that back bilinearly, which averages the noise into blotches
// one buffer texel wide, and an RGBA8 buffer has already rounded the dark
// tail into one-level plateaus before the blit sees it - the contours this
// dither exists to remove come back as blocky diamonds. The neon learned the
// same thing (R7, neon-grade.glsl); see SpotlightRenderer::Render.

/// Interleaved gradient noise, in [-0.5, 0.5). One fract, one dot: the whole
/// dither costs about as much as the bloom's sqrt.
///
/// Deliberately NOT the fract(sin(dot(...))) hash. That one feeds sin an
/// argument in the tens of thousands, and how much of it survives is a
/// precision question every GPU answers differently - on a part that rounds
/// it coarsely the "noise" degenerates into a second set of bands, which is
/// the artefact this function exists to remove.
///
/// @p p is in PIXELS of the target being written: one step of @p p must be one
/// destination pixel, or the noise is sampled at the wrong pitch and stops
/// being noise. spotlight.frag hands it vLocal (a rotation plus a translation
/// of the framebuffer, so the pattern travels with the beam and two
/// overlapping lamps dither independently); spotlight-blit.frag hands it
/// gl_FragCoord, since by then the lamps are one summed image.
float spotDither(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5;
}

/// The dither offset to add to a colour and its coverage alike, in output
/// units: +/- SPOT_DITHER_STEPS / 2 levels of an 8-bit target.
///
/// Strictly under half a step at SPOT_DITHER_STEPS 1.0 (`fract` returns
/// strictly under 1), so GL's round-to-nearest cannot take a fragment the
/// falloff left at ZERO up to 1 - the strip's dark corners and, on the scaled
/// path, the whole unlit viewport the blit covers stay black rather than
/// acquiring a speckle.
float spotDitherOffset(vec2 p) {
    return spotDither(p) * (SPOT_DITHER_STEPS / 255.0);
}
