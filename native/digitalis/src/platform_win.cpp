// Windows implementations of the OS services Digitalis' translator core expects from Linux/bionic:
// anonymous memory, the low-2GB executable code pool, logging, clocks and a few libbase helpers.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include "berberis/base/exec_region.h"
#include "berberis/base/maps_snapshot.h"
#include "berberis/base/mmap.h"
#include "berberis/base/tracing.h"
#include "berberis/runtime_primitives/exec_region_anonymous.h"

#include "common.h"
#include "digitalis_core.h"
#include "kernel.h"

namespace {

// Digitalis keeps host code addresses in 32 bits (HostCodeAddr) and addresses its constants pool
// with 32-bit displacements, so both live below 2GB. refract_native's guest arena is far above.
constexpr ULONG_PTR kLowLimit = 0x7fffffff;

DWORD ToWinProtect(int prot) {
  if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
  if (prot & PROT_WRITE) return PAGE_READWRITE;
  if (prot & PROT_READ) return PAGE_READONLY;
  return PAGE_NOACCESS;
}

MEM_EXTENDED_PARAMETER LowAddressParameter(MEM_ADDRESS_REQUIREMENTS* req) {
  *req = {};
  req->LowestStartingAddress = reinterpret_cast<void*>(0x10000);
  req->HighestEndingAddress = reinterpret_cast<void*>(kLowLimit);
  MEM_EXTENDED_PARAMETER param{};
  param.Type = MemExtendedParameterAddressRequirements;
  param.Pointer = req;
  return param;
}

// VirtualAlloc2/MapViewOfFile3 (Windows 10 1803+) live in kernelbase; resolving them here keeps
// refract_native linking against kernel32 only.
template <typename Fn>
Fn KernelBase(const char* name) {
  static HMODULE module = GetModuleHandleW(L"kernelbase.dll");
  Fn fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
  if (!fn) rn::Fatal("digitalis: %s is missing (Windows 10 1803 or later is required)", name);
  return fn;
}

void* VirtualAllocLow(size_t size, DWORD protect) {
  static auto alloc = KernelBase<decltype(&VirtualAlloc2)>("VirtualAlloc2");
  MEM_ADDRESS_REQUIREMENTS req;
  MEM_EXTENDED_PARAMETER param = LowAddressParameter(&req);
  return alloc(nullptr, nullptr, size, MEM_RESERVE | MEM_COMMIT, protect, &param, 1);
}

void* MapViewLow(HANDLE section, size_t size, ULONG protect) {
  static auto map = KernelBase<decltype(&MapViewOfFile3)>("MapViewOfFile3");
  MEM_ADDRESS_REQUIREMENTS req;
  MEM_EXTENDED_PARAMETER param = LowAddressParameter(&req);
  return map(section, GetCurrentProcess(), nullptr, 0, size, 0, protect, &param, 1);
}

HANDLE CreateSection(size_t size, DWORD protect) {
  return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, protect | SEC_COMMIT,
                            static_cast<DWORD>(uint64_t{size} >> 32), static_cast<DWORD>(size),
                            nullptr);
}

}  // namespace

extern "C" {

int getpagesize(void) { return 4096; }
pid_t gettid(void) { return static_cast<pid_t>(GetCurrentThreadId()); }

long syscall(long number, ...) {
  switch (number) {
    case __NR_getpid:
      return static_cast<long>(GetCurrentProcessId());
    case __NR_gettid:
      return static_cast<long>(GetCurrentThreadId());
    default:
      rn::Fatal("digitalis: unsupported host syscall %ld", number);
  }
}

int ftruncate64(int, off64_t) { rn::Fatal("digitalis: ftruncate64 is not available on Windows"); }

void* mmap(void* addr, size_t size, int prot, int flags, int fd, long long offset) {
  if (fd != -1 || !(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED)) {
    rn::Fatal("digitalis: unsupported mmap(%p, %zu, %d, 0x%x, %d, %lld)", addr, size, prot, flags,
              fd, offset);
  }
  void* p = (flags & MAP_32BIT) ? VirtualAllocLow(size, ToWinProtect(prot))
                                : VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT,
                                               ToWinProtect(prot));
  return p ? p : MAP_FAILED;
}

int munmap(void* addr, size_t) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return -1;
  if (mbi.Type == MEM_MAPPED) return UnmapViewOfFile(mbi.AllocationBase) ? 0 : -1;
  return VirtualFree(mbi.AllocationBase, 0, MEM_RELEASE) ? 0 : -1;
}

int mprotect(void* addr, size_t size, int prot) {
  DWORD old;
  return VirtualProtect(addr, size, ToWinProtect(prot), &old) ? 0 : -1;
}

int clock_gettime(clockid_t clock, struct timespec* ts) {
  if (clock == CLOCK_REALTIME) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (uint64_t{ft.dwHighDateTime} << 32 | ft.dwLowDateTime) - 116444736000000000ull;
    ts->tv_sec = static_cast<time_t>(t / 10000000);
    ts->tv_nsec = static_cast<long>(t % 10000000 * 100);
    return 0;
  }
  static const uint64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<uint64_t>(f.QuadPart);
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  uint64_t ticks = static_cast<uint64_t>(now.QuadPart);
  ts->tv_sec = static_cast<time_t>(ticks / freq);
  ts->tv_nsec = static_cast<long>((ticks % freq) * 1000000000ull / freq);
  return 0;
}

// Android logging goes to refract_native's log.
int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
  if (prio < ANDROID_LOG_INFO && rn::g_verbose < 2) return 0;
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  rn::Log("[%s] %s", tag ? tag : "berberis", buf);
  return 0;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return 0;
}

