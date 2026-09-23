# TODO - libwii-nx

Nothing here is written yet: the modules below run today inside
[libdol-nx](https://github.com/nx-mod/libdol-nx) and move here when the console
split lands, which is the same pass that renames the namespaces.

## Move in

- [ ] `ipc` - the channel to IOS
- [ ] `es` - titles, tickets, and permission to launch one
- [ ] `isfs` - the NAND as a game sees it
- [ ] `nand` - saves and the console's own files
- [ ] `wpad` / `kpad` - the Wii Remote, its extensions and its pointer
- [ ] `sc` - the console's settings
- [ ] `mem` - the expanded memory and its allocators

## Write

- [ ] `wud` - the Bluetooth stack under the remotes
- [ ] `arc`, `tpl` - archives and texture palettes, where a game calls the SDK's
      versions rather than its own
- [ ] Motion: the accelerometer and gyro a remote reports, which Wii Sports needs
- [ ] Launching a title from the NAND, so the Mii Channel and the Wii Menu can
      run at all ([titles](https://github.com/nx-mod/libdol-nx/blob/main/docs/titles.md))
