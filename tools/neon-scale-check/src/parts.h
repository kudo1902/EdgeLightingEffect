#ifndef _NEON_SCALE_CHECK_PARTS_H_
#define _NEON_SCALE_CHECK_PARTS_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check parts [--max N] [--within1 PCT]`: a reader of the
    /// neon's two shared parts - the glow (NeonRenderer.Glow) and the ring
    /// atlas (NeonRenderer.Ring), sampled through neon-light.glsl and
    /// NeonRenderer::UploadSharedLight - must rebuild the neon's light as the screen
    /// got it. For every gate scene (the page's twelve, two moved across the
    /// frame's corner, three as a thin band round it) at scales 1, 0.5 and
    /// 0.25, still, under a rotating hue and under an intensity that falls
    /// and then holds, it draws the frame, then the reader's fullscreen pass
    /// over transparent black, and fails a frame off by more than N levels
    /// (1: the screen's output dither) or with fewer than PCT percent of lit
    /// pixels within 1 - and a held frame more than 1 off the same config
    /// drawn with no history. Returns the process exit code.
    int Parts(int argc, char **argv);

    /// `neon-scale-check shared [--max N] [--within1 PCT]`: the shared buffer
    /// (NeonRenderer.Composite), assembled from the parts, must be the
    /// screen's own frame - at shared scale 1 texel for pixel, with and
    /// without a fill; at 0.5 and 0.4 the screen's light averaged over each
    /// texel's square, weighted by cover. Every texel, those past the frame
    /// included (the light at its edge). The gate scenes and motions of
    /// @ref Parts, held frames included. The screen is dithered and the
    /// shared buffer is not, so 1-2 levels is the floor.
    int Shared(int argc, char **argv);
}

#endif
