#include "machine/machine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gt2 {
namespace {

constexpr uint32_t kIoBase = Bus::kIoBase;
constexpr uint32_t kCyclesPerInstruction = 2;
constexpr uint32_t kCyclesPerScanline = 2146; // 33.8688 MHz / (60 Hz * 263 lines)

std::string Hex(uint32_t v) { return Bus::Hex(v); }

} // namespace

Machine::Machine() : cpu(bus, &gte), regFile_(Bus::kIoEnd - kIoBase, 0) {
    bus.ioRead = [this](uint32_t a, int size) { return IoRead(a, size); };
    bus.ioWrite = [this](uint32_t a, uint32_t v, int size) { IoWrite(a, v, size); };
    cpu.onException = [this](R3000::Exception cause, uint32_t epc) { return OnException(cause, epc); };
}

void Machine::LoadExe(const std::vector<uint8_t>& exe, uint32_t stackTop) {
    auto u32 = [&](size_t o) { return uint32_t(exe[o] | (exe[o + 1] << 8) | (exe[o + 2] << 16) | (uint32_t(exe[o + 3]) << 24)); };
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) throw std::runtime_error("not a PS-X EXE");
    uint32_t address = u32(0x18), size = u32(0x1C);
    if (0x800 + size > exe.size()) throw std::runtime_error("PS-X EXE is truncated");
    std::memcpy(bus.RamPointer(address, size), exe.data() + 0x800, size);
    cpu.SetPc(u32(0x10));
    cpu.gpr[28] = u32(0x14);
    cpu.gpr[29] = cpu.gpr[30] = stackTop;
}

std::string Machine::Run(uint64_t maxInstructions) {
    stopReason_.clear();
    const uint64_t end = cpu.instructionCount + maxInstructions;
    try {
        while (cpu.instructionCount < end && stopReason_.empty()) {
            StepGuest();
        }
    } catch (const std::exception& e) {
        stopReason_ = std::string(e.what()) + " (pc " + Hex(cpu.pc) + ", ra " + Hex(cpu.gpr[31]) + ")";
    }
    return stopReason_.empty() ? "instruction budget exhausted" : stopReason_;
}

void Machine::EnableSceneCapture() {
    sceneCapture_ = true;
    gte.captureEnabled = true;
    hudGpu.isProjected = [this](uint32_t key) { return gte.IsProjected(key); };
    hudGpu.onDisplayFlip = [this] {
        if (onSceneFrame) onSceneFrame(pendingScene_);
        pendingScene_.transforms = gte.capturedTransforms;
        pendingScene_.vertices = gte.capturedVertices;
        gte.ClearCapture();
        hudGpu.ClearTouched();
    };
}

void Machine::TickDevices() {
    const uint64_t now = cpu.instructionCount;
    if (now >= nextVBlank_) {
        nextVBlank_ += kInstructionsPerVBlank;
        report.vblanks++;
        if (padsStarted_) { // the kernel's VBlank handler polls the pads into the InitPAD buffers
            for (size_t port = 0; port < 2; port++) {
                if (!padBuffer_[port] || padLength_[port] < 4) continue;
                const bool connected = port == 0 || pad2Connected;
                const uint16_t buttons = port == 0 ? padButtons : pad2Buttons;
                bus.Write(padBuffer_[port], connected ? 0x00 : 0xFF, 1);
                bus.Write(padBuffer_[port] + 1, 0x41, 1); // digital pad
                bus.Write(padBuffer_[port] + 2, ~buttons & 0xFF, 1);
                bus.Write(padBuffer_[port] + 3, (~buttons >> 8) & 0xFF, 1);
            }
        }
        RaiseIrq(kIrqVBlank);
    }
    if (cdrom_ && cdrom_->NextEventTime() <= now && cdrom_->Tick(now)) RaiseIrq(kIrqCdrom);
    if (now >= nextSpuBatch_) { // 44.1 kHz = one frame per 384 instructions; generated in batches of 64
        nextSpuBatch_ += 384 * 64;
        static const bool noSpu = std::getenv("GT2_NOSPU") != nullptr; // profiling aid
        if (!noSpu && spu.Generate(64)) RaiseIrq(kIrqSpu);
    }
    if (now >= sio_.ackDue) { // the device pulls /ACK low: IRQ7
        sio_.ackDue = UINT64_MAX;
        sio_.irq = true;
        RaiseIrq(kIrqSio);
        if (traceSio) SioTrace("ACK -> IRQ7", 0, 0);
    }
}

void Machine::SioTrace(const char* what, uint32_t address, uint32_t value) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s %s%s%08X", what, address ? Hex(address).c_str() : "", address ? " " : "", value);
    const std::string body = line;
    if (body == sioTraceLast_) { sioTraceRepeats_++; return; }
    if (sioTraceRepeats_ && !report.sioLog.empty()) report.sioLog.back() += " (x" + std::to_string(sioTraceRepeats_ + 1) + ")";
    sioTraceRepeats_ = 0;
    sioTraceLast_ = body;
    std::snprintf(line, sizeof(line), "f%llu +%llu pc %s: ", static_cast<unsigned long long>(report.vblanks),
                  static_cast<unsigned long long>(cpu.instructionCount), Hex(cpu.pc).c_str());
    report.sioLog.push_back(line + body);
}

