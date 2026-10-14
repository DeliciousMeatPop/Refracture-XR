// The Digitalis CpuCore and what its runtime handlers (runtime_win.cpp) share with it.
#pragma once

#include <atomic>

#include "berberis/guest_state/guest_state.h"
#include "cpu.h"

namespace rn {

class DigitalisCore final : public CpuCore {
public:
    DigitalisCore();
    ~DigitalisCore() override;

    StopInfo Run() override;

    u64 X(int i) const override { return state_->cpu.x[i]; }
    void SetX(int i, u64 v) override { state_->cpu.x[i] = v; }
    u64 Sp() const override { return state_->cpu.sp; }
    void SetSp(u64 v) override { state_->cpu.sp = v; }
    u64 Pc() const override { return state_->cpu.insn_addr; }
    void SetPc(u64 v) override { state_->cpu.insn_addr = v; }
    Vec128 V(int i) const override;
    void SetV(int i, Vec128 v) override;
    void GetContext(CpuContext* c) const override;
    void SetContext(const CpuContext& c) override;
    u64& Tpidr() override { return state_->tls; }
    void ClearExclusive() override;
    void RequestStop() override;

    berberis::ThreadState* state() { return state_; }

    // Called on this core's thread from Digitalis' runtime (interpreter, dispatcher, fault handler):
    // records why Run() must return and makes generated code exit at its next check.
    void Stop(const StopInfo& s);
    // Generated code and the interpreter leave at their next check, Run() continues (code cache
    // flush on another thread, or a handled in-place syscall that needs the run loop).
    void Kick();

    static DigitalisCore* Current();

private:
    berberis::ThreadState* state_;
    StopInfo stop_;
    std::atomic<bool> stop_requested_{false};
};

// runtime_win.cpp
void DigitalisRuntimeInit();
void DigitalisInvalidate(u64 addr, u64 len);
void DigitalisRegisterCore(DigitalisCore* core);
void DigitalisUnregisterCore(DigitalisCore* core);
extern thread_local DigitalisCore* g_digitalis_current;
// Guest pc of the region this thread is translating (0 = none), for fatal-error reports.
extern thread_local u64 g_digitalis_translating;

}  // namespace rn
