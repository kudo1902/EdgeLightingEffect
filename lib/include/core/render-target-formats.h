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
    /// allocations that failed though the probe passed the format, each by
    /// what failed it, so a failure blocks only what it says something about.
    /// Out of memory is kept as the smallest total size (texels x attachments)
    /// that failed per format - memory runs out by bytes, whatever the
    /// buffer's shape or attachment count. Anything else - an incomplete
    /// framebuffer, an error that is not memory - is kept per format AND
    /// attachment count, as the smallest size per attachment that failed: a
    /// driver that will not take two RGBA16F attachments says nothing about
    /// one. A walk skips a format at a kept size or larger and tries it again
    /// below it, so a buffer keeps no memory of its own and one failure at 4K
    /// does not cost a smaller buffer its format for the effect's life. And a
    /// buffer wider or taller than GL_MAX_TEXTURE_SIZE is refused before any
    /// row is tried, recording nothing: that limit is the context's, the same
    /// for every format, and says nothing about a buffer within it.
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
        /// in @p internalFormat: it is within the texture size limit, the
        /// format renders here or its probe gave no answer - then the
        /// allocation decides - and no allocation it would repeat has failed
        /// (see the class comment: out of memory at this total size or
        /// smaller, anything else at this many attachments or fewer and this
        /// size per attachment or smaller). What a buffer asks before it
        /// commits to a format - the neon's fields ask it before they build
        /// their programs, the segment coverage table before it changes mode;
        /// ResizeInBestFormat's walk asks the same of each row.
        bool MayAllocate(GLint internalFormat, GLenum format, GLenum type, int width, int height, int attachments)
        {
            return !isPastSizeLimit(width, height) && Ask(internalFormat, format, type).verdict != Verdict::REFUSED &&
                   failedReason(internalFormat, width, height, attachments) == nullptr;
        }

        /// The largest width or height a buffer may have: GL_MAX_TEXTURE_SIZE,
        /// read by @ref LogReport or @ref AskReportFormats (0 before either -
        /// no limit known, none applied), or @ref SetMaxTextureSize's, if
        /// smaller.
        int GetMaxTextureSize() const;

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

        /// Which full-size allocations @ref SetFailedAllocations fails, and how.
        typedef struct ForcedFailure
        {
            std::vector<GLint> formats;      ///< The formats whose allocations fail; none by default.
            int64_t fromTexels = 0;          ///< Only allocations of at least this many texels x attachments; 0 is every size.
            int fromAttachments = 1;         ///< Only allocations with at least this many attachments.
            GLenum error = GL_OUT_OF_MEMORY; ///< What the failure reports, as Resize would: a GL error or an incomplete status.
        } ForcedFailure;

        /// Verification only - a host never calls this. Every full-size
        /// allocation @ref ResizeInBestFormat makes that @p failure selects -
        /// a format it names, at least its size and attachment count - fails
        /// from now on, though the probe passed it, reporting
        /// @p failure.error: the case the walks' own fallback exists for,
        /// which no driver the project runs on produces. The buffer is
        /// released as a failed Resize leaves it, the failure is kept by its
        /// cause as a real one is (out of memory by total size, anything else
        /// by format and attachment count), and the fallback is logged naming
        /// this hook. A threshold lets a run drive the walk back up: a format
        /// that failed large is tried again, and allocates, below it. Per
        /// effect, set before the allocations it should reach, like
        /// @ref SetRefusedFormats.
        void SetFailedAllocations(ForcedFailure failure) { mForcedFailure = std::move(failure); }

        /// The same, out of memory: every allocation in @p formats of
        /// @p fromTexels or more (width x height x attachments; 0, the
        /// default, is every size), whatever its attachment count.
        void SetFailedAllocations(std::vector<GLint> formats, int64_t fromTexels = 0)
        {
            ForcedFailure failure;
            failure.formats = std::move(formats);
            failure.fromTexels = fromTexels;
            SetFailedAllocations(std::move(failure));
        }

        /// Verification only - a host never calls this. Every buffer wider or
        /// taller than @p size is refused before any row is tried, as on a GL
        /// whose GL_MAX_TEXTURE_SIZE is @p size - smaller than any Mac's, so a
        /// run can reach the limit at a viewport it can capture. Applies only
        /// below the real limit; 0, the default, applies none.
        void SetMaxTextureSize(int size) { mMaxTextureSizeOverride = size; }

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
        /// @p width x @p height x @p attachments - its verdict is not
        /// REFUSED, and no allocation it would repeat has failed
        /// (@ref failedReason); @p count if none may. A refused row is final -
        /// the walk tries nothing in it.
        ///
        /// Logs a warning the first time it skips each row for a walk, naming
        /// @p owner and @p what - refused, or skipped for a failure another
        /// walk (or this one, larger) had, and why - so every buffer's log
        /// says what the next row costs it; a walk asks every frame, so the
        /// skips logged are remembered (by the text of @p owner, @p what and
        /// the row's name). A walk's own failure is not logged again as a
        /// skip: its fallback line said it.
        ///
        /// Cheap enough for every frame, which is how often a walk runs (its
        /// Resize early-out is inside it): a lookup over the handful of
        /// answers and failures kept, no GL call, nothing built.
        template <typename Row>
        size_t firstToTry(const Row *rows, size_t count, size_t from, int width, int height, int attachments,
                          const char *owner, const char *what)
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
                const char *failed = failedReason(row.internalFormat, width, height, attachments);
                if (failed)
                {
                    logSkippedAfterFailure(owner, what, row.name, next, width, height, attachments, failed);
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

        /// One full-size allocation that failed though its format's verdict
        /// let a walk try it, kept by what failed it (see the class comment).
        typedef struct FailedAllocation
        {
            GLint internalFormat;
            /// GL_OUT_OF_MEMORY failures are memory: @c texels is the smallest
            /// total (texels x attachments) that ran out, for any attachment
            /// count, and @c attachments is 0. Any other failure is this format
            /// at this many attachments: @c attachments is the count it failed
            /// with (more fail too) and @c texels the smallest size per
            /// attachment that failed.
            bool outOfMemory;
            int attachments;
            int64_t texels;
        } FailedAllocation;

        /// Why the walk skips @p internalFormat at @p width x @p height x
        /// @p attachments - a failure kept that an allocation this size would
        /// repeat, as a phrase for the log - or nullptr if none does.
        const char *failedReason(GLint internalFormat, int width, int height, int attachments) const;

        /// Keep that an allocation of @p internalFormat at @p width x
        /// @p height x @p attachments failed with @p failure (what Resize
        /// reported): GL_OUT_OF_MEMORY by its total size, anything else by its
        /// attachment count and size per attachment. No walk repeats it at
        /// that size or larger, and one smaller still may: each record keeps
        /// the smallest size that failed.
        void recordFailedAllocation(GLint internalFormat, int width, int height, int attachments, GLenum failure);

        /// Whether @ref SetFailedAllocations fails an allocation of
        /// @p internalFormat at @p texels (x attachments) with
        /// @p attachments: it names the format, and both reach its
        /// thresholds.
        bool isAllocationForcedToFail(GLint internalFormat, int64_t texels, int attachments) const;

        /// What a forced failure reports (@ref ForcedFailure::error).
        GLenum getForcedError() const { return mForcedFailure.error; }

        /// Whether @p width or @p height is past @ref GetMaxTextureSize.
        bool isPastSizeLimit(int width, int height) const;

        /// Read GL_MAX_TEXTURE_SIZE into the object, once.
        /// @pre A GL context is current.
        void readLimits();

        /// The warning a walk writes, once, for a buffer past the texture size
        /// limit: refused before any row is tried, and nothing kept.
        void logPastSizeLimit(const char *owner, const char *what, int width, int height);

        /// What a failure Resize reported means, for a log line: the GL error
        /// or framebuffer status by name, or in hex.
        static std::string describeFailure(GLenum failure);

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

        /// The same, for a row skipped at @p width x @p height x
        /// @p attachments for the failure @p reason (@ref failedReason)
        /// describes - another walk's, or this one's at an earlier size.
        void logSkippedAfterFailure(const char *owner, const char *what, const char *row, const char *next, int width,
                                    int height, int attachments, const char *reason);

        /// Whether @ref SetUnknownFormats names @p internalFormat.
        bool isForcedUnknown(GLint internalFormat) const;

    private:
        std::vector<Answer> mAnswers;                 ///< One per format asked, in order: a handful, so a linear scan beats a map.
        std::vector<FailedAllocation> mFailedSizes;   ///< The full-size failures kept: see recordFailedAllocation.
        std::vector<GLint> mRefused;                  ///< SetRefusedFormats' list; checked before mAnswers.
        ForcedFailure mForcedFailure;                 ///< SetFailedAllocations' selection; fails nothing by default.
        std::vector<GLint> mUnknown;                  ///< SetUnknownFormats' list; checked after mRefused, before mAnswers.
        int mMaxTextureSize = 0;                      ///< GL_MAX_TEXTURE_SIZE, once readLimits has run; 0 before.
        int mMaxTextureSizeOverride = 0;              ///< SetMaxTextureSize's; 0 for none.
        std::vector<LoggedSkip> mLoggedSkips;         ///< The skips already logged (isNewSkip), so a walk asking every frame is quiet.
    };

    /// Allocate @p buffer at @p width x @p height in the best of the @p count
    /// @p rows this context may render at that size. THE format walk: every
    /// buffer with a list of formats goes through it - the neon's emission
    /// table, glow coverage and segment tables, gather buffer and two
    /// half-float fields (a list of one), SharedBuffer::BeginWrite, the
    /// flare's and the spotlight's scaled buffers - so the protocol lives in
    /// one place:
    ///   0. refuse a buffer wider or taller than the texture size limit
    ///      (RenderTargetFormats::GetMaxTextureSize) before trying any row,
    ///      keeping nothing: the limit is the context's, not a format's;
    ///   1. ask @p targets for the first row to try (firstToTry, one lookup a
    ///      frame once probed): not refused, and no failure kept that this
    ///      allocation would repeat;
    ///   2. resize to it; if that fails, keep the failure in @p targets by
    ///      what Resize said failed it and go on to the next row to try,
    ///      logging "<owner>: <row> <what> target unavailable at W x H
    ///      (<cause>), falling back to <next>. <consequence>" - a format the
    ///      probe passed or could not judge whose full-size allocation failed
    ///      (memory, an attachment count the driver will not take);
    ///   3. when no row may be tried or none allocates, return false.
    ///
    /// STATELESS: everything a walk needs to remember is in @p targets - the
    /// verdicts and the failures, each by its cause - so a buffer released
    /// between uses keeps nothing, every walk on a format learns from any
    /// walk's failure that says something about it, and a format that failed
    /// at one size is tried again at a smaller one (one reallocation, the
    /// frame the size drops below the failure). A buffer already holding the
    /// row the walk lands on costs Resize's early-out. @p Row names
    /// internalFormat, format, type and name.
    /// @p owner, @p what and @p consequence are only read: the walk runs every
    /// frame and builds nothing. @p allocated, if given, receives the index of
    /// the row that allocated, and is left alone on failure.
    /// @return false only if no row allocated, in which case @p buffer has no
    ///         attachment at all - a buffer holding an earlier allocation when
    ///         no row may be tried at this size is released, not left holding
    ///         it.
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
        // Past the context's texture size limit: every row would fail alike,
        // so none is tried and nothing is kept against a format.
        if (targets.isPastSizeLimit(width, height))
        {
            targets.logPastSizeLimit(owner, what, width, height);
            buffer.Release();
            return false;
        }
        const int64_t texels = static_cast<int64_t>(width) * height * attachments;
        size_t index = targets.firstToTry(rows, count, 0, width, height, attachments, owner, what);
        while (index < count)
        {
            const Row &row = rows[index];
            // SetFailedAllocations: fail as Resize fails, destroying what the
            // buffer held and reporting the hook's cause - a verification run
            // of this very fallback.
            const bool forced = targets.isAllocationForcedToFail(row.internalFormat, texels, attachments);
            GLenum failure = GL_NO_ERROR;
            if (forced)
            {
                buffer.Release();
                failure = targets.getForcedError();
            }
            else if (buffer.Resize(width, height, row.internalFormat, row.format, row.type, filter, attachments,
                                   &failure))
            {
                if (allocated)
                {
                    *allocated = index;
                }
                return true;
            }
            targets.recordFailedAllocation(row.internalFormat, width, height, attachments, failure);
            targets.isNewSkip(owner, what, row.name); // the line below says it; no skip line for it later
            const size_t next = targets.firstToTry(rows, count, index + 1, width, height, attachments, owner, what);
            const std::string cause = RenderTargetFormats::describeFailure(failure);
            const char *hook = forced ? " (failed by RenderTargetFormats::SetFailedAllocations)" : "";
            if (next < count)
            {
                LOG_E("%s: %s %s target unavailable at %d x %d x %d (%s), falling back to %s. %s%s", owner, row.name,
                      what, width, height, attachments, cause.c_str(), rows[next].name, consequence, hook);
            }
            else
            {
                LOG_E("%s: %s %s target unavailable at %d x %d x %d (%s), and no format is left to try.%s", owner,
                      row.name, what, width, height, attachments, cause.c_str(), hook);
            }
            index = next;
        }
        // Nothing allocated - and when no row could even be tried, the buffer
        // still holds an earlier, smaller allocation nobody may draw into
        // again at this size. Give it back, as a failed Resize would have.
        buffer.Release();
        return false;
    }

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_RENDER_TARGET_FORMATS_H_
