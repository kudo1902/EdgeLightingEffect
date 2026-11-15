#ifndef _NEON_SCALE_CHECK_FUZZ_H_
#define _NEON_SCALE_CHECK_FUZZ_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check fuzz [--seed S] [--write FILE] [--against FILE]
    /// [--dump DIR]`: seeded random neon configs, drawn by this build and
    /// compared with another build's - the gate for a change that should move
    /// no pixel of the neon.
    ///
    /// The neon and the debug layer draw four runs (FUZZ_* in fuzz.cpp), every
    /// frame with a fixed delta of 1/60 s:
    ///   - seq:   one effect, a new unrelated config every three frames, every
    ///            frame captured - the first frame after a change and the
    ///            steady ones;
    ///   - fresh: a new effect per config, frames 0 and 1 - program builds and
    ///            first bakes;
    ///   - drift: small changes every frame (size, position, glow, arc ends,
    ///            a segment travelling, the scale, cutoffs, the opaque mode),
    ///            as an animating host makes them;
    ///   - anim:  one to three neon presets on a random config.
    /// Every NeonConfig field, the geometry, the viewport (six, 640 x 1136 to
    /// 3840 x 2160) and the background are drawn at random, and the debug
    /// layer's shared-buffer inset is on in about a third of the configs, so
    /// the neon's shared buffer is compared too. Values stay inside what
    /// ConfigValidation keeps, so a build from before it draws them.
    ///
    /// Each captured frame is hashed (FNV-1a over its RGBA bytes) under a key
    /// "phase run frame". --write FILE writes the hashes; --against FILE fails
    /// on any key whose hash differs or is missing, and with --dump DIR writes
    /// this build's frame for each such key as DIR/fuzz_<phase>_<run>_<frame>.png
    /// (RGBA). Run the other build with --against this one's file and its own
    /// --dump directory, and `diff` the two directories to see by how much.
    /// Without --against it still fails on a GL error left after any frame.
    /// About a minute a seed on an Apple M2 Pro. Returns the process exit code.
    int FuzzCheck(int argc, char **argv);
}

#endif
