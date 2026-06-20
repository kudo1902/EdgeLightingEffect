#ifndef _EDGE_LIGHTING_LOG_UTIL_H_
#define _EDGE_LIGHTING_LOG_UTIL_H_

#include <iostream>
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
