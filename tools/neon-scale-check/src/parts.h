#ifndef _NEON_SCALE_CHECK_PARTS_H_
#define _NEON_SCALE_CHECK_PARTS_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check parts [--max N] [--within1 PCT]`: a reader of the
    /// neon's two shared parts - the glow (NeonRenderer.Glow) and the ring
    /// atlas (NeonRenderer.Ring), sampled through neon-light.glsl and
    /// NeonRenderer::UploadSharedLight - must rebuild the neon's light as the screen
    /// got it. For every scene at scales 1, 0.5 and 0.25, still and under a
    /// rotating hue, it draws the frame, then the reader's fullscreen pass over
    /// transparent black, and fails a frame off by more than N levels (1: the
    /// screen's output dither) or with fewer than PCT percent of lit pixels
    /// within 1. Returns the process exit code.
    int Parts(int argc, char **argv);
}

#endif
