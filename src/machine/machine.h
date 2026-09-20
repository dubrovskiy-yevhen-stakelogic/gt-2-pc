#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "guest/bus.h"
#include "interp/gte.h"
#include "interp/r3000.h"
#include "machine/cdrom.h"
#include "machine/gpu.h"
#include "machine/mdec.h"
#include "machine/memcard.h"
#include "machine/spu.h"

namespace gt2 {

// The guest machine: CPU + RAM + the narrow hardware boundary the game touches + an HLE kernel that
// answers BIOS calls on the host. Everything unknown is recorded in `report` instead of being guessed.
class Machine {
public:
    static constexpr uint32_t kInstructionsPerVBlank = 282'240; // 33.8688 MHz / 60 Hz at ~2 cycles per instruction

    enum Irq : uint32_t { kIrqVBlank = 0, kIrqGpu = 1, kIrqCdrom = 2, kIrqDma = 3, kIrqTimer0 = 4, kIrqTimer1 = 5,
                          kIrqTimer2 = 6, kIrqSio = 7, kIrqSpu = 9 };

    Machine();

    Bus bus;
    Gte gte;
    Gpu gpu;
    Spu spu;
    Mdec mdec;
    R3000 cpu;

    void AttachDisc(const DiscImage* disc) {
        cdrom_ = std::make_unique<Cdrom>(disc);
        cdrom_->onXaSector = [this](const uint8_t* raw) {
            spu.PushXaSector(raw);
            if (onXaSector) onXaSector(raw);
        };
    }
    const Cdrom* Disc() const { return cdrom_.get(); }
    // Memory card in slot 1 (slot 0) or slot 2 (slot 1), backed by a raw .mcd image (created when missing). Without it the
    // slot is empty. The BIOS card calls name the slot by port 0x00 / 0x10 and the file API by "bu00:" / "bu10:".
    void AttachMemoryCard(const std::string& path, int slot = 0) { cards_[size_t(slot & 1)] = std::make_unique<MemoryCard>(path); }
    void LoadExe(const std::vector<uint8_t>& exe, uint32_t stackTop);

    // Runs until the budget is exhausted or the guest needs something we do not provide.
    // Returns the reason it stopped.
    std::string Run(uint64_t maxInstructions);

    // Native-scene support: a second, HUD-only GPU and per-frame GTE capture. `onSceneFrame` fires at every
    // display flip with the transforms of the game frame whose 2D layer has just been completed.
    struct SceneFrame {
        std::vector<Gte::CapturedTransform> transforms;
        std::vector<Gte::CapturedVertex> vertices;
    };
    Gpu hudGpu;
    std::function<void(const SceneFrame& scene)> onSceneFrame;
    void EnableSceneCapture();

    bool traceIo = false;   // per-register I/O statistics in `report` (slow: only for boot-trace surveys)
    bool traceSio = false;  // dev log of every SIO0 / I_STAT.7 / timer 2 access with pc into report.sioLog
    uint16_t padButtons = 0; // port 1 digital pad, bit set = pressed (PS1 bit order: select..., see psx-spx)
    // Port 2: a second digital pad (2 player Battle captures); not connected unless set (the default runs see an empty port).
    bool pad2Connected = false;
    uint16_t pad2Buttons = 0;

    void RaiseIrq(Irq line) { iStat_ |= 1u << line; UpdateIrqLine(); }

    // Observer of the XA-ADPCM sectors the CD-ROM hands to the SPU (raw 2352 bytes; the movie check).
    std::function<void(const uint8_t* rawSector)> onXaSector;

    // GP0/GP1 words in submission order (from direct writes and DMA channel 2).
    std::function<void(bool gp1, uint32_t word)> onGpuWord;

