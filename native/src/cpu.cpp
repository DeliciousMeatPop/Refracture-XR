// Picks the CpuCore backend: REFRACT_CPU=dynarmic (default) or digitalis.
#include <cstdlib>
#include <cstring>

#include "cpu.h"

namespace rn {
namespace {

enum class Backend { Dynarmic, Digitalis };
Backend g_backend = Backend::Dynarmic;

}  // namespace

void CpuInit(size_t max_cores) {
    const char* name = getenv("REFRACT_CPU");
    if (name && !strcmp(name, "digitalis")) {
#ifdef RN_HAVE_DIGITALIS
        g_backend = Backend::Digitalis;
#else
        Fatal("REFRACT_CPU=digitalis, but this refract_native was built without Digitalis "
              "(build native/digitalis first)");
#endif
    } else if (name && *name && strcmp(name, "dynarmic") != 0) {
        Fatal("unknown REFRACT_CPU '%s' (dynarmic or digitalis)", name);
    }
#ifdef RN_HAVE_DIGITALIS
    if (g_backend == Backend::Digitalis) {
        DigitalisCpuInit();
        return;
    }
#endif
    DynarmicCpuInit(max_cores);
}

const char* CpuBackendName() { return g_backend == Backend::Digitalis ? "digitalis" : "dynarmic"; }

std::unique_ptr<CpuCore> CreateCpuCore() {
#ifdef RN_HAVE_DIGITALIS
    if (g_backend == Backend::Digitalis)
        return CreateDigitalisCore();
#endif
    return CreateDynarmicCore();
}

void CpuInvalidateCode(u64 addr, u64 len) {
#ifdef RN_HAVE_DIGITALIS
    if (g_backend == Backend::Digitalis) {
        DigitalisInvalidateCode(addr, len);
        return;
    }
#endif
    DynarmicInvalidateCode(addr, len);
}

const char* CpuCodeTier(u64 host_pc) {
#ifdef RN_HAVE_DIGITALIS
    if (g_backend == Backend::Digitalis)
        return DigitalisCodeTier(host_pc);
#endif
    (void)host_pc;
    return nullptr;
}

}  // namespace rn
