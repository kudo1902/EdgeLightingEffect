#ifndef _NEON_SCALE_CHECK_FORMAT_CHECK_H_
#define _NEON_SCALE_CHECK_FORMAT_CHECK_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check formats [--verbose]`: the library on a driver that
    /// refuses render-target formats, forced through the effect's
    /// RenderTargetFormats (SetRefusedFormats) on the GPU it runs on - the
    /// 8-bit fallback rows that no Mac reaches otherwise.
    ///
    /// Prints the GPU and the startup report, then draws every `check` scene
    /// at scales 1.0, 0.5 and 0.25 - still, with the hue rotating and with the
    /// intensity moving, the frames the half-float fields serve - plus one
    /// flare and one spotlight scene at 0.5 and at 1.0, six times, each on a
    /// fresh
    /// effect with the neon, the flare, the spotlight and a reader of the
    /// neon's shared buffer:
    ///   - A: nothing refused - the reference;
    ///   - B: RGBA16F, RG16F and R16F refused - GLES 3.0 with neither
    ///     EXT_color_buffer_half_float nor EXT_color_buffer_float;
    ///   - B with both fields bypassed (NeonBypass::GLOW_FIELD | RING_FIELD);
    ///   - C: every format the library allocates refused;
    ///   - D: nothing refused, but every full-size allocation in half float
    ///     failed (SetFailedAllocations) - the walks' own fallback, for what
    ///     a probe cannot predict;
    ///   - E: half float unknown to the probe (SetUnknownFormats) - an upload
    ///     that failed for a passing reason, which must not count as "no".
    /// And on retry cells of their own - the flare and the spotlight at 1.0,
    /// 0.5 and 0.25, the neon on `arcs` at 0.5 and 0.25 - beside A and B drawn
    /// on the same cells, D from a size: SetFailedAllocations fails only
    /// allocations of RetryThreshold texels or more, halfway between the
    /// viewport at 0.25 and at 0.5, so each viewport-sized scaled buffer fails
    /// at 0.5 and must allocate again at 0.25:
    ///   - D2: half float fails from that size;
    ///   - D3: every format fails from that size.
    /// It fails if:
    ///   - a run probes other than the report's five formats less the ones a
    ///     hook answers (5, 2, 2, 0, 5, 2, 5, 5 for A, B, B bypassed, C, D, E,
    ///     D2, D3);
    ///   - in B, any offscreen draw writes a refused format, or any texture
    ///     alive at the end has one - what a buffer allocated without asking
    ///     RenderTargetFormats does - or the shared buffer is ever ready in
    ///     anything but RGBA8;
    ///   - in B, a field program links (A must link all three), or any frame
    ///     differs by a byte from the fields-bypassed run's;
    ///   - in B, a frame is further from A's than the bound measured for its
    ///     layer, variant and scale (FORMAT_QUALITY_BOUNDS) - a regression
    ///     gate on the 8-bit rows, not a quality claim;
    ///   - any frame of any run leaves a GL error;
    ///   - in C, the neon is not dropped, a neon frame draws anything, a
    ///     flare or spotlight frame differs by a byte from A's at 1.0 (with no
    ///     scaled buffer to be had, each falls back to the path that needs
    ///     none), that fallback is not logged once per layer, or the shared
    ///     buffer is ever ready;
    ///   - in D, no fallback is logged naming the hook, a half-float format is
    ///     drawn into or alive, the shared buffer is ever not RGBA8, or any
    ///     frame differs by a byte from B's - the fallback has to land every
    ///     buffer where B's probe did;
    ///   - in E, the report does not print the three rows UNKNOWN, a row is
    ///     skipped or falls back, a half-float target or a field program is
    ///     not reached, the shared buffer is ever not RGBA16F, or any frame
    ///     differs by a byte from A's - unknown leaves the allocation to
    ///     decide, and here it succeeds;
    ///   - in D2, the flare's or the spotlight's frame at 0.5 differs by a byte
    ///     from B's (the 8-bit row), any frame at 0.25 differs from A's (half
    ///     float, tried again below the size that failed), no fallback is
    ///     logged naming the hook, or the shared buffer is not seen as RGBA8
    ///     and then RGBA16F;
    ///   - in D3, the flare's or the spotlight's frame at 0.5 differs from
    ///     A's at 1.0 (no scaled buffer in any format), any frame at 0.25
    ///     differs from A's, the fall back to 1.0 and the return to the scale
    ///     are not each logged once per layer, or the shared buffer is ready
    ///     in anything but RGBA16F.
    /// Returns the process exit code. See docs/design/render-target-format-check-plan.md,
    /// section 6.
    int FormatCheck(int argc, char **argv);
}

#endif
