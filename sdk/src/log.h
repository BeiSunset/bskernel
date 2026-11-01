#ifndef BSKERNEL_LOG_H
#define BSKERNEL_LOG_H

#include <android/log.h>
#include <cstdarg>
#include <cstdio>

namespace bskernel::detail {
inline void log(int priority, const char* format, ...) {
    va_list args, android_args;
    va_start(args, format);
    va_copy(android_args, args);
    std::fputs("[bskernel] ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    __android_log_vprint(priority, "bskernel", format, android_args);
    va_end(android_args);
    va_end(args);
}
} // namespace dfmkernel::detail

#ifndef NDEBUG
#define LOGD(...) ::bskernel::detail::log(ANDROID_LOG_DEBUG, __VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif
#define LOGI(...) ::bskernel::detail::log(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) ::bskernel::detail::log(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) ::bskernel::detail::log(ANDROID_LOG_ERROR, __VA_ARGS__)

#endif
