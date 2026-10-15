# system

The console's settings, as a game reads them: language, aspect ratio, sound
mode, the sensor bar's position - `SCGetLanguage` and its like.

The values come from the NAND's SYSCONF, so every game sees the same console.

Beside them, what the EXI bus carries from the console: `sram.cpp`, its 64
bytes of settings, and `rom.cpp`, the boot ROM's two system fonts (Shift-JIS at
0x1AFF00, Windows-1252 at 0x1FCF00) for `OSInitFont`/`OSLoadFont`. The fonts
come from the SD card: `system/ipl.bin` (a dump of the console's own ROM), or
Dolphin's free replacements as `system/font_western.bin` and
`system/font_japanese.bin`. Without them a game that draws with them can halt
(Bomberman Blast: a `CharWriter` panic).
