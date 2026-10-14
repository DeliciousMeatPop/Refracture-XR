// liblog's macros on top of the stderr logger in src/platform_win.cpp.
#pragma once
#include <android/log.h>

#ifdef __cplusplus
extern "C" {
#endif
[[noreturn]] void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...);
#ifdef __cplusplus
}
#endif

#ifndef LOG_TAG
#define LOG_TAG nullptr
#endif

#define android_printLog(prio, tag, ...) __android_log_print(prio, tag, __VA_ARGS__)
#define LOG_PRI(priority, tag, ...) android_printLog(priority, tag, __VA_ARGS__)
#define ALOG(priority, tag, ...) LOG_PRI(ANDROID_##priority, tag, __VA_ARGS__)

#define LOG_ALWAYS_FATAL(...) __android_log_assert(nullptr, LOG_TAG, __VA_ARGS__)
#define LOG_ALWAYS_FATAL_IF(cond, ...) \
  ((__builtin_expect(!!(cond), 0)) ? __android_log_assert(#cond, LOG_TAG, __VA_ARGS__) : (void)0)
#ifdef NDEBUG
#define LOG_FATAL_IF(cond, ...) ((void)0)
#define LOG_FATAL(...) ((void)0)
#else
#define LOG_FATAL_IF(cond, ...) LOG_ALWAYS_FATAL_IF(cond, __VA_ARGS__)
#define LOG_FATAL(...) LOG_ALWAYS_FATAL(__VA_ARGS__)
#endif
#define ALOG_ASSERT(cond, ...) LOG_FATAL_IF(!(cond), __VA_ARGS__)

#define ALOGV(...) ((void)0)
#define ALOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define ALOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define ALOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define ALOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#define ALOGW_IF(cond, ...) ((cond) ? ALOGW(__VA_ARGS__) : (void)0)
#define ALOGE_IF(cond, ...) ((cond) ? ALOGE(__VA_ARGS__) : (void)0)
