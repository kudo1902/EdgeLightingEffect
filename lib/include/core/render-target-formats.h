#ifndef _EDGE_LIGHTING_RENDER_TARGET_FORMATS_H_
#define _EDGE_LIGHTING_RENDER_TARGET_FORMATS_H_

#include "gl/framebuffer.h"
#include "gl/gl-header.h"
#include "util/log-util.h"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace EdgeLighting
{
    class RenderTargetFormats;

    template <typename Row>
    bool ResizeInBestFormat(Framebuffer &buffer, RenderTargetFormats &targets, const Row *rows, size_t count,
                            size_t &tier, int width, int height, GLint filter, int attachments, const char *owner,
                            const char *what, const char *consequence);

    /// What one effect's GL context can render to. Each format asked about is
    /// probed once (@c Framebuffer::IsColorRenderable: a 4 x 4 texture, a
    /// framebuffer, @c glCheckFramebufferStatus) and its answer kept, so every
    /// later ask is a lookup. Owned by @ref EdgeLightingEffect - one per effect,
    /// so one per GL context - and handed to every renderer it registers
    /// (@ref BaseRenderer::GetRenderTargetFormats), the way the
    /// @ref SharedBufferRegistry is.
    ///
    /// Asked where a format is used: every buffer with a list of formats
    /// allocates through @ref ResizeInBestFormat (below), which starts at the
    /// first row this driver renders and falls back on a full-size allocation
    /// the probe did not predict; the neon's half-float fields ask before their
    /// programs are built. The formats the library uses are probed before
    /// anything draws - by the startup report (@ref LogReport) for an effect's
    /// object, by @ref AskReportFormats when a renderer outside an effect makes
    /// its own - and every answer is kept, so no probe of them lands
    /// mid-frame, where it would clear the frame's pending GL errors.
    ///
    /// GL thread only. Creates no GL object until asked, so it can be made
    /// before a context exists. See
    /// docs/design/render-target-format-check-plan.md, section 4.
    class RenderTargetFormats
    {
    public:
        /// One format's answer.
        typedef struct Answer
        {
            GLint internalFormat = 0;         ///< The sized internal format asked about, e.g. GL_RGBA16F.
            GLenum status = 0;                ///< glCheckFramebufferStatus; 0 when there is none: the answer is unknown.
            GLenum uploadError = GL_NO_ERROR; ///< What the probe's glTexImage2D raised, if it failed.
            bool renderable = false;          ///< status == GL_FRAMEBUFFER_COMPLETE.

            /// The probe gave no answer about the format: its upload failed
            /// (GL_OUT_OF_MEMORY, a transfer pair the GL rejects) or the status
            /// query raised an error. The allocation decides instead.
            bool IsUnknown() const { return status == 0; }
        } Answer;

        /// The answer for @p internalFormat: probed on the first ask, with
        /// @p format and @p type as the probe's upload, then kept - every
        /// answer, an unknown one (Answer::IsUnknown) included. Unknown is not
        /// "no": a walk lets the full-size allocation decide such a format, as
        /// it did before any probe existed, so a passing failure of the 4 x 4
        /// probe - or a caller's wrong transfer pair - can neither condemn a
        /// format nor make anything probe it again. Keyed by the internal
        /// format alone - renderability is its property; a valid pair only
        /// feeds the upload. A format @ref SetRefusedFormats names comes back
        /// not renderable with status GL_FRAMEBUFFER_UNSUPPORTED, without a
        /// probe and without touching what is kept.
        /// @pre A GL context is current, if this format has no answer kept.
        Answer Ask(GLint internalFormat, GLenum format, GLenum type);

        /// Whether a buffer may try @p internalFormat: it renders here, or its
        /// probe gave no answer (then the allocation decides). False only for
        /// a format this driver refused, or the hook did.
        bool IsRenderable(GLint internalFormat, GLenum format, GLenum type)
        {
            const Answer answer = Ask(internalFormat, format, type);
            return answer.renderable || answer.IsUnknown();
        }

        /// Log, once, which render-target formats this context can draw into:
        /// one row per format a library buffer asks for, then the four
        /// extensions that decide them on GLES, then how many probes the
        /// report made and what they cost. Called by
        /// @ref EdgeLightingEffect::Initialize after GLUtils::LogCaps, so its
        /// probes are the effect's: every walk later finds its answer kept.
        ///
        /// The rows are every format in the format lists of NeonRenderer
        /// (EMISSION_FORMATS, GLOW_COVER_FORMATS and its RG twin,
        /// GATHER_FORMATS, SCALED_FORMATS, GetFieldFormat), LensFlareRenderer
        /// and SpotlightRenderer (their SCALED_FORMATS): a format added to any
        /// of those should get a row here. One that does not is still answered
        /// correctly, on its first ask - it is only missing from the log.
        /// RGBA8, which OffscreenCapture alone also takes, is mandatory in GL
        /// 3.3 and GLES 3.0; it is listed so a broken driver shows.
        ///
        /// The extension line is informational: on desktop GL the strings are
        /// absent by design - float targets are core there, as in GLES 3.2 -
        /// and it says so rather than printing four misleading "no"s.
        void LogReport();

        /// Ask every format @ref LogReport lists, without the log: what a
        /// renderer outside an effect does when it makes its own object
        /// (BaseRenderer::GetRenderTargetFormats), so its walks, too, find their
        /// answers kept instead of probing mid-frame.
        /// @pre A GL context is current.
        void AskReportFormats();

        /// Every answer kept so far, in the order first asked - so its size is
        /// the number of probes made. A refusal by @ref SetRefusedFormats (the
        /// hook's, not the driver's) is not among them.
        const std::vector<Answer> &GetAnswers() const { return mAnswers; }

        /// Verification only - a host never calls this. Every internal format
        /// in @p formats is refused by this object from now on, as a driver
        /// that cannot render it refuses it: @ref Ask says no, so every walk
        /// lands past it, and the neon's fields shade directly. An empty list,
        /// the default, refuses nothing.
        ///
        /// Per effect: a refusal cannot reach another effect, or outlive this
        /// one. It reaches nothing that does not ask - Framebuffer::Resize
        /// knows nothing of it - so a run under it shows a buffer that
        /// allocates without asking as a refused format in use, which is what
        /// `neon-scale-check formats` checks. Set it before the first
        /// allocation it should affect: a walk's tier never moves back up, so
        /// before Initialize to reach the neon's emission table. The precedent
        /// is NeonRenderer::SetBypass - no config field, no C ABI mirror.
        void SetRefusedFormats(std::vector<GLint> formats) { mRefused = std::move(formats); }

        /// Whether @ref SetRefusedFormats names @p internalFormat.
        bool IsRefused(GLint internalFormat) const;

        /// Verification only - a host never calls this. Every full-size
        /// allocation @ref ResizeInBestFormat makes in a format @p formats
        /// names fails from now on, though the probe passed it: the case the
        /// walks' own fallback exists for (memory, a size limit), which no
        /// driver the project runs on produces. The buffer is released as a
        /// failed Resize leaves it, and the fallback is logged naming this
        /// hook. Per effect, set before the allocations it should reach, like
        /// @ref SetRefusedFormats.
        void SetFailedAllocations(std::vector<GLint> formats) { mFailedAllocations = std::move(formats); }

        /// Whether @ref SetFailedAllocations names @p internalFormat.
        bool IsAllocationFailed(GLint internalFormat) const;

    private:
        /// The walk's question, asked at its allocation - private, since only
        /// @ref ResizeInBestFormat may ask it: the index of the first of
        /// @p count @p rows, at or after @p from, that a buffer may try
        /// (renders here, or unknown - the allocation decides); @p count if
        /// none may. That answer is final - the walk tries nothing, so nothing
        /// is allocated in a refused format - and the walk's failure path puts
        /// the tier back on the last row, so the index never reaches past the
        /// list.
        ///
        /// Logs a warning the first time it skips each row for a walk, naming
        /// @p owner and @p what. A walk resumes at its tier, so a list none of
        /// whose rows renders is asked from its last row every frame: the
        /// skips logged are remembered (by the text of @p owner, @p what and
        /// the row's name), so each is still logged once.
        ///
        /// Cheap enough for every frame, which is how often a walk runs (its
        /// Resize early-out is inside its loop): one lookup over the handful
        /// of answers kept, no GL call, nothing built.
        template <typename Row>
        size_t FirstRenderable(const Row *rows, size_t count, size_t from, const char *owner, const char *what)
        {
            for (size_t tier = from; tier < count; ++tier)
            {
                const Row &row = rows[tier];
                const Answer answer = Ask(row.internalFormat, row.format, row.type);
                if (answer.renderable || answer.IsUnknown())
                {
                    return tier;
                }
                logSkipped(owner, what, row.name, tier + 1 < count ? rows[tier + 1].name : nullptr, answer);
            }
            return count;
        }

        template <typename Row>
        friend bool ResizeInBestFormat(Framebuffer &buffer, RenderTargetFormats &targets, const Row *rows,
                                       size_t count, size_t &tier, int width, int height, GLint filter,
                                       int attachments, const char *owner, const char *what,
                                       const char *consequence);

        /// One skip @ref FirstRenderable has logged: the walk's names and the
        /// row's, as text - an owner may be a buffer's own name, whose storage
        /// a later buffer can reuse, so a pointer is no identity.
        typedef struct LoggedSkip
        {
            std::string owner;
            std::string what;
            std::string row;
        } LoggedSkip;

        /// The warning @ref FirstRenderable writes for a row it skips, @p next
        /// being the row it tries after it (nullptr after the last) - the first
        /// time only, for this walk and row.
        void logSkipped(const char *owner, const char *what, const char *row, const char *next,
                        const Answer &answer);

    private:
        std::vector<Answer> mAnswers;           ///< One per format asked, in order: a handful, so a linear scan beats a map.
        std::vector<GLint> mRefused;            ///< SetRefusedFormats' list; checked before mAnswers.
        std::vector<GLint> mFailedAllocations;  ///< SetFailedAllocations' list.
        std::vector<LoggedSkip> mLoggedSkips;   ///< What logSkipped has already said, so a walk resumed on a refused row is quiet.
    };

    /// Allocate @p buffer at @p width x @p height in the best of the @p count
    /// @p rows this context renders. THE format walk: every buffer with a list
    /// of formats goes through it - the neon's emission table, glow coverage
    /// and segment tables, gather buffer and two half-float fields (a list of
    /// one), SharedBuffer::BeginWrite, the flare's and the spotlight's scaled
    /// buffers - so the protocol lives in one place:
    ///   1. ask @p targets where to start (RenderTargetFormats::FirstRenderable,
    ///      one lookup a frame once probed): the first row that renders, or
    ///      whose probe gave no answer;
    ///   2. resize to each row from there until one allocates, logging a
    ///      fallback as "<owner>: <row> <what> target unavailable, falling
    ///      back to <next>. <consequence>" - a format the probe passed or
    ///      could not judge whose full-size allocation failed (memory, a size
    ///      limit);
    ///   3. when no row may be tried or none allocates, leave @p tier on the
    ///      last row - never past the list - and return false.
    ///
    /// @p tier is where the walk resumes, and is left on the row that
    /// allocated. Keep it beside the buffer and pass it back on every call:
    /// re-asking for a format the driver refused would churn the attachment -
    /// Framebuffer::Resize treats a format change as a reallocation, and its
    /// failure path destroys what was there - and a buffer released between
    /// uses cannot record the refusal itself. So a refused format is asked
    /// for, and its fallback logged, once. @p Row names internalFormat,
    /// format, type and name. @p owner, @p what and @p consequence are only
    /// read: the walk runs every frame and builds nothing.
    /// @return false only if no row allocated, in which case @p buffer has no
    ///         attachment at all.
    template <typename Row>
    bool ResizeInBestFormat(Framebuffer &buffer, RenderTargetFormats &targets, const Row *rows, size_t count,
                            size_t &tier, int width, int height, GLint filter, int attachments, const char *owner,
                            const char *what, const char *consequence)
    {
        tier = targets.FirstRenderable(rows, count, tier, owner, what);
        for (; tier < count; ++tier)
        {
            const Row &row = rows[tier];
            // SetFailedAllocations: fail as Resize fails, destroying what the
            // buffer held - a verification run of this very fallback.
            const bool forced = targets.IsAllocationFailed(row.internalFormat);
            if (forced)
            {
                buffer.Release();
            }
            else if (buffer.Resize(width, height, row.internalFormat, row.format, row.type, filter, attachments))
            {
                return true;
            }
            if (tier + 1 < count)
            {
                LOG_E("%s: %s %s target unavailable, falling back to %s. %s%s", owner, row.name, what,
                      rows[tier + 1].name, consequence,
                      forced ? " (failed by RenderTargetFormats::SetFailedAllocations)" : "");
            }
        }
        // Nothing may be tried, or nothing allocated: stay on the last row,
        // the one most likely to allocate on a later call, rather than off the
        // end.
        tier = count == 0 ? 0 : count - 1;
        return false;
    }

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_RENDER_TARGET_FORMATS_H_
