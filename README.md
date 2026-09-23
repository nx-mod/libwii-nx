# libwii-nx

The Wii's own hardware and system software, as a library, for statically
recompiled Wii games running natively on Nintendo Switch.

It holds only what a Wii has and a GameCube does not. Everything both machines
share - the CPU, GX, the DSP, the OS, the SDK's common libraries, the
translator, the disc reader and the Wii's file formats - is in
[libdol-nx](https://github.com/nx-mod/libdol-nx), which this builds on.

## Why it is small

The two consoles are the same machine with different peripherals. Of the
libraries Nintendo shipped in the two SDKs, sixteen appear in both under the
same names and do the same job: `ai ax axfx base card dsp dvd exi gd gf gx mtx
os pad si vi`. Those are libdol-nx's. What is left is the Wii's own.

## Modules

| Module | Is |
|---|---|
| `ipc` | the channel to IOS, which everything below goes through |
| `es` | titles, tickets and the rights to run them |
| `isfs` | the NAND as a filesystem, as a game sees it |
| `nand` | saves, and the console's own files |
| `wpad` / `kpad` | the Wii Remote, its extensions and its pointer |
| `wud` | the Bluetooth stack underneath them |
| `sc` | the console's settings, as a game reads them |
| `arc` | U8 archives |
| `tpl` | texture palettes |
| `mem` | the Wii's expanded memory and its allocators |
| `usb` | devices reached over IOS |

The Wii's *file formats* - SYSCONF, the Mii database, tickets, TMDs, WADs - are
not here. They are bytes, not hardware, so they live in libdol-nx's `format`
section where a tool or a launcher can use them without a console.

## Middleware

Natives for nw4r, EGG and RVLFaceLib are in libdol-nx's `accel` section with the
rest. A native is code keyed to a library build, not to a machine: it binds when
a game links that library and stays dormant when it does not.

## License

GPL-3.0-or-later. No game code or data is included or distributed.
