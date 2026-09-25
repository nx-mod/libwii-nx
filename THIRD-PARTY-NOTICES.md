# Third-Party Notices

libwii-nx is GPL-3.0-or-later. What it is built from or follows:

## Design reference

- **libogc** by shagkur and contributors, devkitPro - the console's SRAM is
  documented by its `gc/ogc/system.h` (`struct _syssram` / `_syssramex`) and its
  `libogc/system.c` (`__buildchecksum`). `src/system/sram.cpp` follows that
  on-device format - the field offsets and the checksum's arithmetic - and is
  written here; no libogc code is copied. <https://github.com/devkitPro/libogc>

  libogc's threading and kernel code derives from RTEMS. Nothing has been taken
  from those parts; anything that is later must credit RTEMS and carry its
  license alongside libogc's.

- **Dolphin** by the Dolphin Emulator Project - GPL-2.0-or-later, and so
  combinable with this library. It implements the side we implement: the console
  answering, rather than a game asking. `src/system/sram.cpp` follows its
  `Source/Core/Core/HW/Sram.cpp` - `FixSRAMChecksums` agrees with libogc on the
  arithmetic, and its `sram_dump` supplied the default flags, which matter:
  a console that does not say its out-of-box setup is done can be sent back
  through first-time setup instead of booting. Written here; no code copied.
  <https://github.com/dolphin-emu/dolphin>

## Rules for new sources

- Take code only from explicitly licensed projects; treat the rest as reference.
- A format or an algorithm that a console defines is a fact about the hardware,
  but the project it was read from is still named here.
