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
        // The hook first, and without touching what is kept: clearing it
        // gives back the driver's own answer, probed or not.
        if (IsRefused(internalFormat))
        {
            Answer refused;
            refused.internalFormat = internalFormat;
            refused.status = GL_FRAMEBUFFER_UNSUPPORTED;
            refused.renderable = false;
            return refused;
        }
        // Then SetUnknownFormats, likewise kept apart: the answer a probe
        // whose upload failed for a passing reason gives.
        if (isForcedUnknown(internalFormat))
        {
            Answer unknown;
            unknown.internalFormat = internalFormat;
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
        answer.renderable =
            Framebuffer::IsColorRenderable(internalFormat, format, type, &answer.status, &answer.uploadError);
        // Kept whatever it says. Without a status the upload failed or the
        // status query raised an error: GL_INVALID_ENUM / GL_INVALID_VALUE
        // (the internal format itself rejected) is a "no", anything else
        // (GL_OUT_OF_MEMORY, a transfer pair this GL rejects) unknown, and a
        // walk lets the allocation decide - so keeping it condemns nothing,
        // and probing it again (every frame, mid-frame, draining the frame's
        // GL errors) would buy nothing.
        mAnswers.push_back(answer);
        return answer;
    }

    bool RenderTargetFormats::IsAllocationFailed(GLint internalFormat) const
    {
        return std::find(mFailedAllocations.begin(), mFailedAllocations.end(), internalFormat) !=
               mFailedAllocations.end();
    }

    void RenderTargetFormats::AskReportFormats()
    {
        for (const ReportRow &row : REPORT_ROWS)
        {
            Ask(row.internalFormat, row.format, row.type);
        }
    }

    bool RenderTargetFormats::IsRefused(GLint internalFormat) const
    {
        return std::find(mRefused.begin(), mRefused.end(), internalFormat) != mRefused.end();
    }

    bool RenderTargetFormats::isForcedUnknown(GLint internalFormat) const
    {
        return std::find(mUnknown.begin(), mUnknown.end(), internalFormat) != mUnknown.end();
    }

    void RenderTargetFormats::logSkipped(const char *owner, const char *what, const char *row, const char *next,
                                         const Answer &answer)
    {
        // Once per walk and row: a walk none of whose rows renders resumes on
        // its last one every frame. Compared as text, on a path a frame takes
        // only while a row is being skipped.
        for (const LoggedSkip &logged : mLoggedSkips)
        {
            if (logged.owner == owner && logged.what == what && logged.row == row)
            {
                return;
            }
        }
        mLoggedSkips.push_back(LoggedSkip{owner, what, row});

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

    void RenderTargetFormats::LogReport()
    {
        LOG_I("--- Render-target formats ---");
        const size_t probesBefore = mAnswers.size();
        const auto start = std::chrono::steady_clock::now();
        for (const ReportRow &row : REPORT_ROWS)
        {
            const Answer answer = Ask(row.internalFormat, row.format, row.type);
            if (answer.renderable)
            {
                LOG_I("%-8s: renderable", row.name);
                continue;
            }
            if (answer.IsUnknown())
            {
                LOG_I("%-8s: UNKNOWN (%s) - the probe could not judge it; its allocation decides", row.name,
                      DescribeAnswer(answer, isForcedUnknown(row.internalFormat) ? UNKNOWN_BY_HOOK : nullptr).c_str());
                continue;
            }
            LOG_I("%-8s: NOT renderable (%s)", row.name,
                  DescribeAnswer(answer, IsRefused(row.internalFormat) ? REFUSED_BY_HOOK : nullptr).c_str());
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
        // The probes THIS call made - a row already answered, or refused by
        // the hook, adds none.
        LOG_I("%zu probes, %.2f ms", mAnswers.size() - probesBefore, probeMs);
        LOG_I("-----------------------------");
    }

} // namespace EdgeLighting
