# TODO - nand

- [ ] Serve a title's own `/title/<id>/data`, so a channel can save
- [ ] Shared contents (`/shared1` and its `content.map`), which WiiWare and
      Virtual Console titles need
- [x] Report usage the way a console does: `ISFS_GetUsage` counts a directory's
      inodes and whole clusters, capped at the NAND's own geometry
      (16 KiB clusters, 0x7ec0 total less 0x300 reserved, 0x17ff inodes)
- [ ] Report a NAND that is *full* the way a console does, rather than failing:
      `GetNandStats` (free clusters and inodes), and the write paths refusing
      when there is no room
