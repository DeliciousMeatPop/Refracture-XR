// The parts of libbase's android-base/macros.h Digitalis uses.
#pragma once
#include <stddef.h>

#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;      \
  void operator=(const TypeName&) = delete
#define DISALLOW_IMPLICIT_CONSTRUCTORS(TypeName) \
  TypeName() = delete;                           \
  DISALLOW_COPY_AND_ASSIGN(TypeName)

template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];
#define arraysize(array) (sizeof(ArraySizeHelper(array)))

#define LIKELY(exp) (__builtin_expect((exp) != 0, true))
#define UNLIKELY(exp) (__builtin_expect((exp) != 0, false))
#define ABI_STRING "x86_64"
#define TEMP_FAILURE_RETRY(exp) (exp)
#define FALLTHROUGH_INTENDED [[fallthrough]]

// Marks values as used (libbase).
template <typename... T>
void UNUSED(const T&...) {}
