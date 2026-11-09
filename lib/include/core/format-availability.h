#ifndef _EDGE_LIGHTING_FORMAT_AVAILABILITY_H_
#define _EDGE_LIGHTING_FORMAT_AVAILABILITY_H_

#include "gl/gl-header.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace EdgeLighting
{
    /// One texture format a buffer may be allocated in: the sized internal
    /// format and the transfer pair its allocation passes - what glTexImage2D
    /// and Framebuffer::Resize take - plus a name for the log. A format list is
    /// an array of these, best first (@ref FormatList), and a ROW is a
    /// position in one: the walk speaks of rows (AdaptiveFormatTarget).
    typedef struct TextureFormat
    {
        GLint internalFormat; ///< The sized internal format, e.g. GL_RGBA16F.
        GLenum format;        ///< The pixel format of a transfer, e.g. GL_RGBA.
        GLenum type;          ///< Its component type, e.g. GL_HALF_FLOAT.
        const char *name;     ///< For the log lines a walk writes.
    } TextureFormat;

    /// Whether @p internalFormat stores values between 8-bit levels - a float
    /// format. A buffer in one keeps a gradient unrounded, so whatever reads
    /// it back dithers once; an 8-bit one has already rounded it, so the pass
    /// that writes it dithers too.
    inline bool HoldsSubLevels(GLint internalFormat)
    {
        switch (internalFormat)
        {
        case GL_R16F:
        case GL_RG16F:
        case GL_RGB16F:
        case GL_RGBA16F:
        case GL_R32F:
        case GL_RG32F:
        case GL_RGB32F:
        case GL_RGBA32F:
        case GL_R11F_G11F_B10F:
        {
            return true;
        }
        default:
        {
            return false;
        }
        }
    }

    /// A format list: @c count TextureFormats, best first - what a
    /// AdaptiveFormatTarget walks. It points at the list, which has to outlive
    /// whoever keeps it: one of the library's lists below, or a vector its
    /// owner keeps (SharedBuffer).
    typedef struct FormatList
    {
        const TextureFormat *formats = nullptr; ///< The list, best first.
        size_t count = 0;                       ///< How many.

        constexpr FormatList() = default;
        constexpr FormatList(const TextureFormat *first, size_t size) : formats(first), count(size) {}
        /// From an array, its size counted - never written by hand.
        template <size_t N>
        constexpr FormatList(const TextureFormat (&list)[N]) : formats(list), count(N)
        {
        }
        /// Never from a temporary array: it would not outlive the call.
        template <size_t N>
        FormatList(const TextureFormat (&&list)[N]) = delete;

        /// Whether the list holds @p internalFormat.
        constexpr bool Contains(GLint internalFormat) const
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (formats[i].internalFormat == internalFormat)
                {
                    return true;
                }
            }
            return false;
        }
    } FormatList;

    /// The library's format lists, best first. Every library buffer with a
    /// format list walks one of them, under a name of its own that says why,
    /// and the startup report (FormatAvailability::LogReport) asks every
    /// format in @ref LIBRARY_FORMAT_LISTS - so a list added there is
    /// reported, and probed before anything draws. A format a buffer only
    /// wants at 8 bits needs no list: RGBA8 is mandatory.
    ///
    /// Half float, then 8 bits: for data 8 bits would round or clamp.
    inline constexpr TextureFormat RGBA16F_THEN_RGBA8[] = {
        {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, "RGBA16F"},
        {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "RGBA8"},
    };
    /// The same, two channels.
    inline constexpr TextureFormat RG16F_THEN_RG8[] = {
        {GL_RG16F, GL_RG, GL_HALF_FLOAT, "RG16F"},
        {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, "RG8"},
    };
    /// Half float alone, one channel: for a buffer with no use at 8 bits -
    /// the neon's fields, which shade directly without one.
    inline constexpr TextureFormat R16F_ONLY[] = {{GL_R16F, GL_RED, GL_HALF_FLOAT, "R16F"}};
    /// The same, two channels.
    inline constexpr TextureFormat RG16F_ONLY[] = {{GL_RG16F, GL_RG, GL_HALF_FLOAT, "RG16F"}};
    /// Every list above: what the startup report asks.
    inline constexpr FormatList LIBRARY_FORMAT_LISTS[] = {RGBA16F_THEN_RGBA8, RG16F_THEN_RG8, R16F_ONLY,
                                                          RG16F_ONLY};

    class Framebuffer;

    /// What one effect's GL context can render to. Each format asked about is
    /// probed once (@c Framebuffer::IsColorRenderable: a 4 x 4 texture, a
    /// framebuffer, @c glCheckFramebufferStatus) and its answer kept, so every
    /// later ask is a lookup. Owned by @ref EdgeLightingEffect - one per
    /// effect - and handed to every renderer it registers
    /// (@ref BaseRenderer::GetFormatAvailability), the way the
    /// @ref SharedBufferRegistry is. Two effects on one GL context each have
    /// their own: each probes the formats once, and an allocation failure one
    /// of them keeps is not known to the other, which only costs it the
    /// failed allocation of its own.
    ///
    /// Asked where a format is used: every buffer with a list of formats is
    /// an AdaptiveFormatTarget (core/adaptive-format-target.h), sized by
    /// AdaptiveFormatTarget::Ensure - the format walk, which starts at the
    /// first row this driver may render and falls back on a full-size
    /// allocation the probe did not predict; the neon's half-float fields ask
    /// before their programs are built. The formats the library uses are probed
    /// before anything draws - by the startup report (@ref LogReport) for an
    /// effect's object, by
    /// @ref AskReportFormats when a renderer outside an effect makes its own -
    /// and every answer is kept, so no probe of them lands mid-frame, where it
    /// would clear the frame's pending GL errors.
    ///
    /// It also keeps the other half of what decides a format: the full-size
    /// allocations that failed though the probe passed the format, each by
    /// what failed it, so a failure blocks only what it says something about.
    /// Out of memory is kept as the smallest total size (texels x attachments)
    /// that failed per format - memory runs out by bytes, whatever the
    /// buffer's shape or attachment count - and a walk skips that format at
    /// that size or larger and tries it again below it, so one failure at 4K
    /// does not cost a smaller buffer its format. And only for a while: memory
    /// comes back - a video decoder lets go of its frames - so the failure
    /// expires after @ref OUT_OF_MEMORY_RETRY_SECONDS of frame time
    /// (@ref Advance), the first walk past it tries the format again at any
    /// size, and a retry that runs out again doubles the wait, up to
    /// @ref OUT_OF_MEMORY_RETRY_MAX_SECONDS. An allocation at the failed size
    /// or larger that succeeds clears it. A buffer that fell back past the
    /// format moves back up once it may (AdaptiveFormatTarget::Refresh), so one
    /// passing shortage does not leave the shared buffer at 8 bits for the
    /// effect's life.
    /// Anything else - an incomplete framebuffer, an error that is not memory
    /// - is kept per format AND attachment count, at every size: a driver
    /// that will not take two RGBA16F attachments says nothing about one, and
    /// says the same at any size, so trying again smaller would only fail
    /// again (measured: 7 failed allocations over 12 frames of a growing
    /// rect, whose gather buffer shrinks, when it was kept by size). Size is
    /// not what failed it: a buffer wider or taller than GL_MAX_TEXTURE_SIZE
    /// is refused before any row is tried, recording nothing - that limit is
    /// the context's, the same for every format. Such a failure does not
    /// expire: it is the driver's answer, not its state. Either way a buffer
    /// keeps no memory of its own.
    ///
    /// GL thread only. Creates no GL object until asked, so it can be made
    /// before a context exists. See
    /// docs/design/render-target-format-check-plan.md, section 4.
    class FormatAvailability
    {
    public:
        // --- Constants and types ---------------------------------------------

        /// How long, in seconds of frame time (@ref Advance), an out-of-memory
        /// failure keeps its format off at its size and above before a walk
        /// may try it again. Long enough that a shortage still going does not
        /// cost a failed allocation every few frames; short enough that the
        /// shared buffer is back in half float soon after it passes.
        static constexpr double OUT_OF_MEMORY_RETRY_SECONDS = 2.0;
        /// The longest wait: each retry that runs out of memory again doubles
        /// it, from @ref OUT_OF_MEMORY_RETRY_SECONDS, up to this - so a
        /// shortage that never passes costs one failed allocation every ~8.5
        /// minutes once the doubling has reached it.
        static constexpr double OUT_OF_MEMORY_RETRY_MAX_SECONDS = 512.0;

        /// What the probe says about a format - decided once, in @ref Ask.
        typedef enum class ProbeResult
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
        } ProbeResult;

        /// One format's answer.
        typedef struct Answer
        {
            GLint internalFormat = 0;                  ///< The sized internal format asked about, e.g. GL_RGBA16F.
            ProbeResult result = ProbeResult::UNKNOWN; ///< What the probe says - the only field a decision reads.
            GLenum status = 0;                         ///< glCheckFramebufferStatus; 0 when there is none. For the log.
            GLenum uploadError = GL_NO_ERROR;          ///< What the probe's upload raised, if it failed. For the log.
        } Answer;

        /// One full-size allocation that failed though its format's probe
        /// result let a walk try it, kept by what failed it (see the class
        /// comment).
        typedef struct FailedAllocation
        {
            GLint internalFormat = 0;
            /// GL_OUT_OF_MEMORY failures are memory: @c texels is the smallest
            /// total (texels x attachments) that ran out since the failure was
            /// last armed, for any attachment count, and @c attachments is 0.
            /// Any other failure is this format at this many attachments, at
            /// every size: @c attachments is the fewest it failed with (more
            /// fail too) and @c texels is 0.
            bool outOfMemory = false;
            int attachments = 0;
            int64_t texels = 0;
            /// Out of memory only: the frame time (@ref GetTime) from which it
            /// blocks nothing, and how long its last failure kept it - doubled
            /// by a retry that fails again. Infinity and 0 for any other
            /// failure, which does not expire.
            double retryAt = 0.0;
            double wait = 0.0;
        } FailedAllocation;

        /// Which full-size allocations @ref SetFailedAllocations fails, and how.
        typedef struct ForcedFailure
        {
            std::vector<GLint> formats;      ///< The formats whose allocations fail; none by default.
            int64_t fromTexels = 0;          ///< Only allocations of at least this many texels x attachments; 0 is every size.
            int fromAttachments = 1;         ///< Only allocations with at least this many attachments.
            GLenum error = GL_OUT_OF_MEMORY; ///< What the failure reports, as Resize would: a GL error or an incomplete status.
            int count = 0;                   ///< Only the first this many allocations it selects fail - a shortage that passes; 0 fails every one.
        } ForcedFailure;

        // --- Construction ----------------------------------------------------

        FormatAvailability() = default;
        /// Neither copied nor moved: the effect hands out one object, and a
        /// copy's hooks and failures would reach no walk.
        FormatAvailability(const FormatAvailability &) = delete;
        FormatAvailability &operator=(const FormatAvailability &) = delete;

        // --- Asking about a format -------------------------------------------

        /// The answer for @p internalFormat: probed on the first ask, with
        /// @p format and @p type as the probe's upload, then kept - every
        /// answer, an unknown one included, so a passing failure of the 4 x 4
        /// probe - or a caller's wrong transfer pair - can neither condemn a
        /// format nor make anything probe it again. Keyed by the internal
        /// format alone - renderability is its property; a valid pair only
        /// feeds the upload. A format a verification hook names
        /// (@ref SetRefusedFormats, @ref SetUnknownFormats) comes back with
        /// the hook's result, without a probe and without touching what is
        /// kept.
        /// @pre A GL context is current, if this format has no answer kept.
        Answer Ask(GLint internalFormat, GLenum format, GLenum type);
        /// The same, for one row of a format list.
        Answer Ask(const TextureFormat &format) { return Ask(format.internalFormat, format.format, format.type); }

        /// Whether this driver renders to @p internalFormat - the probe
        /// result alone: false for a format it refused AND for one its probe
        /// could not judge. What a host reports as supported. Non-const, as
        /// @ref Ask is - it probes a format not asked yet - so a host holding
        /// a const effect reads the answers kept instead (@ref GetAnswers).
        bool IsRenderable(GLint internalFormat, GLenum format, GLenum type)
        {
            return Ask(internalFormat, format, type).result == ProbeResult::RENDERABLE;
        }

        /// Whether a buffer may allocate @p width x @p height x @p attachments
        /// in @p internalFormat: it is within the texture size limit, the
        /// format renders here or its probe gave no answer - then the
        /// allocation decides - and no allocation it would repeat has failed
        /// (see the class comment: out of memory at this total size or
        /// smaller, anything else at this many attachments or fewer, at any
        /// size). The walk asks it of each row.
        bool MayAllocate(GLint internalFormat, GLenum format, GLenum type, int width, int height, int attachments)
        {
            return !isPastSizeLimit(width, height) &&
                   Ask(internalFormat, format, type).result != ProbeResult::REFUSED &&
                   failedReason(internalFormat, width, height, attachments) == nullptr;
        }
        /// The same, for one row of a format list.
        bool MayAllocate(const TextureFormat &format, int width, int height, int attachments)
        {
            return MayAllocate(format.internalFormat, format.format, format.type, width, height, attachments);
        }

        /// Whether a walk over @p formats at @p width x @p height x
        /// @p attachments has a row to try: @ref MayAllocate of any of them,
        /// by the walk's own rule. What a buffer asks before it commits to a
        /// list - the neon's fields before they build their programs, the
        /// segment coverage table before it changes mode. Logs nothing.
        bool MayAllocate(const FormatList &formats, int width, int height, int attachments)
        {
            return getBestRow(formats, width, height, attachments) < formats.count;
        }

        // --- Frame time: what an out-of-memory failure expires by ------------

        /// Move frame time on by @p seconds (a negative or non-finite step
        /// counts as 0):
        /// the clock an out-of-memory failure expires by. Called by
        /// @ref EdgeLightingEffect::Update with the host's frame delta - real
        /// time between frames, not the effect's Clock, which a host may
        /// pause while memory comes back - and, for an object a renderer made
        /// for itself outside an effect, by that renderer's Update
        /// (BaseRenderer::AdvanceOwnFormatAvailability). A host that never
        /// calls Update never lets a failure expire.
        void Advance(double seconds);

        /// The frame time @ref Advance has reached, seconds.
        double GetTime() const { return mTime; }

        // --- The startup report ----------------------------------------------

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
        /// The rows are every format in @ref LIBRARY_FORMAT_LISTS, once each,
        /// half float first - the lists every library buffer walks, so a list
        /// added there gets its rows. A host's own SharedBuffer list is not
        /// among them: its formats are answered on their first ask. RGBA8,
        /// which OffscreenCapture alone also takes, is mandatory in GL 3.3 and
        /// GLES 3.0; it is listed so a broken driver shows.
        ///
        /// The extension line is informational: on desktop GL the strings are
        /// absent by design - float targets are core there, as in GLES 3.2 -
        /// and it says so rather than printing four misleading "no"s.
        void LogReport();

        /// Ask every format @ref LogReport lists, without the log: what a
        /// renderer outside an effect does when it makes its own object
        /// (BaseRenderer::GetFormatAvailability), so its walks, too, find their
        /// answers kept instead of probing mid-frame.
        /// @pre A GL context is current.
        void AskReportFormats();

        // --- What it has kept ------------------------------------------------

        /// Every answer kept so far, in the order first asked - so its size is
        /// the number of probes made. A hook's answer is not among them.
        const std::vector<Answer> &GetAnswers() const { return mAnswers; }

        /// Every failure kept, in the order first failed: verification reads
        /// what a run left, as @ref GetAnswers. An expired out-of-memory
        /// failure stays until an allocation that large succeeds.
        const std::vector<FailedAllocation> &GetFailedAllocations() const { return mFailedAllocations; }

        // --- Verification hooks: a host never calls these --------------------

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

        /// Verification only - a host never calls this. Every full-size
        /// allocation an AdaptiveFormatTarget's walk makes that @p failure
        /// selects - a format it names, at least its size and attachment
        /// count - fails from now on, though the probe passed it, reporting
        /// @p failure.error: the case the walks' own fallback exists for,
        /// which no driver the project runs on produces. The buffer is
        /// released as a failed Resize leaves it, the failure is kept by its
        /// cause as a real one is (out of memory by total size, anything else
        /// by format and attachment count), and the fallback is logged naming
        /// this hook. A threshold lets a run drive the walk back up: a format
        /// that failed large is tried again, and allocates, below it. Per
        /// effect, set before the allocations it should reach, like
        /// @ref SetRefusedFormats. With a @c count, only that many fail - a
        /// shortage that passes, which an expired failure must recover from.
        void SetFailedAllocations(ForcedFailure failure)
        {
            mForcedFailure = std::move(failure);
            mForcedFailuresTaken = 0;
        }

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

        /// Verification only - a host never calls this. How long an
        /// out-of-memory failure first keeps its format off, in place of
        /// @ref OUT_OF_MEMORY_RETRY_SECONDS; infinity keeps it off for the
        /// effect's life, as before failures expired - for a run about a
        /// failure that holds, whose frames a retry would only perturb. Set
        /// before the first failure it should reach.
        void SetOutOfMemoryRetry(double firstSeconds) { mOutOfMemoryRetrySeconds = firstSeconds; }

    private:
        /// AdaptiveFormatTarget alone walks a format list
        /// (AdaptiveFormatTarget::Ensure, AdaptiveFormatTarget::Refresh),
        /// through the two methods below.
        friend class AdaptiveFormatTarget;

        // --- Types -----------------------------------------------------------

        /// One skip @ref firstToTry has logged: the target's name and the
        /// format's, as text - a target's name is its Framebuffer's label,
        /// whose storage a later buffer can reuse, so a pointer is no
        /// identity.
        typedef struct LoggedSkip
        {
            std::string target;
            std::string format;
        } LoggedSkip;

        // --- The walk: AdaptiveFormatTarget's way in -------------------------

        /// The format walk AdaptiveFormatTarget::Ensure describes: hold
        /// @p width x @p height x @p attachments in @p buffer, in the best of
        /// @p formats this context may render at that size, with @p filter.
        /// Every log line names the buffer by its label
        /// (Framebuffer::GetName); @p fallbackCost ends a fallback's line.
        /// True if a row allocated or was held; false with the buffer
        /// released.
        bool allocateInBestFormat(Framebuffer &buffer, const FormatList &formats, GLint filter,
                                  const char *fallbackCost, int width, int height, int attachments);

        /// Whether no row of @p formats can be tried at all - a size Resize
        /// refuses, a list with nothing in it, a buffer past the texture size
        /// limit - and if so, said in the log and @p buffer released. Nothing
        /// is kept against a format for any of them.
        bool rejectsOutright(Framebuffer &buffer, const FormatList &formats, int width, int height, int attachments);

        /// Whether @p buffer holds a row of @p formats below the one a walk at
        /// its own size would now start at - what AdaptiveFormatTarget::Refresh
        /// asks. False for a buffer that holds nothing, or a format not in the
        /// list. Free while it holds the first row; otherwise a lookup per row
        /// above it, no GL call, no log.
        bool isBelowBestRow(const Framebuffer &buffer, const FormatList &formats);

        // --- The walk's steps ------------------------------------------------

        /// The walk's question, asked at its allocation: the row (the index)
        /// of the first of @p formats, at or after @p from, that may be
        /// tried at @p width x @p height x @p attachments - its probe result
        /// is not REFUSED, and no allocation it would repeat has failed
        /// (@ref failedReason); @c formats.count if none may. A refused row is
        /// final - the walk tries nothing in it.
        ///
        /// Logs a warning the first time it skips each row for a target,
        /// naming @p target - refused, or skipped for a failure another walk
        /// (or this one, larger) had, and why - so every buffer's log says
        /// what the next row costs it; a walk asks every frame, so the skips
        /// logged are remembered (by the text of the target's and the
        /// format's names). A walk's own failure is not logged again as a
        /// skip: its fallback line said it.
        ///
        /// Cheap enough for every frame, which is how often a walk runs (its
        /// Resize early-out is inside it): a lookup over the handful of
        /// answers and failures kept, no GL call, nothing built.
        size_t firstToTry(const FormatList &formats, size_t from, int width, int height, int attachments,
                          const char *target);

        /// The row - the index - of the first of @p formats that may be
        /// allocated at @p width x @p height x @p attachments
        /// (@ref MayAllocate): the row a walk would start at, or
        /// @c formats.count if none may. Logs nothing.
        size_t getBestRow(const FormatList &formats, int width, int height, int attachments);

        /// Why the walk skips @p internalFormat at @p width x @p height x
        /// @p attachments - a failure kept that an allocation this size would
        /// repeat, and that has not expired, as a phrase for the log - or
        /// nullptr if none does.
        const char *failedReason(GLint internalFormat, int width, int height, int attachments) const;

        /// Keep that an allocation of @p internalFormat at @p width x
        /// @p height x @p attachments failed with @p failure (what Resize
        /// reported). GL_OUT_OF_MEMORY by its total size, for a while: no walk
        /// repeats it at that size or larger until it expires, one smaller
        /// still may, and the record keeps the smallest size that failed. A
        /// failure after it expired is a retry that ran out again: it is armed
        /// afresh at this size, for twice the wait. Anything else by its
        /// attachment count alone, for good: no walk tries the format again
        /// with that many attachments or more, at any size, and the record
        /// keeps the fewest that failed.
        void recordFailedAllocation(GLint internalFormat, int width, int height, int attachments, GLenum failure);

        /// A walk allocated @p internalFormat - named @p format - for
        /// @p target at @p width x @p height x @p attachments - a real
        /// allocation, not Resize's early-out. Clears an expired out-of-memory
        /// failure of the format that this size answers (as large or larger),
        /// saying so, and then forgets every target's logged skip of that
        /// format, so the next shortage is logged again. A lookup over the
        /// failures kept, none usually.
        void recordAllocation(GLint internalFormat, int width, int height, int attachments, const char *target,
                              const char *format);

        /// Record that @p target's skip of the format named @p format is
        /// logged: true if it was not already - the one time to log it. Also
        /// how a walk's own failure marks its format, so the frames after it
        /// do not log it again as a skip.
        bool recordSkip(const char *target, const char *format);

        /// Whether @ref SetFailedAllocations fails this allocation of
        /// @p internalFormat at @p texels (x attachments) with
        /// @p attachments - it names the format, both reach its thresholds,
        /// and its @c count is not used up - and if so, counts it.
        bool takeForcedFailure(GLint internalFormat, int64_t texels, int attachments);

        /// Whether @p width or @p height is past @ref GetMaxTextureSize.
        bool isPastSizeLimit(int width, int height) const;

        /// Read GL_MAX_TEXTURE_SIZE into the object, once.
        /// @pre A GL context is current.
        void readLimits();

        // --- Log lines -------------------------------------------------------

        /// The warning a walk writes, once, for a buffer past the texture size
        /// limit: refused before any row is tried, and nothing kept.
        void logPastSizeLimit(const char *target, int width, int height);

        /// The error a walk writes when an allocation of @p format failed with
        /// @p failure (what Resize reported, or a forced one, @p forced):
        /// falling back to @p nextFormat, whose cost @p fallbackCost says, or,
        /// when @p nextFormat is nullptr, with no format left to try.
        void logFallback(const char *target, const char *format, const char *nextFormat, int width, int height,
                         int attachments, GLenum failure, bool forced, const char *fallbackCost);

        /// The warning @ref firstToTry writes for a refused format it skips,
        /// named @p format, @p nextFormat being the one after it (nullptr
        /// after the last) - the first time only, for this target and format.
        void logSkipped(const char *target, const char *format, const char *nextFormat, const Answer &answer);

        /// The same, for a format skipped at @p width x @p height x
        /// @p attachments for the failure @p reason (@ref failedReason)
        /// describes - another walk's, or this one's at an earlier size.
        void logSkippedAfterFailure(const char *target, const char *format, const char *nextFormat, int width,
                                    int height, int attachments, const char *reason);

        /// What a failure Resize reported means, for a log line: the GL error
        /// or framebuffer status by name, or in hex.
        static std::string describeFailure(GLenum failure);

        /// Whether @ref SetRefusedFormats names @p internalFormat.
        bool isForcedRefused(GLint internalFormat) const;

        /// Whether @ref SetUnknownFormats names @p internalFormat.
        bool isForcedUnknown(GLint internalFormat) const;

    private:
        std::vector<Answer> mAnswers;                                  ///< One per format asked, in order: a handful, so a linear scan beats a map.
        std::vector<FailedAllocation> mFailedAllocations;              ///< The full-size failures kept: see recordFailedAllocation.
        std::vector<GLint> mRefused;                                   ///< SetRefusedFormats' list; checked before mAnswers.
        ForcedFailure mForcedFailure;                                  ///< SetFailedAllocations' selection; fails nothing by default.
        int mForcedFailuresTaken = 0;                                  ///< Allocations mForcedFailure has failed, against its count.
        std::vector<GLint> mUnknown;                                   ///< SetUnknownFormats' list; checked after mRefused, before mAnswers.
        int mMaxTextureSize = 0;                                       ///< GL_MAX_TEXTURE_SIZE, once readLimits has run; 0 before.
        int mMaxTextureSizeOverride = 0;                               ///< SetMaxTextureSize's; 0 for none.
        std::vector<LoggedSkip> mLoggedSkips;                          ///< The skips already logged (recordSkip), so a walk asking every frame is quiet.
        double mTime = 0.0;                                            ///< Frame time, seconds (Advance).
        double mOutOfMemoryRetrySeconds = OUT_OF_MEMORY_RETRY_SECONDS; ///< An out-of-memory failure's first wait; SetOutOfMemoryRetry.
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_FORMAT_AVAILABILITY_H_
