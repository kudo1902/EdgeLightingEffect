#ifndef _NEON_SCALE_CHECK_PARTITION_H_
#define _NEON_SCALE_CHECK_PARTITION_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check partition [--configs N] [--seed S]`: below scale 1.0
    /// the blit (pass 2b) and the edge ring (pass 2c) must tile the frame -
    /// every pixel either one covers is covered by exactly one of them, since
    /// both blend premultiplied-over and a pixel drawn twice composites twice.
    /// This renders N random configs, replays both passes' own triangles
    /// through PassRecorder, and fails on any pixel covered twice or left
    /// uncovered between the two. Returns the process exit code.
    int Partition(int argc, char **argv);
}

#endif
