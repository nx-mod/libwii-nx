# TODO - ios

- [x] A title's own contents: OpenContent, OpenTitleContent, ReadContent,
      SeekContent, CloseContent, shared ones through /shared1
- [x] ES reports the running title's id (the project's `title_id`)
- [x] /dev/stm: shutdown and reset leave the program; the event hook is held
      (answering it is what reports a button press)
- [x] TMD and ticket views, stored TMD and contents, owned titles
- [x] /dev/sdio/slot0 from sd.img beside the NAND (port of Dolphin's SDIOSlot0)
- [ ] The SD card as a folder: build sd.img from sdmc:/wii-nx/sd/ at start and
      write changes back at exit, as Dolphin's FatFsUtil does
- [ ] Enough of ES for a title to launch another one (ES_LaunchTitle), which is
      what the Wii Menu and the Mii Channel do
- [ ] The device tree a game opens by name (`/dev/...`), rather than the handful
      answered today
- [ ] Say plainly when a game asks IOS for something this does not answer