int __android_log_write(int prio, const char* tag, const char* text) {
  return __android_log_print(prio, tag, "%s", text);
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
  char buf[2048] = "";
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
  }
  if (rn::g_digitalis_translating) {
    rn::Log("[digitalis] while translating the region at %s",
            rn::DescribeAddress(rn::g_digitalis_translating).c_str());
  }
  rn::Fatal("[%s] %s%s%s", tag ? tag : "berberis", buf, cond ? " (condition: " : "",
            cond ? cond : "");
}

}  // extern "C"

namespace android::base {

void StringAppendV(std::string* dst, const char* format, va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (n <= 0) return;
  size_t old = dst->size();
  dst->resize(old + n + 1);
  vsnprintf(dst->data() + old, n + 1, format, ap);
  dst->resize(old + n);
}

void StringAppendF(std::string* dst, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  StringAppendV(dst, format, ap);
  va_end(ap);
}

std::string StringPrintf(const char* fmt, ...) {
  std::string s;
  va_list ap;
  va_start(ap, fmt);
  StringAppendV(&s, fmt, ap);
  va_end(ap);
  return s;
}

}  // namespace android::base

namespace berberis {

// --- base/mmap.h ---------------------------------------------------------------------------------

void* MmapImpl(MmapImplArgs args) {
  if (args.berberis_flags & kMmapBerberis32Bit) args.flags |= MAP_32BIT;
  return mmap(args.addr, args.size, args.prot, args.flags, args.fd, args.offset);
}

void* MmapImplOrDie(MmapImplArgs args) {
  void* p = MmapImpl(args);
  if (p == MAP_FAILED) {
    rn::Fatal("digitalis: allocating %zu bytes failed: %lu", args.size, GetLastError());
  }
  return p;
}

void MunmapOrDie(void* ptr, size_t size) {
  if (munmap(ptr, size) != 0) rn::Fatal("digitalis: freeing %p failed: %lu", ptr, GetLastError());
}

void MprotectOrDie(void* ptr, size_t size, int prot) {
  if (mprotect(ptr, size, prot) != 0) {
    rn::Fatal("digitalis: protecting %p (+%zu) failed: %lu", ptr, size, GetLastError());
  }
}

// --- Executable memory ---------------------------------------------------------------------------

// One pagefile section mapped twice, like upstream's memfd: read+execute below 2GB for the code,
// read+write anywhere for installing and patching it (ExecRegion::Detach drops the latter).
ExecRegion ExecRegionAnonymousFactory::TryCreate(size_t size) {
  HANDLE section = CreateSection(size, PAGE_EXECUTE_READWRITE);
  if (!section) return {};
  void* exec = MapViewLow(section, size, PAGE_EXECUTE_READ);
  void* write = exec ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size) : nullptr;
  CloseHandle(section);
  if (!write) {
    if (exec) UnmapViewOfFile(exec);
    return {};
  }
  return ExecRegion(static_cast<uint8_t*>(exec), static_cast<uint8_t*>(write), size);
}

ExecRegion ExecRegionAnonymousFactory::Create(size_t size) {
  ExecRegion region = TryCreate(size);
  if (region.begin() == nullptr) {
    rn::Fatal("digitalis: allocating a %zu-byte code region below 2GB failed: %lu", size,
              GetLastError());
  }
  return region;
}

// --- TableOfTables memory (compat/berberis/runtime_primitives/table_of_tables.h) ------------------

void* CreateFilledSectionOrDie(size_t size, uint64_t value, size_t value_size) {
  HANDLE section = CreateSection(size, PAGE_READWRITE);
  if (!section) rn::Fatal("digitalis: creating a %zu-byte table failed: %lu", size, GetLastError());
  void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size);
  if (!view) rn::Fatal("digitalis: mapping a %zu-byte table failed: %lu", size, GetLastError());
  if (value_size == 8) {
    auto* p = static_cast<uint64_t*>(view);
    for (size_t i = 0; i < size / 8; ++i) p[i] = value;
  } else {
    auto* p = static_cast<uint32_t*>(view);
    for (size_t i = 0; i < size / 4; ++i) p[i] = static_cast<uint32_t>(value);
  }
  UnmapViewOfFile(view);
  return section;
}

void* MapSectionOrDie(void* section, size_t size, bool copy_on_write) {
  void* view = MapViewOfFile(static_cast<HANDLE>(section), copy_on_write ? FILE_MAP_COPY : FILE_MAP_READ,
                             0, 0, size);
  if (!view) rn::Fatal("digitalis: mapping a %zu-byte table failed: %lu", size, GetLastError());
  return view;
}

void UnmapSectionView(void* view) { UnmapViewOfFile(view); }
void CloseSection(void* section) { CloseHandle(static_cast<HANDLE>(section)); }

// --- Tracing / maps (diagnostics upstream reads from /proc; not available here) -------------------

// REFRACT_DIGITALIS_TRACE=1: Digitalis' TRACE output (with BERBERIS_FLAGS=print-irs, the heavy
// optimizer's machine IR) goes to stderr.
int Tracing::fd_ = getenv("REFRACT_DIGITALIS_TRACE") ? 2 : -1;
void Tracing::InitImpl() {}

// Only the lookup below is ever called on it, and that touches no members.
MapsSnapshot* MapsSnapshot::GetInstance() {
  alignas(MapsSnapshot) static char storage[sizeof(MapsSnapshot)];
  return reinterpret_cast<MapsSnapshot*>(storage);
}

std::optional<const MapsSnapshot::Record> MapsSnapshot::GetMappedObjectRecordOrUpdateAndRetry(
    uintptr_t) {
  return std::nullopt;
}

}  // namespace berberis
