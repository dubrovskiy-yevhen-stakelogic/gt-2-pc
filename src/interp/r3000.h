#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "guest/bus.h"

namespace gt2 {

// Coprocessor 2 (GTE) interface - implemented separately so the CPU can be tested on its own.
class Cop2 {
public:
    virtual ~Cop2() = default;
    virtual uint32_t ReadData(uint32_t reg) = 0;
    virtual void WriteData(uint32_t reg, uint32_t value) = 0;
    virtual uint32_t ReadControl(uint32_t reg) = 0;
    virtual void WriteControl(uint32_t reg, uint32_t value) = 0;
    virtual void Execute(uint32_t command) = 0;
};

// MIPS R3000A interpreter with the behaviours PS1 code depends on: branch delay slots, load delay
// slots, unaligned lwl/lwr/swl/swr, overflow traps, MIPS results for division by zero, COP0
// exception entry (SR stack, Cause, EPC, BD) and cache isolation.
class R3000 {
public:
    enum Exception : uint32_t { kInterrupt = 0, kAddressLoad = 4, kAddressStore = 5, kSyscall = 8, kBreak = 9,
                                kReservedInstruction = 10, kCoprocessorUnusable = 11, kOverflow = 12 };

    explicit R3000(Bus& bus, Cop2* cop2 = nullptr) : bus_(bus), cop2_(cop2) {}

    std::array<uint32_t, 32> gpr{};
    uint32_t hi = 0, lo = 0;
    uint32_t pc = 0, nextPc = 4;
    std::array<uint32_t, 32> cop0{};
    uint64_t instructionCount = 0;

    // Called on every exception before the guest handler is entered. Return true to swallow the
    // exception (no vector jump) - the harness uses it to service syscalls/BIOS traps on the host.
    std::function<bool(Exception cause, uint32_t epc)> onException;

    // RE aids: linked calls (jal/jalr) and returns (jr ra).
    std::function<void(uint32_t from, uint32_t to)> onCall;
    std::function<void(uint32_t to)> onReturn;

    void SetPc(uint32_t address) { pc = address; nextPc = address + 4; }
    void Step();

    // Runs until pc == stopAddress (checked before each instruction) or maxInstructions elapse.
    // Returns true when the stop address was reached.
    bool RunUntil(uint32_t stopAddress, uint64_t maxInstructions);

private:
    void SetReg(uint32_t index, uint32_t value);
    void Raise(Exception cause);
    void Branch(uint32_t target) { nextPc = target; branchTaken_ = true; }

    Bus& bus_;
    Cop2* cop2_;
    uint32_t currentPc_ = 0;
    bool inDelaySlot_ = false, branchTaken_ = false;
    struct PendingLoad { uint32_t reg = 0, value = 0; };
    PendingLoad load_, nextLoad_;
};

} // namespace gt2
