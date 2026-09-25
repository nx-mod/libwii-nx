// SRAM: the 64 bytes the console keeps its own settings in.
//
// Layout and checksum follow libogc (devkitPro), which documents both:
//   gc/ogc/system.h   struct _syssram / _syssramex
//   libogc/system.c   __buildchecksum
// https://github.com/devkitPro/libogc - parts of libogc derive from RTEMS;
// what is taken here is the on-device format rather than any of its code.
//
//   0x00 u16 checksum        0x14 u8  flash_id[2][12]
//   0x02 u16 checksum_inv    0x2C u32 wirelessKbd_id
//   0x04 u32 ead0            0x30 u16 wirelessPad_id[4]
//   0x08 u32 ead1            0x38 u8  dvderr_code
//   0x0C u32 counter_bias    0x39 u8  padding
//   0x10 s8  display_offsetH 0x3A u8  flashID_chksum[2]
//   0x11 u8  ntd             0x3C u16 gbs
//   0x12 u8  lang            0x3E u16 padding
//   0x13 u8  flags

#include "sram.h"

#include <chrono>
#include <cstring>

namespace wiinx::system::sram {
namespace {

constexpr std::size_t kCheckSum = 0x00;       // u16
constexpr std::size_t kCheckSumInv = 0x02;    // u16
constexpr std::size_t kCounterBias = 0x0C;    // u32
constexpr std::size_t kDisplayOffsetH = 0x10; // s8
constexpr std::size_t kNtd = 0x11;            // u8
constexpr std::size_t kLanguage = 0x12;       // u8
constexpr std::size_t kFlags = 0x13;          // u8

// Bits in `flags` that a booting console cares about.
constexpr std::uint8_t kFlagStereo = 0x04;
constexpr std::uint8_t kFlagOobeDone = 0x08;
constexpr std::uint8_t kFlagReserved = 0x20;  // set on every console Dolphin has seen

std::uint16_t ReadBigEndian16(const std::uint8_t* at)
{
    return static_cast<std::uint16_t>((at[0] << 8) | at[1]);
}

void WriteBigEndian16(std::uint8_t* at, std::uint16_t value)
{
    at[0] = static_cast<std::uint8_t>(value >> 8);
    at[1] = static_cast<std::uint8_t>(value);
}

// Four 16-bit words from offset 0x0C - the settings, and nothing else. One sum
// of the words and one of their complements. A game whose SRAM does not add up
// re-reads it rather than reporting anything, so this is taken from references
// rather than from memory, and from two that agree: libogc's __buildchecksum
// sums buffer[6..9], and Dolphin's FixSRAMChecksums runs from rtc_bias to
// settings_ex, which is the same four words.
void WriteCheckSum(std::uint8_t* sram)
{
    std::uint16_t sum = 0;
    std::uint16_t inverted = 0;
    for (std::size_t word = 6; word < 10; ++word) {
        const std::uint16_t value = ReadBigEndian16(sram + word * 2);
        sum = static_cast<std::uint16_t>(sum + value);
        inverted = static_cast<std::uint16_t>(inverted + static_cast<std::uint16_t>(value ^ 0xFFFFu));
    }
    WriteBigEndian16(sram + kCheckSum, sum);
    WriteBigEndian16(sram + kCheckSumInv, inverted);
}

}  // namespace

const std::uint8_t* Image()
{
    static std::uint8_t sram[kSize];
    static const bool built = [] {
        std::memset(sram, 0, sizeof(sram));
        // A console that has been set up already: the clock unbiased, the screen
        // centred, and the SDK's defaults for the rest. What the Wii itself shows
        // the player - language, sound mode, aspect - comes from SYSCONF, which
        // sc.cpp answers; SRAM carries the older GameCube-era copy.
        sram[kCounterBias] = 0;
        sram[kDisplayOffsetH] = 0;
        sram[kNtd] = 0;
        sram[kLanguage] = 0;  // English
        // Dolphin's defaults, which matter: a console whose out-of-box setup is
        // not marked done can be sent back through first-time setup instead of
        // booting what was asked for.
        sram[kFlags] = kFlagReserved | kFlagOobeDone | kFlagStereo;  // 0x2C
        WriteCheckSum(sram);
        return true;
    }();
    (void)built;
    return sram;
}

std::uint32_t RtcCounter()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    constexpr long long kSecondsTo2000 = 946684800LL;
    return seconds > kSecondsTo2000 ? static_cast<std::uint32_t>(seconds - kSecondsTo2000) : 0u;
}

}  // namespace wiinx::system::sram