// Digital pad protocol (psx-spx): 01 -> FF, 42 -> 41, 00 -> 5A, 00 -> buttons low, 00 -> buttons high.
uint8_t Machine::SioExchange(uint8_t tx, bool& ack) {
    ack = false;
    const bool port1 = (sio_.ctrl & 0x2000) == 0, selected = (sio_.ctrl & 0x0002) != 0;
    if ((!port1 && !pad2Connected) || !selected) { sio_.step = 0; return 0xFF; }
    sio_.port2 = !port1;
    if (traceSio) { // one summary line per transfer: "pad xchg tx 01 42 00 00 00 -> rx FF 41 5A BF FF"
        const uint8_t rx = SioExchangeStep(tx, ack);
        char item[8];
        std::snprintf(item, sizeof(item), " %02X", tx);
        sioTraceTx_ += item;
        std::snprintf(item, sizeof(item), " %02X", rx);
        sioTraceRx_ += item;
        if (!ack) {
            SioTrace(("pad xchg tx" + sioTraceTx_ + " -> rx" + sioTraceRx_).c_str(), 0, padButtons);
            sioTraceTx_.clear();
            sioTraceRx_.clear();
        }
        return rx;
    }
    return SioExchangeStep(tx, ack);
}

uint8_t Machine::SioExchangeStep(uint8_t tx, bool& ack) {
    switch (sio_.step) {
    case 0:
        if (tx != 0x01) return 0xFF; // 0x81 = memory card: no card inserted, nobody answers
        sio_.step = 1; ack = true; return 0xFF;
    case 1:
        if (tx != 0x42) { sio_.step = 0; return 0xFF; } // config commands are for DualShock only
        sio_.step = 2; ack = true; return 0x41;
    case 2: sio_.step = 3; ack = true; return 0x5A;
    case 3: sio_.step = 4; ack = true; return uint8_t(~(sio_.port2 ? pad2Buttons : padButtons) & 0xFF);
    default: sio_.step = 0; return uint8_t((~(sio_.port2 ? pad2Buttons : padButtons) >> 8) & 0xFF);
    }
}

void Machine::StepGuest() {
    if (cpu.instructionCount >= nextEvent_) {
        TickDevices();
        nextEvent_ = std::min({nextVBlank_, nextSpuBatch_, sio_.ackDue, cdrom_ ? cdrom_->NextEventTime() : UINT64_MAX});
    }
    const uint32_t pc = cpu.pc;
    if (pc > 0xC0 || (pc != 0xA0 && pc != 0xB0 && pc != 0xC0)) {
        cpu.Step();
        return;
    }
    const uint32_t function = cpu.gpr[9] & 0xFF;
    char name[16];
    std::snprintf(name, sizeof(name), "%X0:%02X", pc >> 4, function);
    if (traceIo && report.biosCalls[name]++ == 0)
        report.biosFirstUse.push_back(std::string(name) + " (a0 " + Hex(cpu.gpr[4]) + ", a1 " + Hex(cpu.gpr[5]) + ", a2 " +
                                      Hex(cpu.gpr[6]) + ", ra " + Hex(cpu.gpr[31]) + ")");
    const uint32_t ra = cpu.gpr[31];
    if (!BiosCall(pc, function)) {
        stopReason_ = std::string("unimplemented BIOS call ") + name + " from " + Hex(ra);
        return;
    }
    if (cpu.pc == pc) cpu.SetPc(ra); // plain functions return to the caller
}

uint32_t Machine::CallGuest(uint32_t function, uint32_t a0) {
    constexpr uint32_t kSentinel = 0xBFC0DEA0u, kKernelStack = 0x8000DFF0u;
    cpu.gpr[4] = a0;
    cpu.gpr[29] = kKernelStack;
    cpu.gpr[31] = kSentinel;
    cpu.SetPc(function);
    for (uint64_t n = 0; cpu.pc != kSentinel; n++) {
        if (n > 20'000'000 || !stopReason_.empty()) {
            if (stopReason_.empty()) stopReason_ = "kernel-chain handler " + Hex(function) + " did not return";
            break;
        }
        StepGuest();
    }
    return cpu.gpr[2];
}

// ---------------------------------------------------------------- hardware

void Machine::UpdateIrqLine() {
    if (iStat_ & iMask_) cpu.cop0[13] |= 1u << 10;
    else cpu.cop0[13] &= ~(1u << 10);
}

