# TODO - nand

- [x] Serve a title's own `/title/<id>/data`, so a channel can save:
      `ES_GetTitleDirectory` answers with the path and creates it
- [x] Shared contents (`/shared1` and its `content.map`), which WiiWare and
      Virtual Console titles need: resolved in `TranslateNandPath`, so every
      caller gets it. Mega Man 9 keeps 5 of 11 contents there, the Mii Channel
      3 of 7
- [x] Report usage the way a console does: `ISFS_GetUsage` counts a directory's
      inodes and whole clusters, capped at the NAND's own geometry
      (16 KiB clusters, 0x7ec0 total less 0x300 reserved, 0x17ff inodes)
- [x] Report a NAND that is *full* the way a console does, rather than failing:
      `ISFS_GetStats` reports free clusters and inodes against the real
      geometry, and `IOS_Write` refuses with `ISFS_ENOSPC` rather than letting
      the host disk answer instead

Next:
- [ ] Per-file ownership and permissions, so `GetAttr` reports what `SetAttr`
      was given rather than the same answer for everything
- [ ] `ES_Launch`, which is how the System Menu starts a channel
