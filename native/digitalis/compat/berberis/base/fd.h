// Windows stand-in for Digitalis' base/fd.h (raw Linux file syscalls). The translator core on
// Windows creates no memfds (see berberis/runtime_primitives/table_of_tables.h here and
// src/platform_win.cpp's ExecRegion), so only the tracing/write helpers do anything.
#ifndef BERBERIS_BASE_FD_H_
#define BERBERIS_BASE_FD_H_

#include <io.h>

#include <cstddef>
#include <cstdint>

#include "berberis/base/logging.h"

namespace berberis {

inline void WriteFullyOrDie(int fd, const void* data, size_t size) {
  auto* curr = static_cast<const char*>(data);
  while (size > 0) {
    int written = _write(fd, curr, static_cast<unsigned>(size));
    LOG_ALWAYS_FATAL_IF(written < 0, "write to fd %d failed", fd);
    curr += written;
    size -= static_cast<size_t>(written);
  }
}

inline void CloseUnsafe(int fd) { _close(fd); }
inline void TagHostOwnedFdUnsafe(int) {}
inline void CloseHostOwnedFdUnsafe(int fd) { _close(fd); }

}  // namespace berberis

#endif  // BERBERIS_BASE_FD_H_
