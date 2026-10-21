#ifndef _NEON_SCALE_CHECK_SHARED_BUFFER_H_
#define _NEON_SCALE_CHECK_SHARED_BUFFER_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check shared-buffer [--images DIR]`: the neon's shared
    /// buffer (SharedBufferId::NEON), read the way an internal renderer reads
    /// it - by a probe renderer registered after the neon and one moved
    /// before it, each taking the buffer in its Render. For every scene at
    /// every scale, still and with the hue rotating (so the field's composite
    /// fills the buffer), it renders the steady frame and fails if:
    ///   - the probe is handed no ready buffer, or one that is not the whole
    ///     frame's viewport at the scale;
    ///   - the probe before the neon sees another image (ready, version) than
    ///     the one after it - the effect runs every layer's offscreen phase
    ///     before any composites;
    ///   - a still frame drawn with nothing changed moves the version;
    ///   - on a scene with no cut, cutoff or fill, the buffer blitted back the
    ///     way the neon's blit reads it carries under SHARED_MIN_LINE_ENERGY
    ///     of the frame's light on the line - what a buffer without the
    ///     filament does;
    ///   - the hue variant never reaches the field's composite, or the buffer
    ///     is ready while the neon is off or not ready once it is on again;
    ///   - after a rect that moves and shrinks at one scale - the buffer kept,
    ///     so only the windowed clear keeps the old glow out of it - the buffer
    ///     differs by a single texel from the moved rect's on a fresh effect.
    /// It does not compare frames: that sharing the buffer moves no pixel is
    /// `generate` and `diff --max 0` against a build from before.
    /// With --images, writes each buffer over black, top row first, as
    /// DIR/<scene>_<variant>_<scale>.png. Returns the process exit code.
    int SharedBufferCheck(int argc, char **argv);
}

#endif
