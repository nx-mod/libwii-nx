# TODO - libwii-nx

## Done

- `ios` (IPC, ES), `nand` (NAND API, ISFS, async), `input` (WPAD, KPAD, the
  remote's host input), `system` (SC, SRAM): compiled into every Wii build
- Headers only libwii uses: `nand_save_probe.h`, `wii_es_crypto.h`,
  `sc_serial_contract.h`

## Still in libdol-nx, to move here

- `wii_remote_input.h` - libdol's `app` and `platform/input/pad.cpp` include it;
  needs an input interface in libdol first
- `nand_path.h`, `nand_settings.h`, `nand_first_run.h`, `console_identity.h` -
  also used by libdol's `platform/fs/riivolution.cpp` and `platform/net`
- `platform/fs/riivolution.cpp` - disc patching is a Wii thing
- The tests for the above (`tests/runtime/nand_*`, `sc_serial_tests`)

## Write

- `wud` - the Bluetooth stack under the remotes
- Motion: accelerometer and gyro, which Wii Sports needs
- `arc`, `tpl`, `mem`, `usb`
- Launching a title from the NAND (Mii Channel, Wii Menu)
