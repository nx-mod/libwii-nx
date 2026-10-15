# nand

The console's internal memory, as a game sees it: `NANDOpen`, `ISFS_Read`, the
asynchronous forms of both, and the save data behind them.

| File | Is |
|---|---|
| `nand_fs.cpp` | the filesystem a game walks |
| `nand_isfs.cpp` | the IOS service underneath it |
| `nand_api.cpp` | the SDK's own calls |
| `nand_async.cpp` | the same, for a game that will not wait |

An async call does its work at once and queues the guest's callback. With a
callback it returns `NAND_RESULT_OK`, and the result - error or not - goes
through the callback only, as on the console: a caller that sees an error from
both handles it twice (RFL retried a Mii database format from both and failed
on its own scratch file).

What the bytes *mean* - settings, Miis, tickets, titles - is libdol-nx's
`format/nand`, so a launcher can read a NAND without a console running.

## Where the NAND lives

One NAND, shared by every title, at `sdmc:/wii-nx/system/nand` on the Switch:
the same tree a console has (`title/`, `shared1/`, `shared2/`, `sys/`), with
host files standing in for its inodes. It is the user's own - dumped from their
console or fetched from Nintendo's servers (`libdol-nx/tools/wiinx-fetch-nand`)
- and never part of this repository.

What has to be there for the Wii Menu to start: its own `title/00000001/00000002`
(including `data/setting.txt`, the console's region and serial, which no tool
can make up), `shared2/sys/SYSCONF`, and the shared contents its TMD names.
Ownership and permissions a host filesystem cannot hold are kept in
`.wiinx_metadata` beside it.
