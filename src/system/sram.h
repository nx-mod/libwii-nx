#pragma once

// SRAM: the 64 bytes the console keeps its own settings in.
//
// It sits beside SYSCONF rather than with the EXI transport that carries it,
// because what it holds is console state - screen offset, sound mode, language,
// the RTC's bias - and not a property of the bus. Every Wii and GameCube game
// reads it during OSInit, so nothing here belongs to any one game.

#include <cstddef>
#include <cstdint>

namespace wiinx::system::sram {

// The whole of SRAM, checksummed so the SDK accepts it.
inline constexpr std::size_t kSize = 64;
const std::uint8_t* Image();

// Seconds since 2000-01-01, which is what the RTC counter holds.
std::uint32_t RtcCounter();

}  // namespace wiinx::system::sram
