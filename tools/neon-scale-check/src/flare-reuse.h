#ifndef _NEON_SCALE_CHECK_FLARE_REUSE_H_
#define _NEON_SCALE_CHECK_FLARE_REUSE_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check flare-reuse [--no-time] [--dump DIR]`: the lens
    /// flare's reuse of its scaled buffer (LensFlareRenderer's
    /// @c mScaledCache) against a renderer that redraws it every frame
    /// (LensFlareRenderer::SetBypass).
    ///
    /// Two effects - the neon and the flare, as the demo stacks them - draw
    /// one scripted sequence: still frames, the sun moving round the
    /// perimeter, the rays spinning and stopping, the clock paused with them
    /// spinning, an intensity change, a ghost colour change, the rect moving,
    /// a viewport change and back, the layer off and on, and the scale
    /// through 0.25, 1.0 and back to 0.5. Every frame is read back with its
    /// alpha and the two must match BYTE FOR BYTE. Then, unless --no-time,
    /// times still frames - the flare alone at scale 0.5, Update + Render +
    /// glFinish per frame - with and without the reuse at 1920x1080 and
    /// 3840x2160. With --dump DIR, writes both runs' frames of any mismatch.
    /// Returns the process exit code: non-zero on any mismatch.
    int FlareReuseCheck(int argc, char **argv);
}

#endif
