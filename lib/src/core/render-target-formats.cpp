#include "core/render-target-formats.h"
#include "gl/framebuffer.h"
#include "util/gl-utils.h"
#include "util/log-util.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace EdgeLighting
{
    namespace
    {
        typedef RenderTargetFormats::Verdict Verdict;

        /// One format the startup report lists, with the transfer pair its
        /// buffers allocate it with.
        typedef struct ReportRow
        {
            GLint internalFormat;
            GLenum format;
            GLenum type;
            const char *name;
        } ReportRow;

        /// Every format a library buffer asks for - see LogReport.
        const ReportRow REPORT_ROWS[] = {
            {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, "RGBA16F"},
            {GL_RG16F, GL_RG, GL_HALF_FLOAT, "RG16F"},
            {GL_R16F, GL_RED, GL_HALF_FLOAT, "R16F"},
            {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "RGBA8"},
            {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, "RG8"},
        };

        /// What a log line adds when a verification hook gave the answer, so
        /// its log does not read as a device's.
        const char *const REFUSED_BY_HOOK = "refused by RenderTargetFormats::SetRefusedFormats";
        const char *const UNKNOWN_BY_HOOK = "made unknown by RenderTargetFormats::SetUnknownFormats";

        /// The verdict a probe's results give - the one place the rule lives
        /// (see RenderTargetFormats::Verdict for each case's reason).
        Verdict ClassifyProbe(GLenum status, GLenum uploadError)
        {
            if (status == GL_FRAMEBUFFER_COMPLETE)
            {
                return Verdict::RENDERABLE;
            }
            if (status != 0 || uploadError == GL_INVALID_ENUM || uploadError == GL_INVALID_VALUE)
            {
                return Verdict::REFUSED;
            }
            return Verdict::UNKNOWN;
        }

        /// What @p answer says, for a log line: the status by name (or in
        /// hex); the upload's error if it raised one; and @p hook, naming the
        /// verification hook that gave this answer, if one did (nullptr if
        /// the driver did) - REFUSED_BY_HOOK or UNKNOWN_BY_HOOK.
        std::string DescribeAnswer(const RenderTargetFormats::Answer &answer, const char *hook)
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

    RenderTargetFormats::Answer RenderTargetFormats::Ask(GLint internalFormat, GLenum format, GLenum type)
    {
        // The hooks first, and without touching what is kept: clearing them
        // gives back the driver's own answer, probed or not.
        if (IsRefused(internalFormat))
        {
            Answer refused;
            refused.internalFormat = internalFormat;
            refused.verdict = Verdict::REFUSED;
            refused.status = GL_FRAMEBUFFER_UNSUPPORTED;
            return refused;
        }
        // Then SetUnknownFormats: the answer a probe whose upload failed for
        // a passing reason gives.
        if (isForcedUnknown(internalFormat))
        {
            Answer unknown;
            unknown.internalFormat = internalFormat;
            unknown.verdict = Verdict::UNKNOWN;
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
        answer.verdict = ClassifyProbe(answer.status, answer.uploadError);
        // Kept whatever it says, an unknown verdict included: a walk lets the
        // allocation decide such a format, so keeping it condemns nothing, and
        // probing it again (every frame, mid-frame, draining the frame's GL
        // errors) would buy nothing.
        mAnswers.push_back(answer);
        return answer;
    }

    void RenderTargetFormats::AskReportFormats()
    {
        readLimits();
        for (const ReportRow &row : REPORT_ROWS)
        {
            Ask(row.internalFormat, row.format, row.type);
        }
    }

    bool RenderTargetFormats::IsRefused(GLint internalFormat) const
    {
        return std::find(mRefused.begin(), mRefused.end(), internalFormat) != mRefused.end();
    }

    const char *RenderTargetFormats::failedReason(GLint internalFormat, int width, int height, int attachments) const
    {
        const int64_t perAttachment = static_cast<int64_t>(width) * height;
        for (const FailedAllocation &failed : mFailedSizes)
        {
            if (failed.internalFormat != internalFormat)
            {
                continue;
            }
            // Memory runs out by bytes: any allocation at least as large in
            // total repeats it, whatever its shape or attachment count.
            if (failed.outOfMemory && perAttachment * attachments >= failed.texels)
            {
                return "an allocation of it that large already ran out of memory here";
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

    void RenderTargetFormats::recordFailedAllocation(GLint internalFormat, int width, int height, int attachments,
                                                     GLenum failure)
    {
        // Memory by its total size; anything else by its attachment count
        // alone - size is not what failed it, the texture size limit being
        // checked before any row is tried.
        const bool outOfMemory = failure == GL_OUT_OF_MEMORY;
        const int keyAttachments = outOfMemory ? 0 : attachments;
        const int64_t texels = outOfMemory ? static_cast<int64_t>(width) * height * attachments : 0;
        for (FailedAllocation &failed : mFailedSizes)
        {
            if (failed.internalFormat == internalFormat && failed.outOfMemory == outOfMemory)
            {
                failed.texels = std::min(failed.texels, texels);
                failed.attachments = std::min(failed.attachments, keyAttachments);
                return;
            }
        }
        mFailedSizes.push_back(FailedAllocation{internalFormat, outOfMemory, keyAttachments, texels});
    }

    bool RenderTargetFormats::isAllocationForcedToFail(GLint internalFormat, int64_t texels, int attachments) const
    {
        const ForcedFailure &forced = mForcedFailure;
        return texels >= forced.fromTexels && attachments >= forced.fromAttachments &&
               std::find(forced.formats.begin(), forced.formats.end(), internalFormat) != forced.formats.end();
    }

    int RenderTargetFormats::GetMaxTextureSize() const
    {
        if (mMaxTextureSizeOverride > 0 && (mMaxTextureSize <= 0 || mMaxTextureSizeOverride < mMaxTextureSize))
        {
            return mMaxTextureSizeOverride;
        }
        return mMaxTextureSize;
    }

    bool RenderTargetFormats::isPastSizeLimit(int width, int height) const
    {
        const int limit = GetMaxTextureSize();
        return limit > 0 && (width > limit || height > limit);
    }

    void RenderTargetFormats::readLimits()
    {
        if (mMaxTextureSize > 0)
        {
            return;
        }
        GLint size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
        mMaxTextureSize = size;
    }

    void RenderTargetFormats::logPastSizeLimit(const char *owner, const char *what, int width, int height)
    {
        // Once per walk, as a skip is: the walk asks every frame.
        if (!isNewSkip(owner, what, "size limit"))
        {
            return;
        }
        LOG_W("%s: %s target refused at %d x %d - past this GL's texture size limit of %d px%s; no format is tried.",
              owner, what, width, height, GetMaxTextureSize(),
              mMaxTextureSizeOverride > 0 && GetMaxTextureSize() == mMaxTextureSizeOverride
                  ? " (set by RenderTargetFormats::SetMaxTextureSize)"
                  : "");
    }

    std::string RenderTargetFormats::describeFailure(GLenum failure)
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

    bool RenderTargetFormats::isNewSkip(const char *owner, const char *what, const char *row)
    {
        // Once per walk and row: a walk asks every frame. Compared as text, on
        // a path a frame takes only while a row is being skipped.
        for (const LoggedSkip &logged : mLoggedSkips)
        {
            if (logged.owner == owner && logged.what == what && logged.row == row)
            {
                return false;
            }
        }
        mLoggedSkips.push_back(LoggedSkip{owner, what, row});
        return true;
    }

    void RenderTargetFormats::logSkipped(const char *owner, const char *what, const char *row, const char *next,
                                         const Answer &answer)
    {
        if (!isNewSkip(owner, what, row))
        {
            return;
        }

        // A verification run's refusal is not the driver's: say so, or its log
        // reads as a device without the format.
        const std::string why = DescribeAnswer(answer, IsRefused(answer.internalFormat) ? REFUSED_BY_HOOK : nullptr);
        if (next)
        {
            LOG_W("%s: %s %s target is not renderable on this driver (%s), using %s.", owner, row, what, why.c_str(),
                  next);
        }
        else
        {
            LOG_W("%s: %s %s target, the last in its list, is not renderable on this driver either (%s).", owner, row,
                  what, why.c_str());
        }
    }

    void RenderTargetFormats::logSkippedAfterFailure(const char *owner, const char *what, const char *row,
                                                     const char *next, int width, int height, int attachments,
                                                     const char *reason)
    {
        if (!isNewSkip(owner, what, row))
        {
            return;
        }
        if (next)
        {
            LOG_W("%s: %s %s target skipped at %d x %d x %d - %s; using %s.", owner, row, what, width, height,
                  attachments, reason, next);
        }
        else
        {
            LOG_W("%s: %s %s target, the last in its list, skipped at %d x %d x %d - %s.", owner, row, what, width,
                  height, attachments, reason);
        }
    }

    bool RenderTargetFormats::isForcedUnknown(GLint internalFormat) const
    {
        return std::find(mUnknown.begin(), mUnknown.end(), internalFormat) != mUnknown.end();
    }

    void RenderTargetFormats::LogReport()
    {
        LOG_I("--- Render-target formats ---");
        readLimits();
        const size_t probesBefore = mAnswers.size();
        const auto start = std::chrono::steady_clock::now();
        for (const ReportRow &row : REPORT_ROWS)
        {
            const Answer answer = Ask(row.internalFormat, row.format, row.type);
            switch (answer.verdict)
            {
            case Verdict::RENDERABLE:
            {
                LOG_I("%-8s: renderable", row.name);
                break;
            }
            case Verdict::UNKNOWN:
            {
                LOG_I("%-8s: UNKNOWN (%s) - the probe could not judge it; its allocation decides", row.name,
                      DescribeAnswer(answer, isForcedUnknown(row.internalFormat) ? UNKNOWN_BY_HOOK : nullptr).c_str());
                break;
            }
            case Verdict::REFUSED:
            {
                LOG_I("%-8s: NOT renderable (%s)", row.name,
                      DescribeAnswer(answer, IsRefused(row.internalFormat) ? REFUSED_BY_HOOK : nullptr).c_str());
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
