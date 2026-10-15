#pragma once

// The boot ROM on EXI channel 0, device 1: what a game reads of it is the two
// system fonts, the Shift-JIS one at 0x1AFF00 and the Windows-1252 one at
// 0x1FCF00, which OSInitFont/OSLoadFont copy out through __ReadROM and NW4R
// draws text with. Nothing else in it is any game's business.
//
// The fonts are not ours to ship. They come from the SD card, either out of a
// dump of the console's own ROM (system/ipl.bin, 2 MiB) or as the two font files
// Dolphin's free replacements are distributed as (system/font_japanese.bin and
// system/font_western.bin, Yay0 like the originals). Without either, a read
// answers zeros, as it did before, and the game's own checks decide what then.

#include <cstddef>
#include <cstdint>

namespace wiinx::system::rom {

// Copies `length` bytes at ROM offset `offset` to `out`; bytes no source covers
// are zero. False when no font source was found at all.
bool Read(std::uint32_t offset, std::uint8_t* out, std::size_t length);

}  // namespace wiinx::system::rom
