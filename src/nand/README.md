# nand

The console's internal memory, as a game sees it: `NANDOpen`, `ISFS_Read`, the
asynchronous forms of both, and the save data behind them.

| File | Is |
|---|---|
| `nand_fs.cpp` | the filesystem a game walks |
| `nand_isfs.cpp` | the IOS service underneath it |
| `nand_api.cpp` | the SDK's own calls |
| `nand_async.cpp` | the same, for a game that will not wait |

What the bytes *mean* - settings, Miis, tickets, titles - is libdol-nx's
`format/nand`, so a launcher can read a NAND without a console running.
