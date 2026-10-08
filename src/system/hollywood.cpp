// Hollywood's registers as the PowerPC sees them (0xCD800000-0xCD8001FF), the
// Wii's own hardware beside the GameCube's: answered here and registered with
// libdol-nx's memory as an MMIO device.
//
// Only the registers SDK and title code touch inline are answered, and all of
// them are plain storage as the PowerPC uses them, as Dolphin keeps them
// (WII_IPC.cpp): a write is kept and read back, 0 at first. Nothing on the
// Switch acts on them. Any other register in the range still traps.
//
//   0x018         PPCSPEED
//   0x024         VISOLID: a solid colour the video output shows instead of the
//                 frame (the Wii Menu's startup reads and writes it)
//   0x03C         the ARM interrupt mask
//   0x070, 0x180, 0x1CC, 0x1D0
//                 configuration bits the Wii Menu's startup sets with
//                 read-modify-writes
//   0x0C0-0x0DC   GPIO B: out, direction, in, interrupt level, flags, mask,
//                 input mirror, owner. It drives the disc slot LED, the sensor
//                 bar and the like; the inputs read 0 (nothing pressed, no disc
//                 ejected).
#include "memory.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace {

constexpr uint32_t kBase = 0xCD800000u;
constexpr uint32_t kSize = 0x200u;
constexpr uint32_t kGpioBIn = 0x0C8u;

bool Answered(uint32_t offset) {
    switch (offset) {
    case 0x018: case 0x024: case 0x03C: case 0x070:
    case 0x180: case 0x1CC: case 0x1D0:
        return true;
    default:
        return offset >= 0x0C0 && offset <= 0x0DC;
    }
}

std::array<std::atomic<uint32_t>, kSize / 4> g_registers{};

bool Read32(uint32_t addr, uint32_t* value) {
    const uint32_t offset = addr - kBase;
    if ((offset & 3u) || !Answered(offset))
        return false;
    *value = g_registers[offset / 4].load(std::memory_order_relaxed);
    return true;
}

bool Write32(uint32_t addr, uint32_t value) {
    const uint32_t offset = addr - kBase;
    if ((offset & 3u) || !Answered(offset))
        return false;
    if (offset != kGpioBIn)
        g_registers[offset / 4].store(value, std::memory_order_relaxed);
    return true;
}

const bool g_registered = [] {
    Memory::RegisterMmioDevice({"hollywood", kBase, kSize, Read32, Write32});
    return true;
}();

} // namespace
