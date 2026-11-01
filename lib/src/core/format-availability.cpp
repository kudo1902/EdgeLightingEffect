#include "core/format-availability.h"
#include "gl/framebuffer.h"
#include "util/gl-utils.h"
#include "util/log-util.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace EdgeLighting
{
    namespace
    {
        typedef FormatAvailability::ProbeResult ProbeResult;

        /// Every format in LIBRARY_FORMAT_LISTS once, half float first and
        /// otherwise in the order the lists give them, with the transfer pair
        /// its buffers allocate it with: the startup report's rows - see
        /// LogReport.
        std::vector<TextureFormat> GetReportRows()
        {
            std::vector<TextureFormat> rows;
            for (const FormatList &list : LIBRARY_FORMAT_LISTS)
            {
                for (size_t i = 0; i < list.count; ++i)
                {
                    const TextureFormat &candidate = list.formats[i];
                    const bool listed = std::any_of(rows.begin(), rows.end(), [&](const TextureFormat &row)
                                                    { return row.internalFormat == candidate.internalFormat; });
                    if (!listed)
                    {
                        rows.push_back(candidate);
                    }
                }
            }
            std::stable_partition(rows.begin(), rows.end(),
                                  [](const TextureFormat &row) { return HoldsSubLevels(row.internalFormat); });
            return rows;
        }

        /// What a log line adds when a verification hook gave the answer, so
        /// its log does not read as a device's.
        const char *const REFUSED_BY_HOOK = "refused by FormatAvailability::SetRefusedFormats";
        const char *const UNKNOWN_BY_HOOK = "made unknown by FormatAvailability::SetUnknownFormats";

        /// What a probe's status and upload error say - the one place the
        /// rule lives (see FormatAvailability::ProbeResult for each case's
        /// reason).
        ProbeResult ClassifyProbe(GLenum status, GLenum uploadError)
        {
            if (status == GL_FRAMEBUFFER_COMPLETE)
            {
                return ProbeResult::RENDERABLE;
            }
            if (status != 0 || uploadError == GL_INVALID_ENUM || uploadError == GL_INVALID_VALUE)
            {
                return ProbeResult::REFUSED;
            }
            return ProbeResult::UNKNOWN;
        }

        /// What @p answer says, for a log line: the status by name (or in
        /// hex); the upload's error if it raised one; and @p hook, naming the
        /// verification hook that gave this answer, if one did (nullptr if
        /// the driver did) - REFUSED_BY_HOOK or UNKNOWN_BY_HOOK.
        std::string DescribeAnswer(const FormatAvailability::Answer &answer, const char *hook)
        {
            char text[160];
            const char *statusName = GLUtils::GetFramebufferStatusName(answer.status);
            if (statusName)
            {
                std::snprintf(text, sizeof(text), "%s", statusName);
            }
            else
            {
                std::snprintf(text, sizeof(text), "status 0x%x", answer.status);
            }
            std::string out = text;
            if (answer.uploadError != GL_NO_ERROR)
            {
                std::snprintf(text, sizeof(text), ", the 4 x 4 upload raised 0x%x", answer.uploadError);
                out += text;
            }
            if (hook)
            {
                out += ", ";
                out += hook;
            }
            return out;
        }
    }

    FormatAvailability::Answer FormatAvailability::Ask(GLint internalFormat, GLenum format, GLenum type)
    {
        // The hooks first, and without touching what is kept: clearing them
        // gives back the driver's own answer, probed or not.
        if (isForcedRefused(internalFormat))
        {
            Answer refused;
            refused.internalFormat = internalFormat;
            refused.result = ProbeResult::REFUSED;
            refused.status = GL_FRAMEBUFFER_UNSUPPORTED;
            return refused;
        }
        // Then SetUnknownFormats: the answer a probe whose upload failed for
        // a passing reason gives.
        if (isForcedUnknown(internalFormat))
        {
            Answer unknown;
            unknown.internalFormat = internalFormat;
            unknown.result = ProbeResult::UNKNOWN;
            return unknown;
        }
        for (const Answer &answer : mAnswers)
        {
            if (answer.internalFormat == internalFormat)
            {
                return answer;
            }
        }
        Answer answer;
        answer.internalFormat = internalFormat;
        Framebuffer::IsColorRenderable(internalFormat, format, type, &answer.status, &answer.uploadError);
        answer.result = ClassifyProbe(answer.status, answer.uploadError);
        // Kept whatever it says, an unknown result included: a walk lets the
        // allocation decide such a format, so keeping it condemns nothing, and
        // probing it again (every frame, mid-frame, draining the frame's GL
        // errors) would buy nothing.
        mAnswers.push_back(answer);
        return answer;
    }

    void FormatAvailability::AskReportFormats()
    {
        readLimits();
        for (const TextureFormat &row : GetReportRows())
        {
            Ask(row.internalFormat, row.format, row.type);
        }
    }

    bool FormatAvailability::isForcedRefused(GLint internalFormat) const
    {
        return std::find(mRefused.begin(), mRefused.end(), internalFormat) != mRefused.end();
    }

    bool FormatAvailability::allocateInBestFormat(Framebuffer &buffer, const FormatList &formats, GLint filter,
                                                  const char *fallbackCost, int width, int height, int attachments)
    {
        const char *target = buffer.GetName();
        // A size Resize refuses without allocating: no format failed, so
        // nothing is recorded. Released all the same, as a failed walk
        // promises: what it holds was sized for another frame.
        if (width <= 0 || height <= 0 || attachments <= 0)
        {
            LOG_E("%s: target requested at %d x %d with %d attachment(s) - refused.", target, width, height,
                  attachments);
            buffer.Release();
            return false;
        }
        // Past the context's texture size limit: every row would fail alike,
        // so none is tried and nothing is kept against a format.
        if (isPastSizeLimit(width, height))
        {
            logPastSizeLimit(target, width, height);
            buffer.Release();
            return false;
        }
        const int64_t texels = static_cast<int64_t>(width) * height * attachments;
        size_t index = firstToTry(formats, 0, width, height, attachments, target);
        while (index < formats.count)
        {
            const TextureFormat &candidate = formats.formats[index];
            // Whether Resize below is its early-out - the same test it makes:
            // then nothing is allocated, and nothing is learned.
            const bool held = buffer.Holds(width, height, candidate.internalFormat, filter, attachments);
            // SetFailedAllocations: fail as Resize fails, destroying what the
            // buffer held and reporting the hook's cause - a verification run
            // of this very fallback. Never a buffer that already holds the
            // row: Resize would not allocate.
            const bool forced = !held && takeForcedFailure(candidate.internalFormat, texels, attachments);
            GLenum failure = GL_NO_ERROR;
            if (forced)
            {
                buffer.Release();
                failure = mForcedFailure.error;
            }
            else if (buffer.Resize(width, height, candidate.internalFormat, candidate.format, candidate.type, filter,
                                   attachments, &failure))
            {
                if (!held)
                {
                    recordAllocation(candidate.internalFormat, width, height, attachments, target, candidate.name);
                }
                return true;
            }
            recordFailedAllocation(candidate.internalFormat, width, height, attachments, failure);
            // The line below says it; no skip line for it later.
            recordSkip(target, candidate.name);
            const size_t next = firstToTry(formats, index + 1, width, height, attachments, target);
            const std::string cause = describeFailure(failure);
            const char *hook = forced ? " (failed by FormatAvailability::SetFailedAllocations)" : "";
            if (next < formats.count)
            {
                LOG_E("%s: target unavailable in %s at %d x %d x %d (%s), falling back to %s. %s%s", target,
                      candidate.name, width, height, attachments, cause.c_str(), formats.formats[next].name,
                      fallbackCost, hook);
            }
            else
            {
                LOG_E("%s: target unavailable in %s at %d x %d x %d (%s), and no format is left to try.%s", target,
                      candidate.name, width, height, attachments, cause.c_str(), hook);
            }
            index = next;
        }
        // Nothing allocated - and when no row could even be tried, the buffer
        // still holds an earlier, smaller allocation nobody may draw into
        // again at this size. Give it back, as a failed Resize would have.
        buffer.Release();
        return false;
    }

    bool FormatAvailability::isBelowBestRow(const Framebuffer &buffer, const FormatList &formats)
    {
        // Free while the buffer holds its first row - the case every frame
        // takes once nothing has failed.
        if (!buffer.IsValid() || formats.count == 0 || buffer.GetInternalFormat() == formats.formats[0].internalFormat)
        {
            return false;
        }
        for (size_t held = 1; held < formats.count; ++held)
        {
            if (formats.formats[held].internalFormat == buffer.GetInternalFormat())
            {
                return getBestRow(FormatList(formats.formats, held), buffer.GetWidth(), buffer.GetHeight(),
                                  buffer.GetAttachmentCount()) < held;
            }
        }
        return false;
    }

    size_t FormatAvailability::firstToTry(const FormatList &formats, size_t from, int width, int height,
                                           int attachments, const char *target)
    {
        for (size_t index = from; index < formats.count; ++index)
        {
            const TextureFormat &candidate = formats.formats[index];
            const char *nextFormat = index + 1 < formats.count ? formats.formats[index + 1].name : nullptr;
            const Answer answer = Ask(candidate.internalFormat, candidate.format, candidate.type);
            if (answer.result == ProbeResult::REFUSED)
            {
                logSkipped(target, candidate.name, nextFormat, answer);
                continue;
            }
            const char *failed = failedReason(candidate.internalFormat, width, height, attachments);
            if (failed)
            {
                logSkippedAfterFailure(target, candidate.name, nextFormat, width, height, attachments, failed);
                continue;
            }
            return index;
        }
        return formats.count;
    }

    size_t FormatAvailability::getBestRow(const FormatList &formats, int width, int height, int attachments)
    {
        for (size_t index = 0; index < formats.count; ++index)
        {
            const TextureFormat &candidate = formats.formats[index];
            if (MayAllocate(candidate.internalFormat, candidate.format, candidate.type, width, height, attachments))
            {
                return index;
            }
        }
        return formats.count;
    }

    const char *FormatAvailability::failedReason(GLint internalFormat, int width, int height, int attachments) const
    {
        const int64_t perAttachment = static_cast<int64_t>(width) * height;
        for (const FailedAllocation &failed : mFailedAllocations)
        {
            if (failed.internalFormat != internalFormat)
            {
                continue;
            }
            // Memory runs out by bytes: any allocation at least as large in
            // total repeats it, whatever its shape or attachment count - until
            // the failure expires, memory being something a GL gets back.
            if (failed.outOfMemory && mTime < failed.retryAt && perAttachment * attachments >= failed.texels)
            {
                return "an allocation of it that large ran out of memory here, too recently to try again";
            }
            // Anything else said something about this format at that many
            // attachments, whatever the size: as many or more fail too.
            if (!failed.outOfMemory && attachments >= failed.attachments)
            {
                return "an allocation of it with that many attachments already failed here, for a reason other than "
                       "memory - at any size";
            }
        }
        return nullptr;
    }

    void FormatAvailability::recordFailedAllocation(GLint internalFormat, int width, int height, int attachments,
                                                     GLenum failure)
    {
        // Memory by its total size; anything else by its attachment count
        // alone - size is not what failed it, the texture size limit being
        // checked before any row is tried.
        const bool outOfMemory = failure == GL_OUT_OF_MEMORY;
        const int keyAttachments = outOfMemory ? 0 : attachments;
        const int64_t texels = outOfMemory ? static_cast<int64_t>(width) * height * attachments : 0;
        for (FailedAllocation &failed : mFailedAllocations)
        {
            if (failed.internalFormat != internalFormat || failed.outOfMemory != outOfMemory)
            {
                continue;
            }
            if (!outOfMemory)
            {
                failed.attachments = std::min(failed.attachments, keyAttachments);
                return;
            }
            if (mTime < failed.retryAt)
            {
                // Still armed, so this allocation was smaller than the one
                // that failed: memory is tighter than that said. Kept at the
                // smaller size, the wait started again but not lengthened -
                // one shortage, not a retry.
                failed.texels = std::min(failed.texels, texels);
                failed.retryAt = mTime + failed.wait;
                return;
            }
            // Expired and run out again: a retry that failed. Armed afresh at
            // this size - what failed before is older news - for twice the
            // wait, so a shortage that holds costs fewer and fewer tries.
            const double longest = std::max(OUT_OF_MEMORY_RETRY_MAX_SECONDS, mOutOfMemoryRetrySeconds);
            failed.texels = texels;
            failed.wait = std::min(failed.wait * 2.0, longest);
            failed.retryAt = mTime + failed.wait;
            return;
        }
        FailedAllocation added{internalFormat, outOfMemory, keyAttachments, texels,
                               std::numeric_limits<double>::infinity(), 0.0};
        if (outOfMemory)
        {
            added.wait = mOutOfMemoryRetrySeconds;
            added.retryAt = mTime + added.wait;
        }
        mFailedAllocations.push_back(added);
    }

    void FormatAvailability::recordAllocation(GLint internalFormat, int width, int height, int attachments,
                                              const char *target, const char *format)
    {
        // An expired out-of-memory failure this allocation is as large as:
        // the memory is back. Cleared, so the next shortage waits the first
        // wait again. A smaller one says nothing about the size that failed.
        const int64_t texels = static_cast<int64_t>(width) * height * attachments;
        const auto recovered = std::find_if(mFailedAllocations.begin(), mFailedAllocations.end(),
                                            [&](const FailedAllocation &failed)
                                            {
                                                return failed.internalFormat == internalFormat && failed.outOfMemory &&
                                                       texels >= failed.texels;
                                            });
        if (recovered == mFailedAllocations.end())
        {
            return;
        }
        LOG_I("%s: target allocated in %s at %d x %d x %d - the out-of-memory failure kept against %s (from %lld "
              "texels) has passed.",
              target, format, width, height, attachments, format, static_cast<long long>(recovered->texels));
        mFailedAllocations.erase(recovered);
        // And the skips past this format that shortage logged, every walk's: the
        // next shortage is news, and each walk it sends down gets its line
        // again. Not on any other allocation - a skip for a refusal, or a
        // failure that does not expire, is said once.
        mLoggedSkips.erase(std::remove_if(mLoggedSkips.begin(), mLoggedSkips.end(),
                                          [format](const LoggedSkip &logged) { return logged.format == format; }),
                           mLoggedSkips.end());
    }

    bool FormatAvailability::takeForcedFailure(GLint internalFormat, int64_t texels, int attachments)
    {
        const ForcedFailure &forced = mForcedFailure;
        const bool selected =
            texels >= forced.fromTexels && attachments >= forced.fromAttachments &&
            (forced.count <= 0 || mForcedFailuresTaken < forced.count) &&
            std::find(forced.formats.begin(), forced.formats.end(), internalFormat) != forced.formats.end();
        if (selected)
        {
            ++mForcedFailuresTaken;
        }
        return selected;
    }

    void FormatAvailability::Advance(double seconds)
    {
        mTime += std::max(seconds, 0.0);
    }

    int FormatAvailability::GetMaxTextureSize() const
    {
        if (mMaxTextureSizeOverride > 0 && (mMaxTextureSize <= 0 || mMaxTextureSizeOverride < mMaxTextureSize))
        {
            return mMaxTextureSizeOverride;
        }
        return mMaxTextureSize;
    }

    bool FormatAvailability::isPastSizeLimit(int width, int height) const
    {
        const int limit = GetMaxTextureSize();
        return limit > 0 && (width > limit || height > limit);
    }

    void FormatAvailability::readLimits()
    {
        if (mMaxTextureSize > 0)
        {
            return;
        }
        GLint size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
        mMaxTextureSize = size;
    }

    void FormatAvailability::logPastSizeLimit(const char *target, int width, int height)
    {
        // Once per target, as a skip is: the walk asks every frame.
        if (!recordSkip(target, "size limit"))
        {
            return;
        }
        LOG_W("%s: target refused at %d x %d - past this GL's texture size limit of %d px%s; no format is tried.",
              target, width, height, GetMaxTextureSize(),
              mMaxTextureSizeOverride > 0 && GetMaxTextureSize() == mMaxTextureSizeOverride
                  ? " (set by FormatAvailability::SetMaxTextureSize)"
                  : "");
    }

    std::string FormatAvailability::describeFailure(GLenum failure)
    {
        char text[48];
        switch (failure)
        {
        case GL_OUT_OF_MEMORY:
        {
            return "GL_OUT_OF_MEMORY";
        }
        case GL_INVALID_VALUE:
        {
            return "GL_INVALID_VALUE";
        }
        case GL_INVALID_OPERATION:
        {
            return "GL_INVALID_OPERATION";
        }
        case GL_INVALID_ENUM:
        {
            return "GL_INVALID_ENUM";
        }
        default:
        {
            break;
        }
        }
        const char *status = failure != 0 ? GLUtils::GetFramebufferStatusName(failure) : nullptr;
        if (status)
        {
            return status;
        }
        std::snprintf(text, sizeof(text), "0x%x", failure);
        return text;
    }

    bool FormatAvailability::recordSkip(const char *target, const char *format)
    {
        // Once per target and format: a walk asks every frame. Compared as
        // text, on a path a frame takes only while a format is being skipped.
        for (const LoggedSkip &logged : mLoggedSkips)
        {
            if (logged.target == target && logged.format == format)
            {
                return false;
            }
        }
        mLoggedSkips.push_back(LoggedSkip{target, format});
        return true;
    }

    void FormatAvailability::logSkipped(const char *target, const char *format, const char *nextFormat,
                                         const Answer &answer)
    {
        if (!recordSkip(target, format))
        {
            return;
        }

        // A verification run's refusal is not the driver's: say so, or its log
        // reads as a device without the format.
        const std::string why =
            DescribeAnswer(answer, isForcedRefused(answer.internalFormat) ? REFUSED_BY_HOOK : nullptr);
        if (nextFormat)
        {
            LOG_W("%s: %s is not renderable on this driver (%s), using %s.", target, format, why.c_str(), nextFormat);
        }
        else
        {
            LOG_W("%s: %s, the last in its list, is not renderable on this driver either (%s).", target, format,
                  why.c_str());
        }
    }

    void FormatAvailability::logSkippedAfterFailure(const char *target, const char *format, const char *nextFormat,
                                                     int width, int height, int attachments, const char *reason)
    {
        if (!recordSkip(target, format))
        {
            return;
        }
        if (nextFormat)
        {
            LOG_W("%s: %s skipped at %d x %d x %d - %s; using %s.", target, format, width, height, attachments,
                  reason, nextFormat);
        }
        else
        {
            LOG_W("%s: %s, the last in its list, skipped at %d x %d x %d - %s.", target, format, width, height,
                  attachments, reason);
        }
    }

    bool FormatAvailability::isForcedUnknown(GLint internalFormat) const
    {
        return std::find(mUnknown.begin(), mUnknown.end(), internalFormat) != mUnknown.end();
    }

    void FormatAvailability::LogReport()
    {
        LOG_I("--- Render-target formats ---");
        readLimits();
        const size_t probesBefore = mAnswers.size();
        const auto start = std::chrono::steady_clock::now();
        for (const TextureFormat &row : GetReportRows())
        {
            const Answer answer = Ask(row.internalFormat, row.format, row.type);
            switch (answer.result)
            {
            case ProbeResult::RENDERABLE:
            {
                LOG_I("%-8s: renderable", row.name);
                break;
            }
            case ProbeResult::UNKNOWN:
            {
                LOG_I("%-8s: UNKNOWN (%s) - the probe could not judge it; its allocation decides", row.name,
                      DescribeAnswer(answer, isForcedUnknown(row.internalFormat) ? UNKNOWN_BY_HOOK : nullptr).c_str());
                break;
            }
            case ProbeResult::REFUSED:
            {
                LOG_I("%-8s: NOT renderable (%s)", row.name,
                      DescribeAnswer(answer, isForcedRefused(row.internalFormat) ? REFUSED_BY_HOOK : nullptr).c_str());
                break;
            }
            }
        }
        const double probeMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
        const bool gles = version && std::strncmp(version, "OpenGL ES", 9) == 0;
        const std::vector<std::string> extensions = GLUtils::GetExtensions();
        const auto listed = [&extensions](const char *name)
        {
            return std::find(extensions.begin(), extensions.end(), name) != extensions.end() ? "yes" : "no";
        };
        LOG_I("EXT_color_buffer_half_float: %s, EXT_color_buffer_float: %s, EXT_float_blend: %s, "
              "OES_texture_float_linear: %s%s",
              listed("GL_EXT_color_buffer_half_float"), listed("GL_EXT_color_buffer_float"),
              listed("GL_EXT_float_blend"), listed("GL_OES_texture_float_linear"),
              gles ? "" : " (desktop GL: float targets are core)");
        LOG_I("Largest buffer side: %d px (GL_MAX_TEXTURE_SIZE) - a larger buffer is refused before any format is "
              "tried",
              GetMaxTextureSize());
        // The probes THIS call made - a row already answered, or answered by
        // a hook, adds none.
        LOG_I("%zu probes, %.2f ms", mAnswers.size() - probesBefore, probeMs);
        LOG_I("-----------------------------");
    }

} // namespace EdgeLighting
