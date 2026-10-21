#ifndef _NEON_SCALE_CHECK_SEQUENCE_H_
#define _NEON_SCALE_CHECK_SEQUENCE_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check sequence [--sequences N] [--frames F] [--seed S]
    /// [--only K] [--isolate] [--dump DIR]`: a differential check of the
    /// neon's skips and reuses. Seeded random config SEQUENCES - arcs and
    /// segments moving, intensity, bloom and glow animations, the hue on and
    /// off, scale changes, viewport resizes, a moving and resizing rect, the
    /// layer and opaqueOnly toggled, the one-sided cut, cutoffs, fills,
    /// colours and shape changed, with held stretches between - are driven
    /// through effects that differ ONLY in NeonRenderer::SetBypass:
    ///   - `forced` (NeonBypass::ALL), the reference: every pass redone,
    ///     every frame, every rebuild ungated, no field;
    ///   - `exact` (only the skips documented exact active): every frame and
    ///     every shared buffer texel byte-identical to `forced`;
    ///   - `normal` (the renderer as shipped): within NORMAL_TOLERANCE of
    ///     `forced`, INTENSITY_TOLERANCE on a frame whose intensity moved
    ///     (the fields' gain, I56).
    /// With --isolate, `normal` and `exact` each with one more skip bypassed
    /// join them, one effect per bit, and every failing frame lists the ones
    /// it passes: the skips whose bypass clears it. Each sequence
    /// is seeded by S + its index, so --only K replays sequence K alone.
    /// --dump writes each failing frame of each run and of `forced` as PNG.
    /// Returns the process exit code.
    int SequenceCheck(int argc, char **argv);
}

#endif
