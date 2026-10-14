// CpuCore backed by Digitalis, the translator Refract runs in the emulator (lite translator + heavy
// optimizer, block chaining, return prediction). Guest memory is identity mapped, which is what
// Digitalis assumes anyway (guest address == host address).
#include <windows.h>

#include <xmmintrin.h>

#include <cstring>

#include "berberis/guest_state/guest_state_opaque.h"
#include "berberis/runtime_primitives/host_code.h"
#include "berberis/runtime_primitives/runtime_library.h"
#include "berberis/runtime_primitives/translation_cache.h"

#include "digitalis_core.h"

extern "C" void berberis_RunGeneratedCode(berberis::ThreadState* state, berberis::HostCode code);

namespace rn {

using berberis::CPUState;

namespace {

u32 FlagsToNzcv(u16 flags) {
    u32 nzcv = 0;
    if (flags & CPUState::kFlagNegative) nzcv |= 1u << 31;
    if (flags & CPUState::kFlagZero) nzcv |= 1u << 30;
    if (flags & CPUState::kFlagCarry) nzcv |= 1u << 29;
    if (flags & CPUState::kFlagOverflow) nzcv |= 1u << 28;
    return nzcv;
}

u16 NzcvToFlags(u32 nzcv) {
    u16 flags = 0;
    if (nzcv & (1u << 31)) flags |= CPUState::kFlagNegative;
    if (nzcv & (1u << 30)) flags |= CPUState::kFlagZero;
    if (nzcv & (1u << 29)) flags |= CPUState::kFlagCarry;
    if (nzcv & (1u << 28)) flags |= CPUState::kFlagOverflow;
    return flags;
}

// Digitalis keeps the guest's FPCR rounding mode and FZ in the host MXCSR (the interpreter's
// ProgramHostMxcsrFromFpcr); a context switch to a different FPCR has to do the same.
void ProgramMxcsr(u32 fpcr) {
    static constexpr u32 kArmRmodeToX86Rc[4] = {0b00, 0b10, 0b01, 0b11};
    u32 mxcsr = _mm_getcsr();
    mxcsr &= ~((0b11u << 13) | (1u << 15) | (1u << 6));
    mxcsr |= 0b111111u << 7;
    mxcsr |= kArmRmodeToX86Rc[(fpcr >> 22) & 3] << 13;
    if (fpcr & (1u << 24))
        mxcsr |= (1u << 15) | (1u << 6);
    _mm_setcsr(mxcsr);
}

}  // namespace

DigitalisCore* DigitalisCore::Current() { return g_digitalis_current; }

DigitalisCore::DigitalisCore() {
    state_ = berberis::CreateThreadState();
    if (!state_)
        Fatal("digitalis: cannot allocate a guest thread state");
    berberis::SetPendingSignalsStatusAtomic(*state_, berberis::kPendingSignalsEnabled);
    DigitalisRegisterCore(this);
}

DigitalisCore::~DigitalisCore() {
    DigitalisUnregisterCore(this);
    berberis::DestroyThreadState(state_);
}

StopInfo DigitalisCore::Run() {
    DigitalisCore* outer = g_digitalis_current;
    g_digitalis_current = this;
    auto* cache = berberis::TranslationCache::GetInstance();
    StopInfo result;
    for (;;) {
        // Reset the exit flag before looking at stop requests, so a RequestStop() racing with this
        // either is seen here or sets the flag again after the reset.
        state_->pending_signals_status.store(berberis::kPendingSignalsEnabled, std::memory_order_relaxed);
        if (stop_.kind != StopInfo::None) {
            result = stop_;
            stop_ = {};
            if (result.kind != StopInfo::Svc && result.pc == 0)
                result.pc = state_->cpu.insn_addr;
            break;
        }
        if (stop_requested_.exchange(false)) {
            result.kind = StopInfo::External;
            break;
        }
        auto code = cache->GetHostCodePtr(state_->cpu.insn_addr)->load();
        berberis_RunGeneratedCode(state_, berberis::AsHostCode(code));
    }
    g_digitalis_current = outer;
    return result;
}

Vec128 DigitalisCore::V(int i) const {
    Vec128 v;
    std::memcpy(v.data(), &state_->cpu.v[i], 16);
    return v;
}

void DigitalisCore::SetV(int i, Vec128 v) { std::memcpy(&state_->cpu.v[i], v.data(), 16); }

void DigitalisCore::GetContext(CpuContext* c) const {
    const CPUState& cpu = state_->cpu;
    for (int i = 0; i < 31; ++i)
        c->x[i] = cpu.x[i];
    c->sp = cpu.sp;
    c->pc = cpu.insn_addr;
    c->pstate = FlagsToNzcv(cpu.flags);
    c->fpcr = cpu.cached_fpcr;
    c->fpsr = cpu.emulated_fpsr;
    for (int i = 0; i < 32; ++i)
        std::memcpy(c->v[i].data(), &cpu.v[i], 16);
}

void DigitalisCore::SetContext(const CpuContext& c) {
    CPUState& cpu = state_->cpu;
    for (int i = 0; i < 31; ++i)
        cpu.x[i] = c.x[i];
    cpu.sp = c.sp;
    cpu.insn_addr = c.pc;
    cpu.flags = NzcvToFlags(c.pstate);
    if (cpu.cached_fpcr != c.fpcr) {
        cpu.cached_fpcr = c.fpcr;
        ProgramMxcsr(c.fpcr);
    }
    cpu.emulated_fpsr = c.fpsr;
    for (int i = 0; i < 32; ++i)
        std::memcpy(&cpu.v[i], c.v[i].data(), 16);
}

void DigitalisCore::ClearExclusive() { state_->cpu.reservation_address = 0; }

void DigitalisCore::RequestStop() {
    stop_requested_.store(true);
    state_->pending_signals_status.store(berberis::kPendingSignalsPresent);
}

void DigitalisCore::Stop(const StopInfo& s) {
    if (stop_.kind == StopInfo::None)
        stop_ = s;
    state_->pending_signals_status.store(berberis::kPendingSignalsPresent);
}

void DigitalisCore::Kick() {
    state_->pending_signals_status.store(berberis::kPendingSignalsPresent);
}

void DigitalisCpuInit() { DigitalisRuntimeInit(); }

std::unique_ptr<CpuCore> CreateDigitalisCore() { return std::make_unique<DigitalisCore>(); }

void DigitalisInvalidateCode(u64 addr, u64 len) {
    if (len == ~0ull)
        len = (1ull << 48) - addr;
    DigitalisInvalidate(addr, len);
}

}  // namespace rn
