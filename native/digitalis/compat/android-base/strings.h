// The parts of libbase's android-base/strings.h Digitalis uses.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace android::base {

inline bool StartsWith(std::string_view s, std::string_view prefix) { return s.starts_with(prefix); }
inline bool StartsWith(std::string_view s, char prefix) { return !s.empty() && s.front() == prefix; }
inline bool EndsWith(std::string_view s, std::string_view suffix) { return s.ends_with(suffix); }
inline bool EndsWith(std::string_view s, char suffix) { return !s.empty() && s.back() == suffix; }

inline bool ConsumeSuffix(std::string_view* s, std::string_view suffix) {
  if (!s->ends_with(suffix)) return false;
  s->remove_suffix(suffix.size());
  return true;
}

inline std::vector<std::string> Split(const std::string& s, const std::string& delimiters) {
  std::vector<std::string> result;
  size_t base = 0;
  for (;;) {
    size_t found = s.find_first_of(delimiters, base);
    result.push_back(s.substr(base, found - base));
    if (found == std::string::npos) break;
    base = found + 1;
  }
  return result;
}

}  // namespace android::base
