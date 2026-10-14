#pragma once
#include <stdarg.h>
#include <string>
namespace android::base {
std::string StringPrintf(const char* fmt, ...);
void StringAppendF(std::string* dst, const char* fmt, ...);
void StringAppendV(std::string* dst, const char* format, va_list ap);
}  // namespace android::base
