// Force-included into every Digitalis file (/FI). Fills in what clang + libc++/libstdc++ on Linux
// provide and clang-cl + MSVC's STL don't.
#pragma once

#include <limits>
#include <time.h>
#include <type_traits>

#include "posix_compat.h"

// MSVC's STL doesn't count __int128 as an integer type; Digitalis' intrinsics need it to.
namespace std {
template <> inline constexpr bool is_integral_v<__int128> = true;
template <> inline constexpr bool is_integral_v<unsigned __int128> = true;
template <> struct is_integral<__int128> : true_type {};
template <> struct is_integral<unsigned __int128> : true_type {};
template <> inline constexpr bool is_arithmetic_v<__int128> = true;
template <> inline constexpr bool is_arithmetic_v<unsigned __int128> = true;
template <> inline constexpr bool is_signed_v<__int128> = true;
template <> inline constexpr bool is_signed_v<unsigned __int128> = false;
template <> inline constexpr bool is_unsigned_v<__int128> = false;
template <> inline constexpr bool is_unsigned_v<unsigned __int128> = true;
template <> inline constexpr bool is_scalar_v<__int128> = true;
template <> inline constexpr bool is_scalar_v<unsigned __int128> = true;
template <> struct is_signed<__int128> : true_type {};
template <> struct is_signed<unsigned __int128> : false_type {};
template <> struct is_unsigned<__int128> : false_type {};
template <> struct is_unsigned<unsigned __int128> : true_type {};
template <> struct make_signed<__int128> { using type = __int128; };
template <> struct make_signed<unsigned __int128> { using type = __int128; };
template <> struct make_unsigned<__int128> { using type = unsigned __int128; };
template <> struct make_unsigned<unsigned __int128> { using type = unsigned __int128; };

template <>
class numeric_limits<unsigned __int128> : public _Num_int_base {
 public:
  static constexpr unsigned __int128 min() noexcept { return 0; }
  static constexpr unsigned __int128 max() noexcept { return ~static_cast<unsigned __int128>(0); }
  static constexpr unsigned __int128 lowest() noexcept { return min(); }
  static constexpr unsigned __int128 epsilon() noexcept { return 0; }
  static constexpr unsigned __int128 round_error() noexcept { return 0; }
  static constexpr unsigned __int128 denorm_min() noexcept { return 0; }
  static constexpr unsigned __int128 infinity() noexcept { return 0; }
  static constexpr unsigned __int128 quiet_NaN() noexcept { return 0; }
  static constexpr unsigned __int128 signaling_NaN() noexcept { return 0; }
  static constexpr bool is_signed = false;
  static constexpr bool is_modulo = true;
  static constexpr int digits = 128;
  static constexpr int digits10 = 38;
};
template <>
class numeric_limits<__int128> : public _Num_int_base {
 public:
  static constexpr __int128 max() noexcept { return static_cast<__int128>(~static_cast<unsigned __int128>(0) >> 1); }
  static constexpr __int128 min() noexcept { return -max() - 1; }
  static constexpr __int128 lowest() noexcept { return min(); }
  static constexpr __int128 epsilon() noexcept { return 0; }
  static constexpr __int128 round_error() noexcept { return 0; }
  static constexpr __int128 denorm_min() noexcept { return 0; }
  static constexpr __int128 infinity() noexcept { return 0; }
  static constexpr __int128 quiet_NaN() noexcept { return 0; }
  static constexpr __int128 signaling_NaN() noexcept { return 0; }
  static constexpr bool is_signed = true;
  static constexpr bool is_modulo = false;
  static constexpr int digits = 127;
  static constexpr int digits10 = 38;
};
}  // namespace std

typedef __int128 __int128_t;
typedef unsigned __int128 __uint128_t;

#ifdef __cplusplus
extern "C" {
#endif
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_COARSE 6
int clock_gettime(clockid_t clock, struct timespec* ts);
#ifdef __cplusplus
}
#endif