    struct Report {
        std::map<std::string, uint64_t> biosCalls;
        std::vector<std::string> biosFirstUse;
        std::map<uint32_t, std::pair<uint64_t, uint64_t>> io; // physical address -> reads, writes
        std::vector<std::string> ioFirstUse;
        std::vector<std::string> guestLog;                     // printf/puts output of the game
        std::vector<std::string> sioLog;                       // traceSio output (repeated polls are coalesced)
        uint64_t vblanks = 0, interruptsDelivered = 0, gp0Words = 0, gp1Words = 0;
        std::map<uint32_t, uint64_t> dmaTransfers;             // channel -> count
    } report;

private:
    uint32_t IoRead(uint32_t address, int size);
    void IoWrite(uint32_t address, uint32_t value, int size);
    void UpdateIrqLine();
    void RunDma(uint32_t channel);

    void StepGuest();                                  // one instruction or one HLE BIOS call
    uint32_t CallGuest(uint32_t function, uint32_t a0); // nested call on the kernel stack, returns v0
    bool OnException(R3000::Exception cause, uint32_t epc);
    bool BiosCall(uint32_t table, uint32_t function); // false: unimplemented
    std::string GuestString(uint32_t address, size_t maxLength = 256);
    std::string FormatGuestPrintf();

    // Interrupt controller, DMA, misc register file.
    uint32_t iStat_ = 0, iMask_ = 0;
    uint32_t dpcr_ = 0, dicr_ = 0;
    std::array<std::array<uint32_t, 3>, 7> dma_{}; // MADR, BCR, CHCR
    std::array<std::array<uint32_t, 3>, 3> timer_{}; // count (unused), mode, target
    std::vector<uint8_t> regFile_;                  // plain storage for registers without behaviour yet
    uint64_t nextVBlank_ = kInstructionsPerVBlank;
    uint32_t gpuStatReads_ = 0;
    uint64_t nextSpuBatch_ = 0;
    bool sceneCapture_ = false;
    SceneFrame pendingScene_; // captured at the previous flip, waiting for its 2D layer
    uint64_t nextEvent_ = 0; // earliest instruction count at which TickDevices has work; 0 = re-evaluate
    std::array<std::unique_ptr<MemoryCard>, 2> cards_;
    struct OpenFile { bool used = false; MemoryCard::DirEntry entry; uint32_t position = 0; int slot = 0; };
    std::array<OpenFile, 16> files_;
    std::vector<MemoryCard::DirEntry> fileSearch_; // remaining matches of firstfile/nextfile
    bool CardFileCall(uint32_t id);                // BIOS file API on "buXY:" names; false = not a file call
    std::unique_ptr<Cdrom> cdrom_;

    // HLE kernel state.
    uint32_t entryHook_ = 0; // jmp_buf registered through HookEntryInt
    struct SavedContext {
        std::array<uint32_t, 32> gpr{};
        uint32_t hi = 0, lo = 0, epc = 0, sr = 0;
        bool valid = false;
    } saved_;
    struct Event { uint32_t cls = 0, spec = 0, mode = 0, handler = 0; bool open = false, enabled = false, fired = false; };
    std::vector<Event> events_;
    std::vector<std::pair<uint32_t, uint32_t>> pendingEvents_; // (class, spec) to deliver at the next VBlank interrupt
    std::array<uint32_t, 2> padBuffer_{}, padLength_{};
    bool padsStarted_ = false, kernelTablesRequested_ = false;
    uint32_t randSeed_ = 0;
    std::array<std::vector<uint32_t>, 4> interruptChains_; // SysEnqIntRP elements per priority (guest addresses)
    // SIO0 (controller / memory card port): digital pad in port 1, optionally a second one in port 2 (pad2Connected).
    struct Sio {
        uint16_t mode = 0, ctrl = 0, baud = 0;
        uint8_t rx = 0xFF;
        bool rxReady = false, irq = false;
        int step = 0;              // position inside the pad transfer sequence, 0 = idle
        bool port2 = false;        // the transfer in progress addresses port 2
        uint64_t ackDue = UINT64_MAX;
    } sio_;
    uint8_t SioExchange(uint8_t tx, bool& ack);
    uint8_t SioExchangeStep(uint8_t tx, bool& ack);
    void SioTrace(const char* what, uint32_t address, uint32_t value);
    std::string sioTraceLast_, sioTraceTx_, sioTraceRx_;
    uint64_t sioTraceRepeats_ = 0;
    void TickDevices();

    std::string stopReason_;
};

} // namespace gt2
