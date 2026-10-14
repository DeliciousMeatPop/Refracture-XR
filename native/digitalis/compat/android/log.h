// Android logging -> stderr (src/platform_win.cpp).
#pragma once
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { ANDROID_LOG_UNKNOWN, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
       ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT };
int __android_log_print(int prio, const char* tag, const char* fmt, ...);
int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap);
int __android_log_write(int prio, const char* tag, const char* text);
#ifdef __cplusplus
}
#endif
