#ifndef _EDGE_LIGHTING_LOG_UTIL_H_
#define _EDGE_LIGHTING_LOG_UTIL_H_

#include <iostream>
#include <mutex>
#include <thread>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace EdgeLighting
{
    namespace Util
    {
        typedef enum class Level
        {
            DEBUG,
            INFO,
            WARN,
            ERROR
        } Level;

        inline const char *LevelToString(Level level)
        {
            switch (level)
            {
            case Level::DEBUG:
            {
                return "DEBUG";
            }
            case Level::INFO:
            {
                return "INFO";
            }
            case Level::WARN:
            {
                return "WARN";
            }
            case Level::ERROR:
            {
                return "ERROR";
            }
            default:
            {
                return "UNKNOWN";
            }
            }
        }

        inline const char *GetFileName(const char *path)
        {
            const char *sep = std::strrchr(path, '/');
            return sep ? sep + 1 : path;
        }

        inline std::string FormatString(const char *fmt, ...)
        {
            va_list args;
            va_start(args, fmt);

            va_list argsCopy;
            va_copy(argsCopy, args);

            int len = std::vsnprintf(nullptr, 0, fmt, argsCopy);
            va_end(argsCopy);

            std::string result(len, '\0');
            std::vsnprintf(&result[0], len + 1, fmt, args);
            va_end(args);

            return result;
        }

        inline void Print(Level level, const char *file, const char *func, int line, const std::string &message)
        {
            // One lock around the whole line, for two reasons.
            //
            // Correctness: six unsynchronised operator<< calls on one
            // std::cout from two threads is a data race on the stream's own
            // state, not merely interleaved output. ThreadSanitizer reports it
            // as one, and it was the ONLY thing it found once the C ABI took
            // its data lock - every effect and animation call logs, and the
            // render thread logs without that lock by design.
            //
            // Legibility: without it two threads' lines interleave mid-token,
            // which makes the log useless for the exact case it matters most -
            // working out what a threaded host did and in what order. The
            // thread id in the prefix only helps if lines stay whole.
            //
            // Function-local static, so this stays header-only and there is
            // still exactly one mutex across every translation unit.
            static std::mutex sPrintMutex;
            std::lock_guard<std::mutex> lock(sPrintMutex);
            std::cout << "[Thread:" << std::this_thread::get_id() << "]["
                      << LevelToString(level) << "] "
                      << GetFileName(file) << ": "
                      << func << "(" << line << ") > "
                      << message << "\n";
        }

    } // namespace Util
} // namespace EdgeLighting

#define LOG_D(fmt, ...) EdgeLighting::Util::Print(EdgeLighting::Util::Level::DEBUG, __FILE__, __FUNCTION__, __LINE__, EdgeLighting::Util::FormatString(fmt, ##__VA_ARGS__))
#define LOG_I(fmt, ...) EdgeLighting::Util::Print(EdgeLighting::Util::Level::INFO, __FILE__, __FUNCTION__, __LINE__, EdgeLighting::Util::FormatString(fmt, ##__VA_ARGS__))
#define LOG_W(fmt, ...) EdgeLighting::Util::Print(EdgeLighting::Util::Level::WARN, __FILE__, __FUNCTION__, __LINE__, EdgeLighting::Util::FormatString(fmt, ##__VA_ARGS__))
#define LOG_E(fmt, ...) EdgeLighting::Util::Print(EdgeLighting::Util::Level::ERROR, __FILE__, __FUNCTION__, __LINE__, EdgeLighting::Util::FormatString(fmt, ##__VA_ARGS__))

#endif // _EDGE_LIGHTING_LOG_UTIL_H_