uint32_t Machine::IoRead(uint32_t address, int size) {
    if (traceIo) {
        auto& counter = report.io[address];
        if (counter.first++ == 0 && counter.second == 0) report.ioFirstUse.push_back("R " + Hex(address) + " @pc " + Hex(cpu.pc - 4));
    }
    const uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1;
    if (traceSio && ((address >= 0x1F801040 && address < 0x1F801050) || address == 0x1F801070)) { // timer polls are left out: they flood the log
        traceSio = false; // re-entrancy guard: read the register through the normal path, then log it
        const uint32_t v = IoRead(address, size);
        traceSio = true;
        SioTrace("R", address, v);
        return v;
    }

    if (address >= 0x1F801800 && address <= 0x1F801803) return cdrom_ ? cdrom_->Read(address & 3) : 0;
    if (address >= 0x1F801C00 && address < 0x1F802000) {
        uint32_t v = spu.Read(address - 0x1F801C00);
        if (size == 4) v |= uint32_t(spu.Read(address - 0x1F801C00 + 2)) << 16;
        return v & mask;
    }
    switch (address) {
    case 0x1F801070: return iStat_ & mask;
    case 0x1F801074: return iMask_ & mask;
    case 0x1F8010F0: return dpcr_;
    case 0x1F8010F4: return dicr_;
    case 0x1F801810: return gpu.ReadData();
    case 0x1F801814: gpuStatReads_++; return gpu.ReadStatus((report.vblanks & 1) != 0); // odd/even follows the vblank parity
    case 0x1F801820: return mdec.ReadData();
    case 0x1F801824: return mdec.ReadStatus();
    case 0x1F801040: { uint8_t v = sio_.rx; sio_.rx = 0xFF; sio_.rxReady = false; return v; }               // JOY_DATA
    case 0x1F801044: return 0x05u | (sio_.rxReady ? 0x02u : 0) | (sio_.irq ? 0x200u : 0);                    // JOY_STAT
    case 0x1F801048: return sio_.mode;
    case 0x1F80104A: return sio_.ctrl;
    case 0x1F80104E: return sio_.baud;
    case 0x1F801DAE: {                   // SPUSTAT mirrors the mode bits of SPUCNT
        uint32_t spucnt = regFile_[0xDAA] | (regFile_[0xDAB] << 8);
        return spucnt & 0x3F;
    }
    default: break;
    }
    if (address >= 0x1F801080 && address < 0x1F8010F0) {
        uint32_t ch = (address - 0x1F801080) >> 4, reg = ((address & 0xF) >> 2);
        return reg < 3 ? dma_[ch][reg] : 0;
    }
    if (address >= 0x1F801100 && address < 0x1F801130) {
        uint32_t n = (address - 0x1F801100) >> 4, reg = (address & 0xF) >> 2;
        if (reg == 0) {
            uint64_t cycles = cpu.instructionCount * kCyclesPerInstruction;
            const uint32_t mode = timer_[n][1];
            if (n == 1 && (mode & 0x100)) cycles /= kCyclesPerScanline;
            if (n == 2 && (mode & 0x200)) cycles /= 8;
            return uint32_t(cycles) & 0xFFFF;
        }
        return timer_[n][reg < 3 ? reg : 2] & mask;
    }
    uint32_t v = 0;
    std::memcpy(&v, &regFile_[address - kIoBase], size_t(size));
    return v;
}

void Machine::IoWrite(uint32_t address, uint32_t value, int size) {
    if (traceIo) {
        auto& counter = report.io[address];
        if (counter.second++ == 0 && counter.first == 0)
            report.ioFirstUse.push_back("W " + Hex(address) + " = " + Hex(value) + " @pc " + Hex(cpu.pc - 4));
    }
    if (traceSio && ((address >= 0x1F801040 && address < 0x1F801050) || address == 0x1F801070 || address == 0x1F801074 ||
                     (address >= 0x1F801120 && address < 0x1F801130)))
        SioTrace("W", address, value);

    if (address >= 0x1F801800 && address <= 0x1F801803) {
        if (cdrom_) cdrom_->Write(address & 3, uint8_t(value), cpu.instructionCount);
        nextEvent_ = 0; // the controller may have scheduled something sooner
        return;
    }
    if (address >= 0x1F801C00 && address < 0x1F802000) {
        spu.Write(address - 0x1F801C00, uint16_t(value));
        if (size == 4) spu.Write(address - 0x1F801C00 + 2, uint16_t(value >> 16));
        return;
    }
    switch (address) {
    case 0x1F801070: iStat_ &= value; UpdateIrqLine(); return;
    case 0x1F801074: iMask_ = value & 0x7FF; UpdateIrqLine(); return;
    case 0x1F801040: { // JOY_DATA: one byte out, one byte back
        bool ack = false;
        sio_.rx = SioExchange(uint8_t(value), ack);
        sio_.rxReady = true;
        if (ack) sio_.ackDue = cpu.instructionCount + 600;
        nextEvent_ = 0;
        return;
    }
    case 0x1F801048: sio_.mode = uint16_t(value); return;
    case 0x1F80104A:
        sio_.ctrl = uint16_t(value) & ~0x0050u;
        if (value & 0x10) sio_.irq = false;                                      // acknowledge
        if (value & 0x40) { sio_ = Sio{}; }                                      // reset
        if (!(value & 0x0002)) sio_.step = 0;                                    // /JOYn released: transfer ends
        return;
    case 0x1F80104E: sio_.baud = uint16_t(value); return;
    case 0x1F801820: mdec.WriteCommand(value); return;
    case 0x1F801824: mdec.WriteControl(value); return;
    case 0x1F8010F0: dpcr_ = value; return;
    case 0x1F8010F4:
        dicr_ = (dicr_ & 0x7F000000u & ~value) | (value & 0x00FF803Fu);
        if ((dicr_ & 0x7F000000u) && (dicr_ & (1u << 23))) dicr_ |= 1u << 31;
        return;
    case 0x1F801810: report.gp0Words++; gpu.WriteGp0(value); if (sceneCapture_) hudGpu.WriteGp0(value); if (onGpuWord) onGpuWord(false, value); return;
    case 0x1F801814: report.gp1Words++; gpu.WriteGp1(value); if (sceneCapture_) hudGpu.WriteGp1(value); if (onGpuWord) onGpuWord(true, value); return;
    default: break;
    }
    if (address >= 0x1F801080 && address < 0x1F8010F0) {
        uint32_t ch = (address - 0x1F801080) >> 4, reg = (address & 0xF) >> 2;
        if (reg < 3) dma_[ch][reg] = value;
        if (reg == 2 && (value & (1u << 24))) RunDma(ch);
        return;
    }
    if (address >= 0x1F801100 && address < 0x1F801130) {
        uint32_t n = (address - 0x1F801100) >> 4, reg = (address & 0xF) >> 2;
        if (reg < 3) timer_[n][reg] = value & 0xFFFF;
        return;
    }
    std::memcpy(&regFile_[address - kIoBase], &value, size_t(size));
}

