#ifndef PERFDLFILTERLIB_LOG_H
#define PERFDLFILTERLIB_LOG_H

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

#ifdef DEBUG
#undef DEBUG
#endif
#ifdef ERROR
#undef ERROR
#endif

enum class LogLevel
{
    OFF     = 0,
    ERROR   = 1,
    WARNING = 2,
    INFO    = 3,
    DEBUG   = 4,
    TRACE   = 5
};

class Logger
{
public:
    inline static LogLevel currentLevel = LogLevel::INFO;

    static void SetLevel(LogLevel level)
    {
        currentLevel = level;
    }

    static void SetLevel(const std::string &levelStr)
    {
        std::string s = levelStr;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (s == "error") {
            currentLevel = LogLevel::ERROR;
        } else if (s == "warning" || s == "warn") {
            currentLevel = LogLevel::WARNING;
        } else if (s == "info") {
            currentLevel = LogLevel::INFO;
        } else if (s == "debug") {
            currentLevel = LogLevel::DEBUG;
        } else if (s == "trace") {
            currentLevel = LogLevel::TRACE;
        } else if (s == "off" || s == "none") {
            currentLevel = LogLevel::OFF;
        }
    }

    static LogLevel GetLevel()
    {
        return currentLevel;
    }

    static bool IsEnabled(LogLevel level)
    {
        return currentLevel >= level && currentLevel != LogLevel::OFF;
    }

    static bool IsError()
    {
        return IsEnabled(LogLevel::ERROR);
    }

    static bool IsWarning()
    {
        return IsEnabled(LogLevel::WARNING);
    }

    static bool IsInfo()
    {
        return IsEnabled(LogLevel::INFO);
    }

    static bool IsDebug()
    {
        return IsEnabled(LogLevel::DEBUG);
    }

    static bool IsTrace()
    {
        return IsEnabled(LogLevel::TRACE);
    }

    static const char *LevelToString(LogLevel level)
    {
        switch (level) {
        case LogLevel::OFF:
            return "off";
        case LogLevel::ERROR:
            return "error";
        case LogLevel::WARNING:
            return "warning";
        case LogLevel::INFO:
            return "info";
        case LogLevel::DEBUG:
            return "debug";
        case LogLevel::TRACE:
            return "trace";
        default:
            return "unknown";
        }
    }
};

#define LOG_ERROR                              \
    if (!Logger::IsEnabled(LogLevel::ERROR)) { \
    } else                                     \
        std::cerr << "[ERROR] "
#define LOG_WARNING                              \
    if (!Logger::IsEnabled(LogLevel::WARNING)) { \
    } else                                       \
        std::cout << "[WARNING] "
#define LOG_INFO                              \
    if (!Logger::IsEnabled(LogLevel::INFO)) { \
    } else                                    \
        std::cout << "[INFO] "
#define LOG_DEBUG                              \
    if (!Logger::IsEnabled(LogLevel::DEBUG)) { \
    } else                                     \
        std::cout << "[DEBUG] "
#define LOG_TRACE                              \
    if (!Logger::IsEnabled(LogLevel::TRACE)) { \
    } else                                     \
        std::cout << "[TRACE] "

#endif // PERFDLFILTERLIB_LOG_H
