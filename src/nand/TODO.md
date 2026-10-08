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
- [x] Per-file ownership and permissions, so `GetAttr` reports what `SetAttr`
      was given: kept in `.wiinx_metadata` beside the NAND, since a host
      filesystem has nowhere for a Wii uid, gid or its three modes
- [ ] `ES_Launch`, which is how the System Menu starts a channel

Found bringing up the Wii Menu (2026-10-08):
- [ ] `NANDSetStatus` on `setting.txt` fails with -8 (NAND_RESULT_INVALID): the
      menu logs "Failed to set product info file permission!". ISFS `SetAttr`
      itself is implemented, so the refusal is earlier, in the SDK's own
      `NANDSetStatus` path or in what our `NANDGetStatus` handed it
- [ ] `NANDCreateDir("/shared2/test2")` fails: the menu's own write test of
      `/shared2` ("Failed to create dir")
- [ ] Shadow writes on FAT: a closed file replaces the original
      non-atomically, because a FAT rename cannot overwrite (logged on every
      `NANDClose` of `state.dat` and `cache.dat`)
- [ ] `*.nandsafe.tmp` scratch files left behind after a crash
      (`play_rec.dat.nandsafe.tmp`, `cache.dat.nandsafe.tmp`); they are
      discarded on the next open, but nothing cleans up the rest
- [x] NANDOpen, NANDCreate, NANDCreateDir, NANDGetStatus, NANDGetType and
      NANDMove bound at the public functions, not the SDK helpers in front of
      them (libdol-nx `wiinx-scan`/`wiinx-sign-natives`): the helpers take other
      arguments, so NANDOpen returned 0 for the file descriptor
