#ifndef _EDGE_LIGHTING_RENDER_TARGET_FORMATS_H_
#define _EDGE_LIGHTING_RENDER_TARGET_FORMATS_H_

#include "gl/framebuffer.h"
#include "gl/gl-header.h"
#include "util/log-util.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace EdgeLighting
{
    class RenderTargetFormats;

    template <typename Row>
    bool ResizeInBestFormat(Framebuffer &buffer, RenderTargetFormats &targets, const Row *rows, size_t count,
                            int width, int height, GLint filter, int attachments, const char *owner, const char *what,
                            const char *consequence, size_t *allocated = nullptr);

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
    /// first row this driver may render and falls back on a full-size
    /// allocation the probe did not predict; the neon's half-float fields ask
    /// before their programs are built. The formats the library uses are probed
    /// before anything draws - by the startup report (@ref LogReport) for an
    /// effect's object, by @ref AskReportFormats when a renderer outside an
    /// effect makes its own - and every answer is kept, so no probe of them
    /// lands mid-frame, where it would clear the frame's pending GL errors.
    ///
    /// It also keeps the other half of what decides a format: the full-size
    /// allocations that failed though the probe passed the format (memory, a
    /// size limit), as the smallest size that failed per format. A walk skips a
    /// format at that size or larger and tries it again below it, so a buffer
    /// keeps no memory of its own and one failure at 4K does not cost a
    /// smaller buffer its format for the effect's life.
    ///
    /// GL thread only. Creates no GL object until asked, so it can be made
    /// before a context exists. See
    /// docs/design/render-target-format-check-plan.md, section 4.
    class RenderTargetFormats
    {
    public:
        /// What the probe says about a format - decided once, in @ref Ask.
        typedef enum class Verdict
        {
            /// The probe's framebuffer was complete.
            RENDERABLE,
            /// A definitive "no": the framebuffer was incomplete, or the 4 x 4
            /// upload raised GL_INVALID_ENUM or GL_INVALID_VALUE - an argument
            /// the GL does not accept at all, which for a library row means the
            /// internal format (the spec names GL_INVALID_VALUE; Apple's GL
            /// raises GL_INVALID_ENUM, measured). Final: no walk tries it.
            REFUSED,
            /// No answer about the format: the upload failed for a reason that
            /// says nothing about it - GL_OUT_OF_MEMORY, or GL_INVALID_OPERATION
            /// (a format the GL takes, with a transfer pair it does not: the
            /// asker's mistake) - or the status query raised an error. The
            /// full-size allocation decides, as it did before any probe
            /// existed.
            UNKNOWN,
        } Verdict;

        /// One format's answer.
        typedef struct Answer
        {
            GLint internalFormat = 0;          ///< The sized internal format asked about, e.g. GL_RGBA16F.
            Verdict verdict = Verdict::UNKNOWN; ///< What the probe says - the only field a decision reads.
            GLenum status = 0;                 ///< glCheckFramebufferStatus; 0 when there is none. For the log.
            GLenum uploadError = GL_NO_ERROR;  ///< What the probe's glTexImage2D raised, if it failed. For the log.
        } Answer;

        /// The answer for @p internalFormat: probed on the first ask, with
        /// @p format and @p type as the probe's upload, then kept - every
        /// answer, an unknown one included, so a passing failure of the 4 x 4
        /// probe - or a caller's wrong transfer pair - can neither condemn a
        /// format nor make anything probe it again. Keyed by the internal
        /// format alone - renderability is its property; a valid pair only
        /// feeds the upload. A format a verification hook names
        /// (@ref SetRefusedFormats, @ref SetUnknownFormats) comes back with
        /// the hook's verdict, without a probe and without touching what is
        /// kept.
        /// @pre A GL context is current, if this format has no answer kept.
        Answer Ask(GLint internalFormat, GLenum format, GLenum type);

        /// Whether this driver renders to @p internalFormat - the verdict
        /// alone: false for a format it refused AND for one its probe could
        /// not judge. What a host reports as supported.
        bool IsRenderable(GLint internalFormat, GLenum format, GLenum type)
        {
            return Ask(internalFormat, format, type).verdict == Verdict::RENDERABLE;
        }

        /// Whether a buffer may allocate @p width x @p height x @p attachments
        /// in @p internalFormat: it renders here, or its probe gave no answer -
        /// then the allocation decides - and no allocation of it that large
        /// has failed. False for a format this driver refused (or a hook did),
        /// and for one that already failed at this size or larger. What a
        /// buffer asks before it commits to a format - the neon's fields ask
        /// it before they build their programs; ResizeInBestFormat's walk asks
        /// the same of each row.
        bool MayAllocate(GLint internalFormat, GLenum format, GLenum type, int width, int height, int attachments)
        {
            return Ask(internalFormat, format, type).verdict != Verdict::REFUSED &&
                   !hasFailedAt(internalFormat, static_cast<int64_t>(width) * height * attachments);
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
        /// the number of probes made. A hook's answer is not among them.
        const std::vector<Answer> &GetAnswers() const { return mAnswers; }

        /// Verification only - a host never calls this. Every internal format
        /// in @p formats is refused by this object from now on, as a driver
        /// that cannot render it refuses it: @ref Ask says REFUSED, so every
        /// walk lands past it, and the neon's fields shade directly. An empty
        /// list, the default, refuses nothing.
        ///
        /// Per effect: a refusal cannot reach another effect, or outlive this
        /// one. It reaches nothing that does not ask - Framebuffer::Resize
        /// knows nothing of it - so a run under it shows a buffer that
        /// allocates without asking as a refused format in use, which is what
        /// `neon-scale-check formats` checks. Set it before the first
        /// allocation it should affect - before Initialize to reach the neon's
        /// emission table, allocated once there. The precedent is
        /// NeonRenderer::SetBypass - no config field, no C ABI mirror.
        void SetRefusedFormats(std::vector<GLint> formats) { mRefused = std::move(formats); }

        /// Whether @ref SetRefusedFormats names @p internalFormat.
        bool IsRefused(GLint internalFormat) const;

        /// Verification only - a host never calls this. Every full-size
        /// allocation @ref ResizeInBestFormat makes in a format @p formats
        /// names fails from now on, though the probe passed it: the case the
        /// walks' own fallback exists for (memory, a size limit), which no
        /// driver the project runs on produces. The buffer is released as a
        /// failed Resize leaves it, the failure is recorded as a real one is,
        /// and the fallback is logged naming this hook. Per effect, set before
        /// the allocations it should reach, like @ref SetRefusedFormats.
        void SetFailedAllocations(std::vector<GLint> formats) { mFailedAllocations = std::move(formats); }

        /// Verification only - a host never calls this. Every internal format
        /// in @p formats comes back from @ref Ask as UNKNOWN (status 0, no
        /// probe), as a probe whose upload failed for a passing reason leaves
        /// it - so a run can drive the unknown path, which no driver the
        /// project runs on produces: the walks must still allocate such a
        /// format, and the fields still build. Per effect, set before the
        /// first ask, like @ref SetRefusedFormats; a refusal wins over it.
        void SetUnknownFormats(std::vector<GLint> formats) { mUnknown = std::move(formats); }

    private:
        /// The walk's question, asked at its allocation - private, since only
        /// @ref ResizeInBestFormat may ask it: the index of the first of
        /// @p count @p rows, at or after @p from, that may be tried at
        /// @p texels (width x height x attachments) - its verdict is not
        /// REFUSED, and no allocation of it that large has failed
        /// (@ref recordFailedAllocation); @p count if none may. A refused row
        /// is final - the walk tries nothing in it.
        ///
        /// Logs a warning the first time it skips each row for a walk, naming
        /// @p owner and @p what - refused, or already failed this large by
        /// another walk, so every buffer's log says what the next row costs
        /// it; a walk asks every frame, so the skips logged are remembered (by
        /// the text of @p owner, @p what and the row's name). A walk's own
        /// failure is not logged again as a skip: its fallback line said it.
        ///
        /// Cheap enough for every frame, which is how often a walk runs (its
        /// Resize early-out is inside it): a lookup over the handful of
        /// answers and failures kept, no GL call, nothing built.
        template <typename Row>
        size_t firstToTry(const Row *rows, size_t count, size_t from, int64_t texels, const char *owner,
                          const char *what)
        {
            for (size_t index = from; index < count; ++index)
            {
                const Row &row = rows[index];
                const char *next = index + 1 < count ? rows[index + 1].name : nullptr;
                const Answer answer = Ask(row.internalFormat, row.format, row.type);
                if (answer.verdict == Verdict::REFUSED)
                {
                    logSkipped(owner, what, row.name, next, answer);
                    continue;
                }
                if (hasFailedAt(row.internalFormat, texels))
                {
                    logSkippedAfterFailure(owner, what, row.name, next, texels);
                    continue;
                }
                return index;
            }
            return count;
        }

        template <typename Row>
        friend bool ResizeInBestFormat(Framebuffer &buffer, RenderTargetFormats &targets, const Row *rows,
                                       size_t count, int width, int height, GLint filter, int attachments,
                                       const char *owner, const char *what, const char *consequence,
                                       size_t *allocated);

        /// One format whose full-size allocation failed though its verdict let
        /// a walk try it: the smallest allocation, in texels x attachments,
        /// that failed.
        typedef struct FailedAllocation
        {
            GLint internalFormat;
            int64_t texels;
        } FailedAllocation;

        /// Whether an allocation of @p internalFormat at @p texels or more has
        /// failed: the walk then skips it at this size.
        bool hasFailedAt(GLint internalFormat, int64_t texels) const;

        /// Record that an allocation of @p internalFormat at @p texels failed:
        /// no walk tries it that large again, and one smaller still may. Keeps
        /// the smallest size that failed.
        void recordFailedAllocation(GLint internalFormat, int64_t texels);

        /// Whether @ref SetFailedAllocations names @p internalFormat.
        bool isAllocationForcedToFail(GLint internalFormat) const;

        /// One skip @ref firstToTry has logged: the walk's names and the
        /// row's, as text - an owner may be a buffer's own name, whose storage
        /// a later buffer can reuse, so a pointer is no identity.
        typedef struct LoggedSkip
        {
            std::string owner;
            std::string what;
            std::string row;
        } LoggedSkip;

        /// Whether this walk's skip of @p row has not been logged yet; records
        /// it as logged either way. Also how a walk's own failure marks its
        /// row, so the frames after it do not log it again as a skip.
        bool isNewSkip(const char *owner, const char *what, const char *row);

        /// The warning @ref firstToTry writes for a refused row it skips,
        /// @p next being the row after it (nullptr after the last) - the first
        /// time only, for this walk and row.
        void logSkipped(const char *owner, const char *what, const char *row, const char *next,
                        const Answer &answer);

        /// The same, for a row skipped at @p texels because an allocation of
        /// it that large failed - in another walk, or this one at an earlier
        /// size.
        void logSkippedAfterFailure(const char *owner, const char *what, const char *row, const char *next,
                                    int64_t texels);

        /// Whether @ref SetUnknownFormats names @p internalFormat.
        bool isForcedUnknown(GLint internalFormat) const;

    private:
        std::vector<Answer> mAnswers;                 ///< One per format asked, in order: a handful, so a linear scan beats a map.
        std::vector<FailedAllocation> mFailedSizes;   ///< One per format whose full-size allocation failed: see recordFailedAllocation.
        std::vector<GLint> mRefused;                  ///< SetRefusedFormats' list; checked before mAnswers.
        std::vector<GLint> mFailedAllocations;        ///< SetFailedAllocations' list.
        std::vector<GLint> mUnknown;                  ///< SetUnknownFormats' list; checked after mRefused, before mAnswers.
        std::vector<LoggedSkip> mLoggedSkips;         ///< The skips already logged (isNewSkip), so a walk asking every frame is quiet.
    };

    /// Allocate @p buffer at @p width x @p height in the best of the @p count
    /// @p rows this context may render at that size. THE format walk: every
    /// buffer with a list of formats goes through it - the neon's emission
    /// table, glow coverage and segment tables, gather buffer and two
    /// half-float fields (a list of one), SharedBuffer::BeginWrite, the
    /// flare's and the spotlight's scaled buffers - so the protocol lives in
    /// one place:
    ///   1. ask @p targets for the first row to try (firstToTry, one lookup a
    ///      frame once probed): not refused, and not already failed at this
    ///      size or larger;
    ///   2. resize to it; if that fails, record the failure in @p targets and
    ///      go on to the next row to try, logging "<owner>: <row> <what> target
    ///      unavailable, falling back to <next>. <consequence>" - a format the
    ///      probe passed or could not judge whose full-size allocation failed
    ///      (memory, a size limit);
    ///   3. when no row may be tried or none allocates, return false.
    ///
    /// STATELESS: everything a walk needs to remember is in @p targets - the
    /// verdicts and the failed sizes - so a buffer released between uses
    /// keeps nothing, every walk on a format learns from any walk's failure,
    /// and a format that failed at one size is tried again at a smaller one
    /// (one reallocation, the frame the size drops below the failure). A
    /// buffer already holding the row the walk lands on costs Resize's
    /// early-out. @p Row names internalFormat, format, type and name.
    /// @p owner, @p what and @p consequence are only read: the walk runs every
    /// frame and builds nothing. @p allocated, if given, receives the index of
    /// the row that allocated, and is left alone on failure.
    /// @return false only if no row allocated, in which case @p buffer has no
    ///         attachment at all.
    template <typename Row>
    bool ResizeInBestFormat(Framebuffer &buffer, RenderTargetFormats &targets, const Row *rows, size_t count,
                            int width, int height, GLint filter, int attachments, const char *owner, const char *what,
                            const char *consequence, size_t *allocated)
    {
        // A size Resize refuses without allocating: no format failed, so
        // nothing is recorded.
        if (width <= 0 || height <= 0 || attachments <= 0)
        {
            LOG_E("%s: %s target requested at %d x %d with %d attachment(s) - refused.", owner, what, width, height,
                  attachments);
            return false;
        }
        const int64_t texels = static_cast<int64_t>(width) * height * attachments;
        size_t index = targets.firstToTry(rows, count, 0, texels, owner, what);
        while (index < count)
        {
            const Row &row = rows[index];
            // SetFailedAllocations: fail as Resize fails, destroying what the
            // buffer held - a verification run of this very fallback.
            const bool forced = targets.isAllocationForcedToFail(row.internalFormat);
            if (forced)
            {
                buffer.Release();
            }
            else if (buffer.Resize(width, height, row.internalFormat, row.format, row.type, filter, attachments))
            {
                if (allocated)
                {
                    *allocated = index;
                }
                return true;
            }
            targets.recordFailedAllocation(row.internalFormat, texels);
            targets.isNewSkip(owner, what, row.name); // the line below says it; no skip line for it later
            const size_t next = targets.firstToTry(rows, count, index + 1, texels, owner, what);
            const char *hook = forced ? " (failed by RenderTargetFormats::SetFailedAllocations)" : "";
            if (next < count)
            {
                LOG_E("%s: %s %s target unavailable at %d x %d, falling back to %s. %s%s", owner, row.name, what,
                      width, height, rows[next].name, consequence, hook);
            }
            else
            {
                LOG_E("%s: %s %s target unavailable at %d x %d, and no format is left to try.%s", owner, row.name,
                      what, width, height, hook);
            }
            index = next;
        }
        return false;
    }

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_RENDER_TARGET_FORMATS_H_
