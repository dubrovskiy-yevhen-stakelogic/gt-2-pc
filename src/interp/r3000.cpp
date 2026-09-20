#include "interp/r3000.h"

#include <stdexcept>

namespace gt2 {
namespace {

constexpr uint32_t kSr = 12, kCause = 13, kEpc = 14, kBadVaddr = 8;
constexpr uint32_t kSrIsolateCache = 1u << 16, kSrBev = 1u << 22;

inline uint32_t Rs(uint32_t i) { return (i >> 21) & 31; }
inline uint32_t Rt(uint32_t i) { return (i >> 16) & 31; }
inline uint32_t Rd(uint32_t i) { return (i >> 11) & 31; }
inline uint32_t Shamt(uint32_t i) { return (i >> 6) & 31; }
inline uint32_t ImmS(uint32_t i) { return uint32_t(int32_t(int16_t(i & 0xFFFF))); }
inline uint32_t ImmZ(uint32_t i) { return i & 0xFFFF; }

} // namespace

void R3000::SetReg(uint32_t index, uint32_t value) {
    if (load_.reg == index) load_.reg = 0; // a direct write beats the load that is still in flight
    gpr[index] = value;
}

void R3000::Raise(Exception cause) {
    uint32_t epc = inDelaySlot_ ? currentPc_ - 4 : currentPc_;
    if (onException && onException(cause, epc)) return;
    uint32_t& sr = cop0[kSr];
    sr = (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu);
    cop0[kCause] = (cop0[kCause] & ~0x8000007Cu) | (uint32_t(cause) << 2) | (inDelaySlot_ ? 0x80000000u : 0);
    cop0[kEpc] = epc;
    SetPc((sr & kSrBev) ? 0xBFC00180u : 0x80000080u);
    nextLoad_.reg = 0;
}

bool R3000::RunUntil(uint32_t stopAddress, uint64_t maxInstructions) {
    for (uint64_t n = 0; n < maxInstructions; n++) {
        if (pc == stopAddress) return true;
        Step();
    }
    return pc == stopAddress;
}

void R3000::Step() {
    currentPc_ = pc;
    inDelaySlot_ = branchTaken_;
    branchTaken_ = false;
    instructionCount++;

    if ((cop0[kSr] & 1) && (cop0[kSr] & cop0[kCause] & 0xFF00)) {
        // A load still in flight completes before the exception is taken.
        if (nextLoad_.reg) gpr[nextLoad_.reg] = nextLoad_.value;
        nextLoad_.reg = 0;
        load_.reg = 0;
        Raise(kInterrupt);
        return;
    }
    load_ = nextLoad_;
    nextLoad_.reg = 0;
    if (pc & 3) {
        cop0[kBadVaddr] = pc;
        Raise(kAddressLoad);
        return;
    }

    uint32_t i;
    if (!bus_.FastRead32(pc, i)) i = bus_.Read(pc, 4);
    pc = nextPc;
    nextPc += 4;

    const uint32_t rs = gpr[Rs(i)], rt = gpr[Rt(i)];
    auto load = [&](uint32_t reg, uint32_t value) {
        if (load_.reg == reg) load_.reg = 0;
        nextLoad_ = {reg, value};
    };
    auto link = [&](uint32_t reg) { SetReg(reg, currentPc_ + 8); };
    auto branchIf = [&](bool taken) {
        branchTaken_ = true; // the next instruction is a delay slot either way
        if (taken) nextPc = currentPc_ + 4 + (ImmS(i) << 2);
    };
    const bool isolated = (cop0[kSr] & kSrIsolateCache) != 0;

    switch (i >> 26) {
    case 0x00:
        switch (i & 63) {
        case 0x00: SetReg(Rd(i), rt << Shamt(i)); break;
        case 0x02: SetReg(Rd(i), rt >> Shamt(i)); break;
        case 0x03: SetReg(Rd(i), uint32_t(int32_t(rt) >> Shamt(i))); break;
        case 0x04: SetReg(Rd(i), rt << (rs & 31)); break;
        case 0x06: SetReg(Rd(i), rt >> (rs & 31)); break;
        case 0x07: SetReg(Rd(i), uint32_t(int32_t(rt) >> (rs & 31))); break;
        case 0x08: branchTaken_ = true; nextPc = rs; if (onReturn && Rs(i) == 31) onReturn(rs); break;
        case 0x09: branchTaken_ = true; nextPc = rs; link(Rd(i)); if (onCall) onCall(currentPc_, rs); break;
        case 0x0C: Raise(kSyscall); break;
        case 0x0D: Raise(kBreak); break;
        case 0x10: SetReg(Rd(i), hi); break;
        case 0x11: hi = rs; break;
        case 0x12: SetReg(Rd(i), lo); break;
        case 0x13: lo = rs; break;
        case 0x18: {
            int64_t p = int64_t(int32_t(rs)) * int64_t(int32_t(rt));
            lo = uint32_t(p);
            hi = uint32_t(uint64_t(p) >> 32);
            break;
        }
        case 0x19: {
            uint64_t p = uint64_t(rs) * uint64_t(rt);
            lo = uint32_t(p);
            hi = uint32_t(p >> 32);
            break;
        }
        case 0x1A: {
            int32_t n = int32_t(rs), d = int32_t(rt);
            if (d == 0) { lo = n >= 0 ? 0xFFFFFFFFu : 1u; hi = uint32_t(n); }
            else if (uint32_t(n) == 0x80000000u && d == -1) { lo = 0x80000000u; hi = 0; }
            else { lo = uint32_t(n / d); hi = uint32_t(n % d); }
            break;
        }
        case 0x1B:
            if (rt == 0) { lo = 0xFFFFFFFFu; hi = rs; }
            else { lo = rs / rt; hi = rs % rt; }
            break;
        case 0x20: {
            uint32_t r = rs + rt;
            if (~(rs ^ rt) & (rs ^ r) & 0x80000000u) Raise(kOverflow);
            else SetReg(Rd(i), r);
            break;
        }
        case 0x21: SetReg(Rd(i), rs + rt); break;
        case 0x22: {
            uint32_t r = rs - rt;
            if ((rs ^ rt) & (rs ^ r) & 0x80000000u) Raise(kOverflow);
            else SetReg(Rd(i), r);
            break;
        }
        case 0x23: SetReg(Rd(i), rs - rt); break;
        case 0x24: SetReg(Rd(i), rs & rt); break;
        case 0x25: SetReg(Rd(i), rs | rt); break;
        case 0x26: SetReg(Rd(i), rs ^ rt); break;
        case 0x27: SetReg(Rd(i), ~(rs | rt)); break;
        case 0x2A: SetReg(Rd(i), int32_t(rs) < int32_t(rt)); break;
        case 0x2B: SetReg(Rd(i), rs < rt); break;
        default: Raise(kReservedInstruction); break;
        }
        break;
    case 0x01: { // BLTZ / BGEZ / BLTZAL / BGEZAL
        bool ge = (Rt(i) & 1) != 0;
        bool taken = ge ? int32_t(rs) >= 0 : int32_t(rs) < 0;
        if ((Rt(i) & 0x1E) == 0x10) link(31);
        branchIf(taken);
        break;
    }
    case 0x02: branchTaken_ = true; nextPc = ((currentPc_ + 4) & 0xF0000000u) | ((i & 0x03FFFFFFu) << 2); break;
    case 0x03:
        branchTaken_ = true;
        nextPc = ((currentPc_ + 4) & 0xF0000000u) | ((i & 0x03FFFFFFu) << 2);
        link(31);
        if (onCall) onCall(currentPc_, nextPc);
        break;
    case 0x04: branchIf(rs == rt); break;
    case 0x05: branchIf(rs != rt); break;
    case 0x06: branchIf(int32_t(rs) <= 0); break;
    case 0x07: branchIf(int32_t(rs) > 0); break;
    case 0x08: {
        uint32_t imm = ImmS(i), r = rs + imm;
        if (~(rs ^ imm) & (rs ^ r) & 0x80000000u) Raise(kOverflow);
        else SetReg(Rt(i), r);
        break;
    }
    case 0x09: SetReg(Rt(i), rs + ImmS(i)); break;
    case 0x0A: SetReg(Rt(i), int32_t(rs) < int32_t(ImmS(i))); break;
    case 0x0B: SetReg(Rt(i), rs < ImmS(i)); break;
    case 0x0C: SetReg(Rt(i), rs & ImmZ(i)); break;
    case 0x0D: SetReg(Rt(i), rs | ImmZ(i)); break;
    case 0x0E: SetReg(Rt(i), rs ^ ImmZ(i)); break;
    case 0x0F: SetReg(Rt(i), ImmZ(i) << 16); break;
    case 0x10: // COP0
        switch (Rs(i)) {
        case 0x00: load(Rt(i), cop0[Rd(i)]); break;
        case 0x04: cop0[Rd(i)] = rt; break;
        case 0x10: cop0[kSr] = (cop0[kSr] & ~0xFu) | ((cop0[kSr] >> 2) & 0xFu); break; // RFE
        default: Raise(kReservedInstruction); break;
        }
        break;
    case 0x12: // COP2 (GTE)
        if (!cop2_) throw std::runtime_error("GTE instruction at " + Bus::Hex(currentPc_) + " but no GTE is attached");
        if (i & (1u << 25)) cop2_->Execute(i & 0x01FFFFFFu);
        else switch (Rs(i)) {
            case 0x00: load(Rt(i), cop2_->ReadData(Rd(i))); break;
            case 0x02: load(Rt(i), cop2_->ReadControl(Rd(i))); break;
            case 0x04: cop2_->WriteData(Rd(i), rt); break;
            case 0x06: cop2_->WriteControl(Rd(i), rt); break;
            default: Raise(kReservedInstruction); break;
        }
        break;
    case 0x11: case 0x13: Raise(kCoprocessorUnusable); break;
    case 0x20: load(Rt(i), uint32_t(int32_t(int8_t(bus_.Read(rs + ImmS(i), 1))))); break;
    case 0x21: {
        uint32_t a = rs + ImmS(i);
        if (a & 1) { cop0[kBadVaddr] = a; Raise(kAddressLoad); }
        else load(Rt(i), uint32_t(int32_t(int16_t(bus_.Read(a, 2)))));
        break;
    }
    case 0x22: { // LWL
        uint32_t a = rs + ImmS(i), cur = load_.reg == Rt(i) ? load_.value : rt;
        uint32_t w = bus_.Read(a & ~3u, 4), s = (a & 3) * 8;
        load(Rt(i), (cur & (0x00FFFFFFu >> s)) | (w << (24 - s)));
        break;
    }
    case 0x23: {
        uint32_t a = rs + ImmS(i);
        if (a & 3) { cop0[kBadVaddr] = a; Raise(kAddressLoad); }
        else {
            uint32_t v;
            if (!bus_.FastRead32(a, v)) v = bus_.Read(a, 4);
            load(Rt(i), v);
        }
        break;
    }
    case 0x24: load(Rt(i), bus_.Read(rs + ImmS(i), 1) & 0xFF); break;
    case 0x25: {
        uint32_t a = rs + ImmS(i);
        if (a & 1) { cop0[kBadVaddr] = a; Raise(kAddressLoad); }
        else load(Rt(i), bus_.Read(a, 2) & 0xFFFF);
        break;
    }
    case 0x26: { // LWR
        uint32_t a = rs + ImmS(i), cur = load_.reg == Rt(i) ? load_.value : rt;
        uint32_t w = bus_.Read(a & ~3u, 4), s = (a & 3) * 8;
        load(Rt(i), s == 0 ? w : ((cur & (0xFFFFFFFFu << (32 - s))) | (w >> s)));
        break;
    }
    case 0x28: if (!isolated) bus_.Write(rs + ImmS(i), rt & 0xFF, 1); break;
    case 0x29: {
        uint32_t a = rs + ImmS(i);
        if (a & 1) { cop0[kBadVaddr] = a; Raise(kAddressStore); }
        else if (!isolated) bus_.Write(a, rt & 0xFFFF, 2);
        break;
    }
    case 0x2A: { // SWL
        uint32_t a = rs + ImmS(i), s = (a & 3) * 8;
        uint32_t w = bus_.Read(a & ~3u, 4);
        if (!isolated) bus_.Write(a & ~3u, (w & (0xFFFFFF00u << s)) | (rt >> (24 - s)), 4);
        break;
    }
    case 0x2B: {
        uint32_t a = rs + ImmS(i);
        if (a & 3) { cop0[kBadVaddr] = a; Raise(kAddressStore); }
        else if (!isolated && !bus_.FastWrite32(a, rt)) bus_.Write(a, rt, 4);
        break;
    }
    case 0x2E: { // SWR
        uint32_t a = rs + ImmS(i), s = (a & 3) * 8;
        uint32_t w = bus_.Read(a & ~3u, 4);
        if (!isolated) bus_.Write(a & ~3u, s == 0 ? rt : ((w & (0xFFFFFFFFu >> (32 - s))) | (rt << s)), 4);
        break;
    }
    case 0x32: { // LWC2
        if (!cop2_) throw std::runtime_error("GTE load at " + Bus::Hex(currentPc_) + " but no GTE is attached");
        cop2_->WriteData(Rt(i), bus_.Read(rs + ImmS(i), 4));
        break;
    }
    case 0x3A: { // SWC2
        if (!cop2_) throw std::runtime_error("GTE store at " + Bus::Hex(currentPc_) + " but no GTE is attached");
        bus_.Write(rs + ImmS(i), cop2_->ReadData(Rt(i)), 4);
        break;
    }
    default: Raise(kReservedInstruction); break;
    }

    if (load_.reg) gpr[load_.reg] = load_.value;
    gpr[0] = 0;
}

} // namespace gt2
