// Digitalis' runtime glue for Windows: what runtime/runtime_library_x86_64.cc, runtime/arm64/
// translator*.cc and Berberis' signal handling do on Linux.
//
// - Entry/exit stubs between C++ and generated code, rewritten for the Win64 ABI: the state pointer
//   arrives in rcx, rdi/rsi/xmm6-xmm15 are callee-saved, and calls need 32 bytes of shadow space.
//   Generated code itself is unchanged (it calls no host functions, only these stubs).
// - The dispatcher handlers (translate, interpret, gear up), with guest executability taken from
//   refract_native's address space instead of Berberis' GuestMapShadow.
// - Guest syscalls, undefined instructions and memory faults become CpuCore stops; generated code
//   leaves through pending_signals_status, the flag it already polls for Linux signals.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "berberis/assembler/machine_code.h"
#include "berberis/base/config.h"
#include "berberis/base/config_globals.h"
#include "berberis/guest_state/guest_addr.h"
#include "berberis/guest_state/guest_state.h"
#include "berberis/guest_state/guest_state_opaque.h"
#include "berberis/heavy_optimizer/arm64/heavy_optimize_region.h"
#include "berberis/interpreter/arm64/interpreter.h"
#include "berberis/lite_translator/lite_translate_region.h"
#include "berberis/runtime_primitives/code_pool.h"
#include "berberis/runtime_primitives/host_code.h"
#include "berberis/runtime_primitives/recovery_code.h"
#include "berberis/runtime_primitives/runtime_library.h"
#include "berberis/runtime_primitives/translation_cache.h"

#include "faulty_memory_accesses.h"

#include "digitalis_core.h"
#include "kernel.h"
#include "linux_abi.h"
#include "memory.h"