void Machine::RunDma(uint32_t ch) {
    report.dmaTransfers[ch]++;
    uint32_t address = dma_[ch][0] & 0x1FFFFC;
    const uint32_t bcr = dma_[ch][1], chcr = dma_[ch][2];
    auto ramWord = [&](uint32_t a) { return bus.Read(a & 0x1FFFFC, 4); };

    if (ch == 6) { // ordering table clear: reverse linked list
        uint32_t count = bcr & 0xFFFF;
        if (count == 0) count = 0x10000;
        for (uint32_t i = 0; i < count; i++, address -= 4)
            bus.Write(address & 0x1FFFFC, i + 1 == count ? 0x00FFFFFFu : ((address - 4) & 0x1FFFFF), 4);
    } else if (ch == 4) { // SPU RAM transfer, direction from CHCR bit 0
        uint32_t bytes = (bcr & 0xFFFF) * (bcr >> 16 ? bcr >> 16 : 1) * 4;
        if (((chcr >> 9) & 3) != 1) bytes = (bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000) * 4;
        if (chcr & 1) spu.DmaWrite(bus.RamPointer(address, bytes), bytes);
        else spu.DmaRead(bus.RamPointer(address, bytes), bytes);
    } else if (ch == 0 || ch == 1) { // MDEC in (RAM -> MDEC) / out (MDEC -> RAM), sync mode 1 blocks or mode 0 words
        const uint32_t syncMode = (chcr >> 9) & 3;
        uint32_t words = syncMode == 1 ? (bcr & 0xFFFF) * (bcr >> 16) : (bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000);
        std::vector<uint32_t> buffer(words);
        if (ch == 0) {
            for (uint32_t i = 0; i < words; i++) buffer[i] = ramWord(address + i * 4);
            mdec.DmaIn(buffer.data(), words);
        } else {
            mdec.DmaOut(buffer.data(), words);
            for (uint32_t i = 0; i < words; i++) bus.Write((address + i * 4) & 0x1FFFFC, buffer[i], 4);
        }
    } else if (ch == 3 && cdrom_) { // CD-ROM data FIFO -> RAM
        uint32_t bytes = (bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000) * 4;
        cdrom_->DmaRead(bus.RamPointer(address, bytes), bytes);
    } else if (ch == 2 && (chcr & 1)) { // RAM -> GPU
        if (sceneCapture_) hudGpu.BeginList();
        const uint32_t syncMode = (chcr >> 9) & 3;
        if (syncMode == 2) {
            for (uint32_t guard = 0; guard < 0x100000; guard++) {
                uint32_t header = ramWord(address);
                for (uint32_t i = 1; i <= (header >> 24); i++) {
                    report.gp0Words++;
                    { const uint32_t w = ramWord(address + i * 4); gpu.WriteGp0(w); if (sceneCapture_) hudGpu.WriteGp0(w); if (onGpuWord) onGpuWord(false, w); }
                }
                if (header & 0x800000) break;
                address = header & 0x1FFFFC;
            }
        } else {
            uint32_t words = syncMode == 1 ? (bcr & 0xFFFF) * (bcr >> 16) : (bcr & 0xFFFF);
            for (uint32_t i = 0; i < words; i++) {
                report.gp0Words++;
                { const uint32_t w = ramWord(address + i * 4); gpu.WriteGp0(w); if (sceneCapture_) hudGpu.WriteGp0(w); if (onGpuWord) onGpuWord(false, w); }
            }
        }
    }
    if (ch == 2 && sceneCapture_) hudGpu.EndList();
    // Every transfer completes instantly.
    dma_[ch][2] &= ~((1u << 24) | (1u << 28));
    if ((dicr_ & (1u << (16 + ch))) && (dicr_ & (1u << 23))) {
        dicr_ |= (1u << (24 + ch)) | (1u << 31);
        RaiseIrq(kIrqDma);
    }
}

// ---------------------------------------------------------------- HLE kernel

