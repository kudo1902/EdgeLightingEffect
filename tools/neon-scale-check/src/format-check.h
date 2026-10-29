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
    /// And what each cause of a failure may block (format-check-causes.cpp):
    ///   - D4: on the main cells, RGBA16F incomplete with two attachments
    ///     (a ForcedFailure reporting GL_FRAMEBUFFER_UNSUPPORTED) - the
    ///     gather's segment attachment;
    ///   - D7: D4's failure, then out of memory instead, on 12 `segments`
    ///     cells whose rect grows - so the gather buffer shrinks below the
    ///     last failure every cell;
    ///   - D5: on the retry cells, the fields bypassed in it and its own A,
    ///     textures up to one px less than the viewport at 0.5
    ///     (SetMaxTextureSize);
    ///   - D6: on the `segments` cells, beside their own A, the segment
    ///     coverage table's RG16F and RG8 refused, then failing every
    ///     allocation;
    ///   - S: the neon, the flare and the spotlight drawn outside an effect,
    ///     each with its own RenderTargetFormats.
    /// It fails if:
    ///   - a run probes other than the report's five formats less the ones a
    ///     hook answers (5, 2, 2, 0, 5, 2, 5, 5, 5, 5, 5, 5, 3, 5 for A, B, B
    ///     bypassed, C, D, E, D2, D3, D4, both D7 runs, D5, D6 refused, D6
    ///     failing), or a
    ///     renderer outside an effect probes other than five by the end of
    ///     its Initialize, or any after;
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
    ///     in anything but RGBA16F;
    ///   - in D4, no gather fallback is logged naming the hook, a cell
    ///     without segments differs by a byte from A's, or the shared buffer
    ///     is ever not RGBA16F - kept by size alone, the gather's
    ///     two-attachment failure put every larger one-attachment buffer on
    ///     RGBA8;
    ///   - in D7, the incomplete framebuffer fails other than once - kept by
    ///     size, it was tried again at every smaller gather size - or out of
    ///     memory fails only once, so the gather never shrank and the check
    ///     above proved nothing;
    ///   - in D5, the flare's or the spotlight's 0.5 frame is not A's 1.0
    ///     frame, a 0.25 frame is not A's, the refusal is not logged once for
    ///     each of the three scaled buffers, any allocation is tried and
    ///     fails, or the shared buffer is ready in anything but RGBA16F;
    ///   - in D6, the table is tried other than never (refused) or once,
    ///     both rows failing (failing), its fill program is built when the
    ///     table was known unavailable before the first frame, the held-back
    ///     warning is not logged once, a frame is more than 2 levels from
    ///     A's, or the shared buffer leaves RGBA16F - and A must build the
    ///     fill program, the proof that check can fail;
    ///   - in D8, with only the first allocation of one format failing -
    ///     RGBA16F (the emission table's; on `arcs`, `segments`, the flare and
    ///     the spotlight), RG16F on `arcs` (the coverage table's), RG16F on
    ///     `segments` (the segment table's), RGBA16F from the viewport's size
    ///     on `arcs` (the shared buffer's) and on the flare (its buffer's),
    ///     and RGBA16F at two attachments on `segments` at scale 1/16 (the
    ///     gather's, which falls alone there) - the frame after the 3 s of
    ///     frame time that let it expire, or a cell drawn after it, differs
    ///     by a byte from A's (every buffer must move back up, those a still
    ///     frame reuses included, and a field baked from a buffer's 8-bit row
    ///     must not be composited after it - and is baked again on the next
    ///     frame), nothing was drawn in the 8-bit row meanwhile, the shared
    ///     buffer is not seen as RGBA8 and then RGBA16F (the RGBA16F and
    ///     viewport-sized ways) or leaves RGBA16F (the RG16F and gather
    ///     ways), the failure is not cleared or that is not logged once;
    ///     with every RGBA16F allocation from the viewport's size failing on
    ///     `small_rect`, the shared buffer is not tried exactly twice - its
    ///     first allocation and its retry - or the frame after it, or any
    ///     texel of the shared buffer (read back), differs from a run whose
    ///     failure is held, or a texel outside its last write's window
    ///     (SharedBuffer::GetLastWriteWindow) is not 0, or none lies outside
    ///     it - then the check proves nothing;
    ///     and with every RGBA16F allocation failing over 40
    ///     frames 1 s apart, it is not tried exactly 5 times (0, 2, 6, 14,
    ///     30 s: one retry per expiry, each doubling the wait) or does not end
    ///     waiting 32 s until 62 s. D and D6 failing hold their failure for
    ///     the run (RenderTargetFormats::SetOutOfMemoryRetry, infinity);
    ///   - in S, a frame differs by a byte from A's frame of the same cell,
    ///     or leaves a GL error.
    /// Returns the process exit code. See docs/design/render-target-format-check-plan.md,
    /// section 6.
    int FormatCheck(int argc, char **argv);
}

#endif