namespace berberis {

// The frame berberis_RunGeneratedCode sets up below its saved registers: the scratch the regions
// may use, and a sentinel host return pair (see kHostReturnPairSize), plus alignment.
inline constexpr uint32_t kBaseFrameSize = 40;
inline constexpr uint32_t kXmmSaveSize = 10 * 16;  // xmm6-xmm15
static_assert(kBaseFrameSize >= config::kHostReturnPairSize);
// Return address, 8 saved registers, xmm6-15, the caller's host return stack, then the base frame:
// the stack must end up aligned for calls.
static_assert((8 + 8 * 8 + kXmmSaveSize + 2 * 8 + kBaseFrameSize) % 16 == 0);

// Exits generated code: syncs insn_addr, restores the C++ caller's registers and leaves rcx = state
// for a sibling call (the caller of berberis_RunGeneratedCode provided its shadow space).
// clang-format off
#define END_GENERATED_CODE(EXIT_INSN)                                     \
  asm(                                                                    \
      "mov %%rax, %c[InsnAddr](%%rbp)\n"                                  \
      "movb %[OutsideGeneratedCode], %c[Residence](%%rbp)\n"              \
      "mov %%rbp, %%rcx\n"                                                \
      /* Drop host return pairs and the base frame, put back the */       \
      /* caller's host return stack. */                                   \
      "mov %c[RetBase](%%rbp), %%rsp\n"                                   \
      "add %[BaseFrameSize], %%rsp\n"                                     \
      "pop %c[RetLimit](%%rbp)\n"                                         \
      "pop %c[RetBase](%%rbp)\n"                                          \
      "movdqu 0(%%rsp), %%xmm6\n"                                         \
      "movdqu 16(%%rsp), %%xmm7\n"                                        \
      "movdqu 32(%%rsp), %%xmm8\n"                                        \
      "movdqu 48(%%rsp), %%xmm9\n"                                        \
      "movdqu 64(%%rsp), %%xmm10\n"                                       \
      "movdqu 80(%%rsp), %%xmm11\n"                                       \
      "movdqu 96(%%rsp), %%xmm12\n"                                       \
      "movdqu 112(%%rsp), %%xmm13\n"                                      \
      "movdqu 128(%%rsp), %%xmm14\n"                                      \
      "movdqu 144(%%rsp), %%xmm15\n"                                      \
      "add %[XmmSaveSize], %%rsp\n"                                       \
      "pop %%r15\n"                                                       \
      "pop %%r14\n"                                                       \
      "pop %%r13\n"                                                       \
      "pop %%r12\n"                                                       \
      "pop %%rsi\n"                                                       \
      "pop %%rdi\n"                                                       \
      "pop %%rbx\n"                                                       \
      "pop %%rbp\n"                                                       \
      EXIT_INSN                                                           \
      ::[InsnAddr] "i"(offsetof(berberis::ThreadState, cpu.insn_addr)),   \
      [Residence] "i"(offsetof(berberis::ThreadState, residence)),        \
      [OutsideGeneratedCode] "i"(berberis::kOutsideGeneratedCode),        \
      [RetBase] "i"(offsetof(berberis::ThreadState, host_return_stack.base)),   \
      [RetLimit] "i"(offsetof(berberis::ThreadState, host_return_stack.limit)), \
      [BaseFrameSize] "i"(berberis::kBaseFrameSize),                      \
      [XmmSaveSize] "i"(berberis::kXmmSaveSize))
// clang-format on

extern "C" {

void berberis_HandleNotTranslated(ThreadState* state);
void berberis_HandleInterpret(ThreadState* state);
const void* berberis_GetDispatchAddress(ThreadState* state);
void berberis_HandleLiteCounterThresholdReached(ThreadState* state);
void berberis_HandleNoExec(ThreadState* state);

[[gnu::naked]] [[gnu::noinline]] void berberis_RunGeneratedCode(ThreadState* state, HostCode code) {
  // Win64: state in rcx, code in rdx. Stack at entry is 8 mod 16.
  // clang-format off
  asm(
    "push %%rbp\n"
    "push %%rbx\n"
    "push %%rdi\n"
    "push %%rsi\n"
    "push %%r12\n"
    "push %%r13\n"
    "push %%r14\n"
    "push %%r15\n"
    "sub %[XmmSaveSize], %%rsp\n"
    "movdqu %%xmm6, 0(%%rsp)\n"
    "movdqu %%xmm7, 16(%%rsp)\n"
    "movdqu %%xmm8, 32(%%rsp)\n"
    "movdqu %%xmm9, 48(%%rsp)\n"
    "movdqu %%xmm10, 64(%%rsp)\n"
    "movdqu %%xmm11, 80(%%rsp)\n"
    "movdqu %%xmm12, 96(%%rsp)\n"
    "movdqu %%xmm13, 112(%%rsp)\n"
    "movdqu %%xmm14, 128(%%rsp)\n"
    "movdqu %%xmm15, 144(%%rsp)\n"

    // Save the host return stack of generated code that called us, if any (END_GENERATED_CODE
    // puts it back), and set up ours: no pairs yet, and a sentinel pair a guest return only matches
    // if it returns to 0, whose host return exits generated code.
    "push %c[RetBase](%%rcx)\n"
    "push %c[RetLimit](%%rcx)\n"
    "sub %[BaseFrameSize], %%rsp\n"
    "mov %%rsp, %c[RetBase](%%rcx)\n"
    "lea -%c[RetStackSize](%%rsp), %%r8\n"
    "mov %%r8, %c[RetLimit](%%rcx)\n"
    "lea berberis_entry_ExitGeneratedCode(%%rip), %%r8\n"
    "mov %%r8, %c[PairHost](%%rsp)\n"
    "movq $0, %c[PairGuest](%%rsp)\n"

    "mov %%rcx, %%rbp\n"
    "mov %c[InsnAddr](%%rbp), %%rax\n"
    "movb %[InsideGeneratedCode], %c[Residence](%%rbp)\n"
    "jmp *%%rdx\n"
    ::[InsnAddr] "i"(offsetof(ThreadState, cpu.insn_addr)),
    [Residence] "i"(offsetof(ThreadState, residence)),
    [InsideGeneratedCode] "i"(kInsideGeneratedCode),
    [RetBase] "i"(offsetof(ThreadState, host_return_stack.base)),
    [RetLimit] "i"(offsetof(ThreadState, host_return_stack.limit)),
    [RetStackSize] "i"(config::kHostReturnStackSize),
    [PairHost] "i"(config::kHostReturnPairHostOffset),
    [PairGuest] "i"(config::kHostReturnPairGuestOffset),
    [BaseFrameSize] "i"(kBaseFrameSize),
    [XmmSaveSize] "i"(kXmmSaveSize));
  // clang-format on
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_Interpret() {
  // Region code jumps here with rsp aligned for calls (base frame or below, see kBaseFrameSize).
  // clang-format off
  asm(
    "mov %%rax, %c[InsnAddr](%%rbp)\n"
    "movb %[OutsideGeneratedCode], %c[Residence](%%rbp)\n"
    "sub $32, %%rsp\n"  // Win64 shadow space
    "mov %%rbp, %%rcx\n"
    "call berberis_HandleInterpret\n"
    "mov %%rbp, %%rcx\n"
    "call berberis_GetDispatchAddress\n"
    "add $32, %%rsp\n"
    "mov %%rax, %%rdx\n"
    "mov %c[InsnAddr](%%rbp), %%rax\n"
    "movb %[InsideGeneratedCode], %c[Residence](%%rbp)\n"
    "jmp *%%rdx\n"
    ::[InsnAddr] "i"(offsetof(berberis::ThreadState, cpu.insn_addr)),
    [Residence] "i"(offsetof(berberis::ThreadState, residence)),
    [OutsideGeneratedCode] "i"(berberis::kOutsideGeneratedCode),
    [InsideGeneratedCode] "i"(berberis::kInsideGeneratedCode));
  // clang-format on
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_ExitGeneratedCode() {
  END_GENERATED_CODE("ret");
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_Stop() {
  END_GENERATED_CODE("ret");
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_NoExec() {
  END_GENERATED_CODE("jmp berberis_HandleNoExec");
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_NotTranslated() {
  END_GENERATED_CODE("jmp berberis_HandleNotTranslated");
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_Translating() {
  END_GENERATED_CODE("ret");
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_Invalidating() {
  END_GENERATED_CODE("ret");
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_Wrapping() {
  END_GENERATED_CODE("ret");
}

[[gnu::naked]] [[gnu::noinline]] void berberis_entry_HandleLiteCounterThresholdReached() {
  END_GENERATED_CODE("jmp berberis_HandleLiteCounterThresholdReached");
}

}  // extern "C"

// From code_gen_lib (EmitDirectDispatch / EmitDirectCallDispatch).
void SetDirectDispatchChaining(bool enabled);
void SetReturnPrediction(bool enabled);

namespace {

using rn::DigitalisCore;
using rn::StopInfo;

GuestCodeEntry::Kind kSpecialHandler = GuestCodeEntry::Kind::kSpecialHandler;
GuestCodeEntry::Kind kInterpreted = GuestCodeEntry::Kind::kInterpreted;
GuestCodeEntry::Kind kLiteTranslated = GuestCodeEntry::Kind::kLiteTranslated;
GuestCodeEntry::Kind kHeavyOptimized = GuestCodeEntry::Kind::kHeavyOptimized;

// Same modes as the emulator build (berberis.mode / BERBERIS_MODE).
enum class TranslationMode { kInterpretOnly, kLiteTranslateOrFallbackToInterpret, kTwoGear };
TranslationMode g_translation_mode = TranslationMode::kTwoGear;

enum class TranslationGear { kFirst, kSecond };

// Upstream keeps fully translated regions under 20 instructions on the lite tier. Unity/IL2CPP
// code is mostly short regions between calls, so that left over half of Manta's main thread on
// lite code; letting every hot region go heavy took it from ~105 to ~122 fps (2026-10-15).
constexpr size_t kGearUpMinInsns = 1;
constexpr uint32_t kInsnSize = 4;

// Executable bytes from pc, scanning at most one guest page past it (GuestMapShadow's contract).
size_t GetExecutableRegionSize(GuestAddr pc) {
  rn::Vma vma;
  if (!rn::Mem().FindVma(pc, &vma) || !(vma.prot & rn::lx::PROT_EXEC_)) return 0;
  GuestAddr scan_end = AlignUp(pc + config::kGuestPageSize, config::kGuestPageSize);
  rn::Vma next;
  GuestAddr end = vma.end;
  // Adjacent executable mappings (a library's text split by mprotect) continue the scan.
  while (end < scan_end && rn::Mem().FindVma(end, &next) && next.start == end &&
         (next.prot & rn::lx::PROT_EXEC_)) {
    end = next.end;
  }
  return std::min<GuestAddr>(end, scan_end) - pc;
}

std::vector<TranslationCache::ChainSite>& LastInstalledChainSites() {
  static thread_local std::vector<TranslationCache::ChainSite> sites;
  return sites;
}

// Host code ranges and their tier, for the host profiler (rn::DigitalisCodeTier). Entries of
// dropped code stay until the pool reuses the address.
std::mutex g_tiers_mu;
std::map<HostCodeAddr, std::pair<uint32_t, bool>> g_tiers;  // start -> (size, heavy)

HostCodePiece InstallTranslated(MachineCode* machine_code, GuestAddr pc, size_t size, bool heavy) {
  HostCodeAddr host_code = GetDefaultCodePoolInstance()->Add(machine_code);
  auto& sites = LastInstalledChainSites();
  sites.clear();
  if (host_code != kNullHostCodeAddr) {
    machine_code->ForEachChainSite([&](uint32_t offset, intptr_t target) {
      sites.push_back({host_code + offset, static_cast<GuestAddr>(target)});
    });
    std::lock_guard lock(g_tiers_mu);
    g_tiers[host_code] = {machine_code->install_size(), heavy};
  }
  (void)pc;
  (void)size;
  return {host_code, machine_code->install_size()};
}

std::tuple<bool, HostCodePiece, size_t, GuestCodeEntry::Kind> TryLiteTranslateAndInstallRegion(
    GuestAddr pc, size_t exec_size, LiteTranslateParams params = LiteTranslateParams()) {
  MachineCode machine_code;
  params.end_pc = pc + exec_size;
  params.allow_dispatch = true;
  if (IsConfigFlagSet(kDisableRegMap)) params.enable_reg_mapping = false;
  auto [success, stop_pc] = TryLiteTranslateRegion(pc, &machine_code, params);
  size_t size = stop_pc - pc;
  if (success) {
    HostCodePiece piece = InstallTranslated(&machine_code, pc, size, false);
    if (piece.code == kNullHostCodeAddr) return {false, {}, 0, {}};
    return {true, piece, size, kLiteTranslated};
  }
  if (size == 0) return {false, {}, 0, {}};
  // Partial success: re-translate up to the point of failure.
  MachineCode another_machine_code;
  params.end_pc = stop_pc;
  std::tie(success, stop_pc) = TryLiteTranslateRegion(pc, &another_machine_code, params);
  CHECK(success);
  CHECK_EQ(stop_pc, params.end_pc);
  HostCodePiece piece = InstallTranslated(&another_machine_code, pc, size, false);
  if (piece.code == kNullHostCodeAddr) return {false, {}, 0, {}};
  return {true, piece, size, kLiteTranslated};
}

// BERBERIS_GEARUP_MIN_INSNS: smallest fully translated region the heavy tier keeps (upstream's knob).
size_t GearUpMinInsns() {
  static const size_t value = [] {
    const char* v = getenv("BERBERIS_GEARUP_MIN_INSNS");
    return v ? static_cast<size_t>(strtoul(v, nullptr, 10)) : kGearUpMinInsns;
  }();
  return value;
}

// BERBERIS_GEAR_STATS=1: every 10 s, log the gear-up outcomes (kept heavy, kept a looping prefix,
// bailed, too small) and the instructions the heavy optimizer bailed on most often.
struct GearStats {
  std::mutex mu;
  uint64_t heavy = 0, prefix = 0, bailed = 0, small = 0;
  std::map<uint32_t, std::pair<uint64_t, GuestAddr>> bail_insns;  // word -> (count, example pc)
  uint64_t last_log_ns = 0;
};

void RecordGearUp(int outcome, GuestAddr stop_pc) {  // 0 heavy, 1 prefix, 2 bailed, 3 small
  static const bool enabled = getenv("BERBERIS_GEAR_STATS") != nullptr;
  if (!enabled) return;
  static GearStats s;
  std::lock_guard lock(s.mu);
  (outcome == 0 ? s.heavy : outcome == 1 ? s.prefix : outcome == 2 ? s.bailed : s.small)++;
  if (outcome == 1 || outcome == 2) {
    uint32_t word = 0;
    rn::SafeCopyFromGuest(&word, stop_pc, 4);
    auto& e = s.bail_insns[word];
    if (!e.first++) e.second = stop_pc;
  }
  const uint64_t now = rn::MonotonicNs();
  if (now - s.last_log_ns < 10'000'000'000ull) return;
  s.last_log_ns = now;
  rn::Log("digitalis gear-up: %llu heavy, %llu looping prefix, %llu bailed, %llu too small (< %zu insns)",
          static_cast<unsigned long long>(s.heavy), static_cast<unsigned long long>(s.prefix),
          static_cast<unsigned long long>(s.bailed), static_cast<unsigned long long>(s.small), GearUpMinInsns());
  std::vector<std::pair<uint64_t, uint32_t>> top;
  for (auto& [word, e] : s.bail_insns) top.push_back({e.first, word});
  std::sort(top.rbegin(), top.rend());
  for (size_t i = 0; i < top.size() && i < 15; ++i)
    rn::Log("  bailed %6llu x on %08x, e.g. %s", static_cast<unsigned long long>(top[i].first), top[i].second,
            rn::DescribeAddress(s.bail_insns[top[i].second].second).c_str());
}

std::tuple<bool, HostCodePiece, size_t, GuestCodeEntry::Kind> HeavyOptimizeAndInstallRegion(
    GuestAddr pc, size_t exec_size) {
  MachineCode machine_code;
  bool has_in_region_backedge = false;
  auto [stop_pc, success, number_of_instructions] =
      HeavyOptimizeRegion(pc, &machine_code, {.end_pc = pc + exec_size}, &has_in_region_backedge);
  size_t size = stop_pc - pc;
  // A prefix the heavy frontend translated before bailing is kept only when it holds a loop
  // (see translator_x86_64.cc upstream); small regions stay lite.
  if (!success && !(has_in_region_backedge && size > 0)) {
    RecordGearUp(2, stop_pc);
    return {false, {}, 0, {}};
  }
  if (success && number_of_instructions < GearUpMinInsns()) {
    RecordGearUp(3, stop_pc);
    return {false, {}, 0, {}};
  }
  RecordGearUp(success ? 0 : 1, stop_pc);
  HostCodePiece piece = InstallTranslated(&machine_code, pc, size, true);
  if (piece.code == kNullHostCodeAddr) return {false, {}, 0, {}};
  return {true, piece, size, kHeavyOptimized};
}

template <TranslationGear kGear = TranslationGear::kFirst>
void TranslateRegion(GuestAddr pc) {
  rn::g_digitalis_translating = pc;
  struct Reset {
    ~Reset() { rn::g_digitalis_translating = 0; }
  } reset;
  TranslationCache* cache = TranslationCache::GetInstance();
  GuestCodeEntry* entry;
  if constexpr (kGear == TranslationGear::kFirst) {
    entry = cache->AddAndLockForTranslation(pc, 0);
  } else {
    entry = cache->LockForGearUpTranslation(pc);
  }
  if (!entry) return;

  size_t exec_size = GetExecutableRegionSize(pc);
  if (exec_size < kInsnSize || (pc & 3)) {
    cache->SetTranslatedAndUnlock(pc, entry, kInsnSize, kSpecialHandler, {kEntryNoExec, 0});
    return;
  }

  bool success = false;
  HostCodePiece host_code_piece{kEntryInterpret, 0};
  size_t size = kInsnSize;
  GuestCodeEntry::Kind kind = kInterpreted;
  if (g_translation_mode == TranslationMode::kInterpretOnly) {
  } else if (kGear == TranslationGear::kSecond) {
    std::tie(success, host_code_piece, size, kind) = HeavyOptimizeAndInstallRegion(pc, exec_size);
    if (!success) {
      std::tie(success, host_code_piece, size, kind) = TryLiteTranslateAndInstallRegion(pc, exec_size);
    }
  } else {
    LiteTranslateParams params;
    if (g_translation_mode == TranslationMode::kTwoGear) {
      params.enable_self_profiling = true;
      params.counter_location = &entry->invocation_counter;
      params.counter_threshold = config::kGearSwitchThreshold;
    }
    std::tie(success, host_code_piece, size, kind) =
        TryLiteTranslateAndInstallRegion(pc, exec_size, params);
  }
  if (!success) {
    host_code_piece = {kEntryInterpret, 0};
    size = kInsnSize;
    kind = kInterpreted;
  }
  cache->SetTranslatedAndUnlock(pc, entry, size, kind, host_code_piece,
                                success ? &LastInstalledChainSites() : nullptr);
}

// --- Cores -----------------------------------------------------------------------------------------

std::mutex g_cores_mu;
std::vector<DigitalisCore*> g_cores;

void FlushGuestCodeCache() {
  std::lock_guard lock(g_cores_mu);
  for (DigitalisCore* core : g_cores) core->Kick();
}

// --- Faults ----------------------------------------------------------------------------------------

// A guest memory access in generated code faulted: run the region's recovery code (it syncs the
// guest state, including insn_addr, and exits generated code) and report the fault from Run().
LONG CALLBACK GeneratedCodeFaultHandler(EXCEPTION_POINTERS* ep) {
  // REFRACT_DIGITALIS_DEBUG=1: log every host exception (also the ones SEH handles later).
  static const bool debug = getenv("REFRACT_DIGITALIS_DEBUG") != nullptr;
  if (debug && ep->ExceptionRecord->ExceptionCode >= 0x80000000u) {
    DigitalisCore* c = rn::g_digitalis_current;
    rn::Log("[digitalis] exception 0x%lx at rip 0x%llx (exe+0x%llx) (info %llx %llx) residence %d guest pc 0x%llx",
            ep->ExceptionRecord->ExceptionCode, ep->ContextRecord->Rip,
            ep->ContextRecord->Rip - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
            ep->ExceptionRecord->ExceptionInformation[0], ep->ExceptionRecord->ExceptionInformation[1],
            c ? int(GetResidence(*c->state())) : -1, c ? c->state()->cpu.insn_addr : 0);
  }
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  DigitalisCore* core = rn::g_digitalis_current;
  if (!core) return EXCEPTION_CONTINUE_SEARCH;
  ThreadState* state = core->state();
  if (GetResidence(*state) != kInsideGeneratedCode) return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t recovery = FindRecoveryCode(ep->ContextRecord->Rip, state);
  if (!recovery) {
    rn::Log("[digitalis] fault in generated code without recovery: rip 0x%llx addr 0x%llx guest pc ~0x%llx",
            ep->ContextRecord->Rip, ep->ExceptionRecord->ExceptionInformation[1],
            state->cpu.insn_addr);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  StopInfo s;
  s.kind = StopInfo::Fault;
  s.fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
  s.fault_write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
  core->Stop(s);
  ep->ContextRecord->Rip = recovery;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void ReportInterpreterFault(const void* addr, bool write) {
  if (DigitalisCore* core = rn::g_digitalis_current) {
    StopInfo s;
    s.kind = StopInfo::Fault;
    s.fault_addr = reinterpret_cast<uintptr_t>(addr);
    s.fault_write = write;
    core->Stop(s);
  }
}

}  // namespace

// --- Dispatcher handlers (called from the entry stubs) --------------------------------------------

extern "C" void berberis_HandleNotTranslated(ThreadState* state) {
  TranslateRegion(state->cpu.insn_addr);
}

extern "C" void berberis_HandleInterpret(ThreadState* state) {
  InterpretBatch(state, 500, TranslationCache::GetInstance(), /* stop_at_untranslated */ false);
}

extern "C" const void* berberis_GetDispatchAddress(ThreadState* state) {
  if (ArePendingSignalsPresent(*state)) return AsHostCode(kEntryExitGeneratedCode);
  return AsHostCode(TranslationCache::GetInstance()->GetHostCodePtr(state->cpu.insn_addr)->load());
}

extern "C" void berberis_HandleLiteCounterThresholdReached(ThreadState* state) {
  TranslateRegion<TranslationGear::kSecond>(state->cpu.insn_addr);
}

// The guest jumped to memory that is not executable.
extern "C" void berberis_HandleNoExec(ThreadState* state) {
  if (DigitalisCore* core = rn::g_digitalis_current) {
    StopInfo s;
    s.kind = StopInfo::Fault;
    s.fault_addr = state->cpu.insn_addr;
    core->Stop(s);
  }
}

// --- Interpreter hooks -------------------------------------------------------------------------------

void InvalidateGuestRange(GuestAddr start, GuestAddr end) {
  if (TranslationCache::GetInstance()->InvalidateGuestRange(start, end)) FlushGuestCodeCache();
}

// svc #imm, from the interpreter, with insn_addr still at the svc (it advances after we return).
// Plain syscalls run here like in the Dynarmic backend; host thunks (imm != 0) and syscalls that
// need the run loop make Run() return.
void RunGuestSyscall(ThreadState* state) {
  DigitalisCore* core = rn::g_digitalis_current;
  CHECK(core);
  uint32_t insn = *ToHostAddr<const uint32_t>(state->cpu.insn_addr);
  uint32_t imm = (insn >> 5) & 0xffff;
  static const bool in_place =
      !(getenv("REFRACT_JIT_INPLACE") && *getenv("REFRACT_JIT_INPLACE") == '0');
  if (imm == 0 && in_place) {
    switch (rn::SyscallInJit()) {
      case 1:
        return;
      case 2:
        core->Kick();
        return;
      default:
        break;
    }
  }
  StopInfo s;
  s.kind = StopInfo::Svc;
  s.imm = imm;
  core->Stop(s);
}

void UndefinedInsn(GuestAddr pc) {
  if (DigitalisCore* core = rn::g_digitalis_current) {
    StopInfo s;
    s.kind = StopInfo::Interpret;
    s.pc = pc;
    core->Stop(s);
  }
}

void BreakpointInsn(GuestAddr pc) {
  if (DigitalisCore* core = rn::g_digitalis_current) {
    StopInfo s;
    s.kind = StopInfo::Exception;
    s.exception = 8;  // Dynarmic's A64::Exception::Breakpoint, which thread.cpp checks for
    s.pc = pc;
    core->Stop(s);
  }
}

// Interpreter memory accesses (interpreter/faulty_memory_accesses.h) under SEH instead of
// Berberis' recovery-address trick.
FaultyLoadResult FaultyLoad(const void* addr, uint8_t data_bytes) {
  __try {
    switch (data_bytes) {
      case 1: return {*static_cast<const volatile uint8_t*>(addr), 0};
      case 2: return {*static_cast<const volatile uint16_t*>(addr), 0};
      case 4: return {*static_cast<const volatile uint32_t*>(addr), 0};
      default: return {*static_cast<const volatile uint64_t*>(addr), 0};
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ReportInterpreterFault(addr, false);
    return {0, 1};
  }
}

bool FaultyStore(void* addr, uint8_t data_bytes, uint64_t value) {
  __try {
    switch (data_bytes) {
      case 1: *static_cast<volatile uint8_t*>(addr) = static_cast<uint8_t>(value); break;
      case 2: *static_cast<volatile uint16_t*>(addr) = static_cast<uint16_t>(value); break;
      case 4: *static_cast<volatile uint32_t*>(addr) = static_cast<uint32_t>(value); break;
      default: *static_cast<volatile uint64_t*>(addr) = value; break;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ReportInterpreterFault(addr, true);
    return true;
  }
}

void AddFaultyMemoryAccessRecoveryCode() {
  InitExtraRecoveryCodeUnsafe({});
}

}  // namespace berberis

namespace rn {

thread_local DigitalisCore* g_digitalis_current = nullptr;
thread_local u64 g_digitalis_translating = 0;

void DigitalisRuntimeInit() {
  using namespace berberis;
  if (const char* mode = getenv("BERBERIS_MODE")) {
    if (!strcmp(mode, "interpret-only")) g_translation_mode = TranslationMode::kInterpretOnly;
    else if (!strcmp(mode, "lite-translate-or-interpret"))
      g_translation_mode = TranslationMode::kLiteTranslateOrFallbackToInterpret;
  }
  const char* chain = getenv("BERBERIS_CHAIN");
  SetDirectDispatchChaining(!(chain && chain[0] == '0'));
  const char* retpred = getenv("BERBERIS_RETPRED");
  SetReturnPrediction(!(retpred && retpred[0] == '0'));
  InitHostEntries();
  InitInterpreter();
  AddVectoredExceptionHandler(1, GeneratedCodeFaultHandler);
  Log("[digitalis] translator ready (mode %s, chaining %s, return prediction %s, top-byte-ignore %s)",
      g_translation_mode == TranslationMode::kTwoGear ? "two-gear"
      : g_translation_mode == TranslationMode::kInterpretOnly ? "interpret-only"
                                                              : "lite-translate-or-interpret",
      chain && chain[0] == '0' ? "off" : "on", retpred && retpred[0] == '0' ? "off" : "on",
      IsConfigFlagSet(kTopByteIgnore) ? "on" : "off");
}

void DigitalisInvalidate(u64 addr, u64 len) {
  berberis::InvalidateGuestRange(addr, addr + len);
}

void DigitalisRegisterCore(DigitalisCore* core) {
  std::lock_guard lock(berberis::g_cores_mu);
  berberis::g_cores.push_back(core);
}

void DigitalisUnregisterCore(DigitalisCore* core) {
  std::lock_guard lock(berberis::g_cores_mu);
  std::erase(berberis::g_cores, core);
}

const char* DigitalisCodeTier(u64 host_pc) {
  std::lock_guard lock(berberis::g_tiers_mu);
  auto it = berberis::g_tiers.upper_bound(static_cast<berberis::HostCodeAddr>(host_pc));
  if (it == berberis::g_tiers.begin()) return nullptr;
  --it;
  if (host_pc >= it->first + it->second.first) return nullptr;
  return it->second.second ? "heavy" : "lite";
}

}  // namespace rn