bool Machine::OnException(R3000::Exception cause, uint32_t epc) {
    uint32_t& sr = cpu.cop0[12];
    if (cause == R3000::kSyscall) {
        switch (cpu.gpr[4]) {
        case 0: break;
        case 1: // EnterCriticalSection
            cpu.gpr[2] = (sr & 0x401) == 0x401 ? 1 : 0;
            sr &= ~0x401u;
            break;
        case 2: sr |= 0x401; break; // ExitCriticalSection
        default: stopReason_ = "unimplemented syscall " + std::to_string(cpu.gpr[4]) + " at " + Hex(epc); break;
        }
        return true;
    }
    if (cause == R3000::kInterrupt) {
        if (!entryHook_) {
            stopReason_ = "interrupt (I_STAT " + Hex(iStat_) + ") with no entry hook installed";
            return true;
        }
        report.interruptsDelivered++;
        saved_.gpr = cpu.gpr;
        saved_.hi = cpu.hi;
        saved_.lo = cpu.lo;
        saved_.epc = epc;
        saved_.sr = sr;
        saved_.valid = true;
        sr = (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu);
        // Kernel events that became due (memory card outcomes) are delivered from interrupt context.
        if ((iStat_ & 1) && !pendingEvents_.empty()) {
            auto due = std::move(pendingEvents_);
            pendingEvents_.clear();
            for (const auto& [cls, spec] : due)
                for (size_t i = 0; i < events_.size() && stopReason_.empty(); i++) {
                    Event& e = events_[i];
                    if (!e.open || !e.enabled || e.cls != cls || e.spec != spec) continue;
                    if (e.mode == 0x1000 && e.handler) CallGuest(e.handler, 0);
                    else e.fired = true;
                }
        }
        // Handlers registered with SysEnqIntRP run first: {next, handler, verifier}; the handler is called
        // with the verifier's result when that is non-zero.
        for (const auto& chain : interruptChains_)
            for (size_t i = 0; i < chain.size() && stopReason_.empty(); i++) {
                const uint32_t element = chain[i];
                const uint32_t handler = bus.Read(element + 4, 4), verifier = bus.Read(element + 8, 4);
                const uint32_t verdict = verifier ? CallGuest(verifier, 0) : 0;
                if (verdict && handler) CallGuest(handler, verdict);
            }
        if (!stopReason_.empty()) return true;
        // The kernel's exception handler ends in longjmp(entryHook, 1): ra, sp, fp, s0-s7, gp.
        auto w = [&](uint32_t i) { return bus.Read(entryHook_ + i * 4, 4); };
        cpu.gpr[31] = w(0);
        cpu.gpr[29] = w(1);
        cpu.gpr[30] = w(2);
        for (uint32_t i = 0; i < 8; i++) cpu.gpr[16 + i] = w(3 + i);
        cpu.gpr[28] = w(11);
        cpu.gpr[2] = 1;
        cpu.SetPc(cpu.gpr[31]);
        return true;
    }
    stopReason_ = "guest exception " + std::to_string(cause) + " at " + Hex(epc);
    return true;
}

