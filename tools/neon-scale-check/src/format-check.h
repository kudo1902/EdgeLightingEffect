#ifndef _NEON_SCALE_CHECK_FORMAT_CHECK_H_
#define _NEON_SCALE_CHECK_FORMAT_CHECK_H_

namespace NeonScaleCheck
{
    /// `neon-scale-check formats [--verbose]`: the library on a driver that
    /// refuses render-target formats, forced through the effect's
    /// FormatAvailability (SetRefusedFormats) on the GPU it runs on - the
    /// 8-bit fallback rows that no Mac reaches otherwise.
    ///
    /// Prints the GPU and the startup report, then draws every `check` scene
    /// at scales 1.0, 0.5 and 0.25 - still, with the hue rotating and with the
    /// intensity moving, the frames the half-float fields serve - plus one
    /// flare and one spotlight scene, each at 0.5 and at 1.0, seven times, each
    /// on a fresh
    /// effect with the neon, the flare, the spotlight and a reader of the
    /// neon's shared buffer:
    ///   - A: nothing refused - the reference;
    ///   - B: RGBA16F, RG16F and R16F refused - GLES 3.0 with neither
    ///     EXT_color_buffer_half_float nor EXT_color_buffer_float;
    ///   - B with both fields bypassed (NeonBypass::GLOW_FIELD | RING_FIELD);
    ///   - B8: everything finer than 8 bits refused - half float and the
    ///     reduced buffers' RGB10_A2, which no conforming driver refuses - so
    ///     every buffer is on its 8-bit row;
    ///   - C: every format the library allocates refused;
    ///   - D: nothing refused, but every full-size allocation in half float
    ///     failed (SetFailedAllocations) - the walks' own fallback, for what
    ///     a probe cannot predict;
    ///   - E: half float unknown to the probe (SetUnknownFormats) - an upload
    ///     that failed for a passing reason, which must not count as "no".
    /// And what each cause of a failure may block (format-check-causes.cpp):
    ///   - D4: on the main cells, RGBA16F incomplete with two attachments
    ///     (a ForcedFailure reporting GL_FRAMEBUFFER_UNSUPPORTED) - the
    ///     gather's segment attachment;
    ///   - D5: on cells of its own (the flare and the spotlight at 1.0, 0.5
    ///     and 0.25, the neon on `arcs` at 0.5 and 0.25), the fields bypassed in it and its own A,
    ///     textures up to one px less than the viewport at 0.5
    ///     (SetMaxTextureSize);
    ///   - D6: on the `segments` cells, beside their own A, the segment
    ///     coverage table's RG16F and RG8 refused, then failing every
    ///     allocation;
    ///   - S: the neon, the flare and the spotlight drawn outside an effect,
    ///     each with its own FormatAvailability.
    /// It fails if:
    ///   - a run probes other than the report's six formats less the ones a
    ///     hook answers (6, 3, 3, 2, 0, 6, 3, 6, 6, 4, 6 for A, B, B bypassed,
    ///     B8, C, D, E, D4, D5, D6 refused, D6 failing), or a
    ///     renderer outside an effect probes other than six by the end of
    ///     its Initialize, or any after;
    ///   - in B, any offscreen draw writes a refused format, or any texture
    ///     alive at the end has one - what a buffer allocated without asking
    ///     FormatAvailability does - or the shared buffer is ever ready in
    ///     anything but RGB10_A2, which needs no half float;
    ///   - in B, a field program links (A must link all three), or any frame
    ///     differs by a byte from the fields-bypassed run's;
    ///   - in B, a frame is further from A's than the bound measured for its
    ///     layer, variant and scale (FORMAT_QUALITY_BOUNDS) - a regression
    ///     gate on the 8-bit rows, not a quality claim;
    ///   - in B8, a refused format is drawn into or alive, the shared buffer
    ///     is ever ready in anything but RGBA8, or a frame is further from
    ///     A's than the same bounds;
    ///   - any frame of any run leaves a GL error;
    ///   - in C, the neon is not dropped, a neon frame draws anything, a
    ///     flare or spotlight frame differs by a byte from A's at 1.0 (with no
    ///     scaled buffer to be had, each falls back to the path that needs
    ///     none), that fallback is not logged once each, or the shared
    ///     buffer is ever ready;
    ///   - in D, no fallback is logged naming the hook, a half-float format is
    ///     drawn into or alive, the shared buffer is ever not RGB10_A2, or any
    ///     frame differs by a byte from B's - the fallback has to land every
    ///     buffer where B's probe did;
    ///   - in E, the report does not print the three rows UNKNOWN, a row is
    ///     skipped or falls back, a half-float target or a field program is
    ///     not reached, the shared buffer is ever not RGB10_A2, or any frame
    ///     differs by a byte from A's - unknown leaves the allocation to
    ///     decide, and here it succeeds;
    ///   - in D4, no gather fallback is logged naming the hook, a cell
    ///     without segments differs by a byte from A's, or the shared buffer
    ///     is ever not RGB10_A2 - kept by format alone, the gather's
    ///     two-attachment failure would put every one-attachment half-float
    ///     buffer on RGBA8;
    ///   - in D5, the flare's or the spotlight's 0.5 frame is not A's 1.0
    ///     frame, a 0.25 frame is
    ///     not A's, the refusal is not logged once for each of the three scaled
    ///     buffers, any allocation is tried and
    ///     fails, or the shared buffer is ready in anything but RGB10_A2;
    ///   - in D6, the table is tried other than never (refused) or once,
    ///     both rows failing (failing), its fill program is built when the
    ///     table was known unavailable before the first frame, the held-back
    ///     warning is not logged once, a frame is more than 2 levels from
    ///     A's, or the shared buffer leaves RGB10_A2 - and A must build the
    ///     fill program, the proof that check can fail. A failed allocation
    ///     is final (FormatAvailability), so no case waits for one to clear;
    ///   - in S, a frame differs by a byte from A's frame of the same cell,
    ///     or leaves a GL error.
    /// Returns the process exit code. See docs/design/render-target-format-check-plan.md,
    /// section 6.
    int FormatCheck(int argc, char **argv);
}

#endif
