/**
 * OMTLogging.h - central logging with an optional intercept callback
 * Port of libomtnet/src/OMTLogging.cs (log callback: upstream ca95e4c)
 *
 * One place decides where a log line goes:
 *   - an application-supplied callback (omt_setlogcallback), if installed, is
 *     the ONLY sink and receives the formatted message — the same string the
 *     default sink would print, without the level decoration;
 *   - otherwise the default sink: logcat on Android, stderr everywhere else,
 *     in the "[omt][I] message" format libomtcpp has always printed.
 *
 * All writes are serialized by one mutex, so lines never interleave and a
 * callback never runs concurrently with itself. Do NOT call back into OMT
 * functions from the callback — several call sites log while holding
 * internal locks.
 */

#ifndef OMT_LOGGING_H
#define OMT_LOGGING_H

#include <cstdarg>
#include <cstdio>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace omt {
namespace logging {

enum class Level : int {
    Info = 0,
    Warn = 1,
    Error = 2,
    Debug = 3
};

typedef void (*Callback)(const char* line, void* userData);

inline Callback g_callback = nullptr;
inline void* g_callbackUserData = nullptr;
inline std::mutex g_mutex;

inline void setCallback(Callback callback, void* userData) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_callback = callback;
    g_callbackUserData = userData;
}

inline void write(Level level, const char* fmt, ...) {
    static const char kLevelChar[] = {'I', 'W', 'E', 'D'};

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    // n >= sizeof(buf) means the message was truncated to fit; buf stays NUL-
    // terminated either way.

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_callback) {
        g_callback(buf, g_callbackUserData);
        return;
    }
#ifdef __ANDROID__
    static const int kPriority[] = {ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                    ANDROID_LOG_ERROR, ANDROID_LOG_DEBUG};
    __android_log_print(kPriority[static_cast<int>(level)], "libomt", "%s", buf);
#else
    std::fprintf(stderr, "[omt][%c] %s\n", kLevelChar[static_cast<int>(level)], buf);
#endif
}

} // namespace logging
} // namespace omt

#define OMT_LOGI(...) ::omt::logging::write(::omt::logging::Level::Info, __VA_ARGS__)
#define OMT_LOGW(...) ::omt::logging::write(::omt::logging::Level::Warn, __VA_ARGS__)
#define OMT_LOGE(...) ::omt::logging::write(::omt::logging::Level::Error, __VA_ARGS__)
#define OMT_LOGD(...) ::omt::logging::write(::omt::logging::Level::Debug, __VA_ARGS__)

// Map the per-file LOG* macros every module already uses onto the central
// logger. Guarded so a translation unit (or a consumer's header) that defined
// its own keeps its own.
#ifndef LOGI
#define LOGI OMT_LOGI
#endif
#ifndef LOGW
#define LOGW OMT_LOGW
#endif
#ifndef LOGE
#define LOGE OMT_LOGE
#endif
#ifndef LOGD
#define LOGD OMT_LOGD
#endif

#endif // OMT_LOGGING_H