std::string Machine::GuestString(uint32_t address, size_t maxLength) {
    std::string s;
    for (size_t i = 0; i < maxLength; i++) {
        char c = char(bus.Read(address + uint32_t(i), 1));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

std::string Machine::FormatGuestPrintf() {
    const std::string fmt = GuestString(cpu.gpr[4], 1024);
    uint32_t argIndex = 1;
    auto nextArg = [&]() {
        uint32_t v = argIndex < 4 ? cpu.gpr[4 + argIndex] : bus.Read(cpu.gpr[29] + argIndex * 4, 4);
        argIndex++;
        return v;
    };
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        std::string spec = "%";
        for (i++; i < fmt.size() && std::strchr("-+ #0123456789.l", fmt[i]); i++)
            if (fmt[i] != 'l') spec.push_back(fmt[i]);
        if (i >= fmt.size()) break;
        char buf[128];
        switch (fmt[i]) {
        case '%': out.push_back('%'); break;
        case 's': out += GuestString(nextArg()); break;
        case 'c': out.push_back(char(nextArg())); break;
        case 'd': case 'i': spec.push_back('d'); std::snprintf(buf, sizeof(buf), spec.c_str(), int32_t(nextArg())); out += buf; break;
        case 'u': case 'x': case 'X': case 'o': spec.push_back(fmt[i]); std::snprintf(buf, sizeof(buf), spec.c_str(), nextArg()); out += buf; break;
        case 'p': std::snprintf(buf, sizeof(buf), "%08X", nextArg()); out += buf; break;
        default: out += spec; out.push_back(fmt[i]); break;
        }
    }
    return out;
}

// BIOS file API restricted to the memory card device ("bu00:NAME"). Operations complete immediately;
// callers that asked for O_NOWAIT (0x8000) additionally get the EvSpIOE card events.
bool Machine::CardFileCall(uint32_t id) {
    auto& v0 = cpu.gpr[2];
    const uint32_t a0 = cpu.gpr[4], a1 = cpu.gpr[5], a2 = cpu.gpr[6];
    constexpr uint32_t kFail = 0xFFFFFFFFu;
    int slot = 0;
    auto cardName = [&](uint32_t address, std::string& name) { // "bu00:NAME" (slot 1) / "bu10:NAME" (slot 2) -> NAME
        const std::string full = GuestString(address);
        if (full.size() < 5 || full.compare(0, 2, "bu") != 0 || full[4] != ':') return false;
        name = full.substr(5);
        if (full[2] != '0' && full[2] != '1') return false;
        slot = full[2] - '0';
        return cards_[size_t(slot)] != nullptr;
    };
    auto signalDone = [&] {
        pendingEvents_.push_back({0xF4000001u, 0x0004u});
        pendingEvents_.push_back({0xF0000011u, 0x0004u});
    };
    auto fillDirEntry = [&](uint32_t address, const MemoryCard::DirEntry& e) {
        for (uint32_t i = 0; i < 20; i++) bus.Write(address + i, i < e.name.size() ? uint8_t(e.name[i]) : 0, 1);
        bus.Write(address + 0x14, 0x50, 4);             // attribute: regular file
        bus.Write(address + 0x18, e.size, 4);
        bus.Write(address + 0x1C, 0, 4);
        bus.Write(address + 0x20, uint32_t(e.firstBlock), 4);
    };
    auto matches = [](const std::string& pattern, const std::string& name) {
        size_t i = 0;
        for (; i < pattern.size(); i++) {
            if (pattern[i] == '*') return true;
            if (i >= name.size() || (pattern[i] != '?' && pattern[i] != name[i])) return false;
        }
        return i == name.size();
    };

    switch (id) {
    case 0xB032: { // open(name, mode)
        std::string name;
        v0 = kFail;
        if (!cardName(a0, name)) return true;
        MemoryCard& card = *cards_[size_t(slot)];
        if (a1 & 0x200) card.Create(name, int(a1 >> 16));
        auto entry = card.Find(name);
        if (!entry) return true;
        for (size_t fd = 0; fd < files_.size(); fd++)
            if (!files_[fd].used) {
                files_[fd] = {true, *entry, 0, slot};
                v0 = uint32_t(fd);
                break;
            }
        if (a1 & 0x200) card.Flush();
        if (a1 & 0x8000) signalDone();
        return true;
    }
    case 0xB033: { // lseek(fd, offset, whence)
        if (a0 >= files_.size() || !files_[a0].used) { v0 = kFail; return true; }
        OpenFile& f = files_[a0];
        f.position = a2 == 0 ? a1 : a2 == 1 ? f.position + a1 : f.entry.size + a1;
        v0 = f.position;
        return true;
    }
    case 0xB034: case 0xB035: { // read / write(fd, buffer, length)
        if (a0 >= files_.size() || !files_[a0].used) { v0 = kFail; return true; }
        OpenFile& f = files_[a0];
        if (!cards_[size_t(f.slot)]) { v0 = kFail; return true; }
        MemoryCard& card = *cards_[size_t(f.slot)];
        std::vector<uint8_t> buffer(a2);
        if (id == 0xB035) {
            for (uint32_t i = 0; i < a2; i++) buffer[i] = uint8_t(bus.Read(a1 + i, 1));
            v0 = uint32_t(card.Write(f.entry, f.position, buffer.data(), buffer.size()));
            card.Flush();
        } else {
            v0 = uint32_t(card.Read(f.entry, f.position, buffer.data(), buffer.size()));
            for (uint32_t i = 0; i < v0; i++) bus.Write(a1 + i, buffer[i], 1);
        }
        f.position += v0;
        signalDone();
        return true;
    }
    case 0xB036: // close(fd)
        if (a0 < files_.size()) files_[a0].used = false;
        v0 = a0;
        return true;
    case 0xB041: { // format(device)
        std::string name;
        v0 = cardName(a0, name) ? 1 : 0;
        if (v0) { cards_[size_t(slot)]->Format(); cards_[size_t(slot)]->Flush(); }
        return true;
    }
    case 0xB042: { // firstfile(pattern, direntry)
        std::string pattern;
        v0 = 0;
        fileSearch_.clear();
        if (!cardName(a0, pattern)) return true;
        for (const auto& e : cards_[size_t(slot)]->List())
            if (matches(pattern, e.name)) fileSearch_.push_back(e);
        [[fallthrough]];
    }
    case 0xB043: { // nextfile(direntry)
        const uint32_t dir = id == 0xB042 ? a1 : a0;
        v0 = 0;
        if (fileSearch_.empty()) return true;
        fillDirEntry(dir, fileSearch_.front());
        fileSearch_.erase(fileSearch_.begin());
        v0 = dir;
        return true;
    }
    case 0xB045: { // delete(name)
        std::string name;
        v0 = cardName(a0, name) && cards_[size_t(slot)]->Delete(name) ? 1 : 0;
        if (v0) cards_[size_t(slot)]->Flush();
        return true;
    }
    default: return false;
    }
}

bool Machine::BiosCall(uint32_t table, uint32_t function) {
    auto& v0 = cpu.gpr[2];
    const uint32_t a0 = cpu.gpr[4], a1 = cpu.gpr[5], a2 = cpu.gpr[6], a3 = cpu.gpr[7];
    const uint32_t id = (table << 8) | function;
    auto event = [&](uint32_t handle) -> Event* {
        uint32_t index = handle & 0xFFFF;
        return (handle >> 24) == 0xF1 && index < events_.size() ? &events_[index] : nullptr;
    };

    switch (id) {
    case 0xA03F: report.guestLog.push_back(FormatGuestPrintf()); return true; // printf
    case 0xA03E: case 0xB03F: report.guestLog.push_back(GuestString(a0)); return true; // puts
    case 0xB03D: {                                                             // putchar
        if (report.guestLog.empty() || report.guestLog.back().ends_with('\n')) report.guestLog.emplace_back();
        report.guestLog.back().push_back(char(a0));
        return true;
    }
    // libc routines the BIOS exports (A0 table).
    case 0xA028: for (uint32_t i = 0; i < a1; i++) bus.Write(a0 + i, 0, 1); v0 = a0; return true;                    // bzero
    case 0xA027: for (uint32_t i = 0; i < a2; i++) bus.Write(a1 + i, bus.Read(a0 + i, 1), 1); return true;           // bcopy(src, dst, n)
    case 0xA029: {                                                                                                  // bcmp
        v0 = 0;
        for (uint32_t i = 0; i < a2 && v0 == 0; i++) v0 = bus.Read(a0 + i, 1) != bus.Read(a1 + i, 1);
        return true;
    }
    case 0xA02C: {                                                                                                  // memmove
        std::vector<uint8_t> tmp(a2);
        for (uint32_t i = 0; i < a2; i++) tmp[i] = uint8_t(bus.Read(a1 + i, 1));
        for (uint32_t i = 0; i < a2; i++) bus.Write(a0 + i, tmp[i], 1);
        v0 = a0;
        return true;
    }
    case 0xA02E: {                                                                                                  // memchr
        v0 = 0;
        for (uint32_t i = 0; i < a2 && !v0; i++) if (bus.Read(a0 + i, 1) == (a1 & 0xFF)) v0 = a0 + i;
        return true;
    }
    case 0xA025: v0 = (a0 >= 'a' && a0 <= 'z') ? a0 - 32 : a0; return true; // toupper
    case 0xA026: v0 = (a0 >= 'A' && a0 <= 'Z') ? a0 + 32 : a0; return true; // tolower
    case 0xA015: {                                                          // strcat
        std::string s = GuestString(a1, 65536);
        uint32_t end = a0 + uint32_t(GuestString(a0, 65536).size());
        for (size_t i = 0; i <= s.size(); i++) bus.Write(end + uint32_t(i), uint8_t(s.c_str()[i]), 1);
        v0 = a0;
        return true;
    }
    case 0xA01A: {                                                          // strncpy
        std::string s = GuestString(a1, a2);
        for (uint32_t i = 0; i < a2; i++) bus.Write(a0 + i, i < s.size() ? uint8_t(s[i]) : 0, 1);
        v0 = a0;
        return true;
    }
    case 0xA01C: case 0xA01E: {                                             // index / strchr
        std::string s = GuestString(a0, 65536);
        size_t at = s.find(char(a1));
        v0 = (a1 & 0xFF) == 0 ? a0 + uint32_t(s.size()) : at == std::string::npos ? 0 : a0 + uint32_t(at);
        return true;
    }
    case 0xA02A: for (uint32_t i = 0; i < a2; i++) bus.Write(a0 + i, bus.Read(a1 + i, 1), 1); v0 = a0; return true;  // memcpy
    case 0xA02B: for (uint32_t i = 0; i < a2; i++) bus.Write(a0 + i, a1 & 0xFF, 1); v0 = a0; return true;            // memset
    case 0xA02D: {                                                                                                  // memcmp
        v0 = 0;
        for (uint32_t i = 0; i < a2 && v0 == 0; i++) v0 = uint32_t(int32_t(bus.Read(a0 + i, 1)) - int32_t(bus.Read(a1 + i, 1)));
        return true;
    }
    case 0xA01B: v0 = uint32_t(GuestString(a0, 65536).size()); return true; // strlen
    case 0xA019: {                                                          // strcpy
        std::string s = GuestString(a1, 65536);
        for (size_t i = 0; i <= s.size(); i++) bus.Write(a0 + uint32_t(i), uint8_t(s.c_str()[i]), 1);
        v0 = a0;
        return true;
    }
    case 0xA017: v0 = uint32_t(GuestString(a0, 65536).compare(GuestString(a1, 65536))); return true; // strcmp
    case 0xA018: v0 = uint32_t(GuestString(a0, a2).compare(GuestString(a1, a2))); return true;       // strncmp
    case 0xA02F: randSeed_ = randSeed_ * 0x41C64E6Du + 0x3039u; v0 = (randSeed_ >> 16) & 0x7FFF; return true; // rand
    case 0xA030: randSeed_ = a0; return true;                                                                 // srand
    // GPU helpers of the BIOS: thin wrappers around the GP0/GP1 ports.
    case 0xA048: IoWrite(0x1F801814, a0, 4); return true;                                             // SendGP1Command
    case 0xA049: IoWrite(0x1F801810, a0, 4); v0 = 0; return true;                                     // GPU_cw
    case 0xA04A: for (uint32_t i = 0; i < a1; i++) IoWrite(0x1F801810, bus.Read(a0 + i * 4, 4), 4); return true; // GPU_cwp
    case 0xA046: {                                                                                    // GPU_dw(x, y, w, h, src)
        const uint32_t src = bus.Read(cpu.gpr[29] + 16, 4), words = (a2 * a3 + 1) / 2;
        IoWrite(0x1F801810, 0xA0000000u, 4);
        IoWrite(0x1F801810, (a1 << 16) | (a0 & 0xFFFF), 4);
        IoWrite(0x1F801810, (a3 << 16) | (a2 & 0xFFFF), 4);
        for (uint32_t i = 0; i < words; i++) IoWrite(0x1F801810, bus.Read(src + i * 4, 4), 4);
        return true;
    }
    case 0xA04D: v0 = IoRead(0x1F801814, 4); return true; // GetGPUStatus
    case 0xA04E: v0 = 0; return true;                     // GPU_sync: transfers complete instantly here
    case 0xA044: return true;             // FlushCache
    case 0xA072: return true;             // CdRemove
    case 0xB019: entryHook_ = a0; return true; // HookEntryInt
    case 0xB018: entryHook_ = 0; return true;  // ResetEntryInt
    case 0xB017: {                             // ReturnFromException
        if (!saved_.valid) { stopReason_ = "ReturnFromException without a saved context"; return true; }
        cpu.gpr = saved_.gpr;
        cpu.hi = saved_.hi;
        cpu.lo = saved_.lo;
        cpu.cop0[12] = saved_.sr;
        cpu.SetPc(saved_.epc);
        saved_.valid = false;
        UpdateIrqLine();
        return true;
    }
    case 0xB04A: case 0xB04B: case 0xB04C: v0 = 1; return true; // InitCARD / StartCARD / StopCARD (no card inserted)
    case 0xA070: return true;                                   // _bu_init
    case 0xB012:                                                // InitPAD(buf1, len1, buf2, len2)
        padBuffer_ = {a0, a2};
        padLength_ = {a1, a3};
        v0 = 1;
        return true;
    case 0xB013: padsStarted_ = true; v0 = 1; return true;  // StartPAD
    case 0xB014: padsStarted_ = false; v0 = 1; return true; // StopPAD
    // PsyQ libcard/libpad patch kernel code through these tables. Our kernel is HLE, so the tables live in
    // the (otherwise unused) kernel RAM and every entry points at a scratch area that is never executed.
    case 0xB056: case 0xB057: {
        const uint32_t tableAddress = function == 0x56 ? 0x00000674u : 0x00000874u, scratch = 0x00001000u;
        for (uint32_t i = 0; i < 0x60; i++)
            if (bus.Read(tableAddress + i * 4, 4) == 0) bus.Write(tableAddress + i * 4, scratch + i * 0x80, 4);
        kernelTablesRequested_ = true;
        v0 = tableAddress;
        return true;
    }
    // Memory card requests are asynchronous: the call returns 1 and the outcome arrives as a kernel event.
    // With no card inserted the outcome is EvSpTIMOUT (0x0100) on both card event classes.
    case 0xA0AB: case 0xA0AC: case 0xA0AD: case 0xB04E: case 0xB04F: { // _card_info, _card_load, _card_auto, write/read sector
        const uint32_t port = (a0 >> 4) & 0xF; // port number 0x00 (slot 1) / 0x10 (slot 2)
        MemoryCard* card = port < 2 ? cards_[port].get() : nullptr;
        const bool present = card != nullptr;
        if (present && id == 0xB04E) { // _card_write(port, frame, src)
            uint8_t frame[MemoryCard::kFrameSize];
            for (uint32_t i = 0; i < MemoryCard::kFrameSize; i++) frame[i] = uint8_t(bus.Read(a2 + i, 1));
            card->WriteFrame(a1, frame);
            card->Flush();
        } else if (present && id == 0xB04F) { // _card_read(port, frame, dst)
            uint8_t frame[MemoryCard::kFrameSize];
            card->ReadFrame(a1, frame);
            for (uint32_t i = 0; i < MemoryCard::kFrameSize; i++) bus.Write(a2 + i, frame[i], 1);
        }
        const uint32_t outcome = present ? 0x0004u : 0x0100u; // EvSpIOE / EvSpTIMOUT
        pendingEvents_.push_back({0xF4000001u, outcome});
        pendingEvents_.push_back({0xF0000011u, outcome});
        v0 = 1;
        return true;
    }
    case 0xB032: case 0xB033: case 0xB034: case 0xB035: case 0xB036: case 0xB041: case 0xB042: case 0xB043: case 0xB045:
        return CardFileCall(id);
    case 0xB050: v0 = 0; return true; // allow_new_card
    case 0xB05C: v0 = 0x11; return true; // _card_status: no card
    // Krom2RawAdd(sjis): address of a kanji glyph in the BIOS ROM font. We have no ROM font yet, so answer
    // with the documented "no such character" value. OPEN: memory-card screen text will be missing.
    case 0xB051: v0 = 0xFFFFFFFFu; report.guestLog.push_back("[hle] Krom2RawAdd(" + Hex(a0) + ") -> not found"); return true;
    case 0xC002: // SysEnqIntRP(priority, element)
        if (a0 < 4) interruptChains_[a0].insert(interruptChains_[a0].begin(), a1);
        v0 = 0;
        return true;
    case 0xC003: // SysDeqIntRP(priority, element)
        if (a0 < 4) std::erase(interruptChains_[a0], a1);
        v0 = 0;
        return true;
    case 0xB05B: v0 = 0; return true; // ChangeClearPAD
    case 0xC00A: v0 = 0; return true; // ChangeClearRCnt
    case 0xB008: {                    // OpenEvent(class, spec, mode, func)
        events_.push_back({a0, a1, a2, a3, true, false, false});
        v0 = 0xF1000000u | uint32_t(events_.size() - 1);
        return true;
    }
    case 0xB009: if (Event* e = event(a0)) e->open = false; v0 = 1; return true;                  // CloseEvent
    case 0xB00C: if (Event* e = event(a0)) e->enabled = true; v0 = 1; return true;                // EnableEvent
    case 0xB00D: if (Event* e = event(a0)) e->enabled = false; v0 = 1; return true;               // DisableEvent
    case 0xB00B: {                                                                                // TestEvent
        Event* e = event(a0);
        v0 = e && e->fired ? 1 : 0;
        if (e) e->fired = false;
        return true;
    }
    default: return false;
    }
}

} // namespace gt2
