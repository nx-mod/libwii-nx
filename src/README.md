# src

What only a Wii has. Everything both consoles share is in
[libdol-nx](https://github.com/nx-mod/libdol-nx).

| Module | Is |
|---|---|
| [`ios`](ios/README.md) | the channel to IOS, and the title service behind it |
| [`nand`](nand/README.md) | the console's internal memory, as a game sees it |
| [`input`](input/README.md) | the Wii Remote, its extensions and its pointer |
| [`system`](system/README.md) | the console's settings, as a game reads them |

These are compiled into the runtime rather than linked as a library of their
own: they answer a game's SDK calls, and that means they use the runtime's
memory and its logging. `cmake/Sources.cmake` is what a build includes.

The Wii's *file formats* - SYSCONF, the Mii database, tickets, TMDs, WADs - are
not here. They are bytes rather than hardware, so they live in libdol-nx's
`format` section, where a tool or a launcher can use them without a console.
