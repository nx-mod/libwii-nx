# ios

The Wii's other operating system, and how a game talks to it.

A Wii game does not touch the disc, the NAND, USB or the network directly: it
sends a request to IOS over IPC and waits. `ios.cpp` is that channel, and
`esp.cpp` is ES, the service that knows which titles exist and what may run.

A GameCube has none of this - a GameCube game owns the machine - which is why
these are here and not in libdol-nx.

Devices answered: ES (`esp.cpp`), `/dev/stm`, `/dev/sdio/slot0` (`sdio.cpp`),
`/dev/di` (`di.cpp`), the network devices, and the NAND's own (`../nand`).
