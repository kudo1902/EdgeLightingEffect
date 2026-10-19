#ifndef _NEON_SCALE_CHECK_CLOCK_CHECK_H_
#define _NEON_SCALE_CHECK_CLOCK_CHECK_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check clock [--frames K] [--dump DIR]`: the effect's clock
    /// and the phases its shaders are handed, at a start time of about ten
    /// days - the
    /// uptime an edge device running the effect continuously reaches, and
    /// past 2^19 s (~6.07 days), where a float clock stepped by 1/60 s stopped
    /// moving at all. Fails if:
    ///   - EdgeLighting::Clock, set to the start time, does not advance by the
    ///     sum of 600 frame deltas of 1/60 s to within a microsecond;
    ///   - GetCyclePhase, stepped by 1/60 s there, does not step by
    ///     rate / 60 to within 1e-6 of a cycle every frame;
    ///   - K frames of the neon (hue rotating), the droplets, the lens flare
    ///     (rays spinning) and the debug layer's gradient strip, drawn by an
    ///     effect whose clock starts at that time, differ by more than one
    ///     level from the same K frames drawn by an effect starting at 0. The
    ///     start time, CLOCK_START_SECONDS, is a whole number of cycles of
    ///     every phase involved (the hue's, the flare's and the droplets'
    ///     DROPLET_TIME_PERIOD), so the two runs are owed the same frames;
    ///   - the last of those frames equals the first: the effect stood still.
    /// Prints, for the record, how far a float accumulator would have moved
    /// over the same 600 frames. With --dump DIR, writes the run from 0 as
    /// DIR/clock_NNN.png (RGBA), for `diff` against another build's - how the
    /// change moves frames at an ordinary uptime. Returns the process exit
    /// code.
    int ClockCheck(int argc, char **argv);
}

#endif
