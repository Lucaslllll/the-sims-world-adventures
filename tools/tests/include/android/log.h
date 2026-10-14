#pragma once
#include <cstdarg>
#include <cstdio>
enum { ANDROID_LOG_DEBUG = 3, ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5, ANDROID_LOG_ERROR = 6 };
inline int __android_log_print(int, const char* tag, const char* format, ...) {
    std::fprintf(stderr, "%s: ", tag);
    va_list args;
    va_start(args, format);
    int result = std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    return result;
}
