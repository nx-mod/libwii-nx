// /dev/di: the disc drive, as IOS serves it.
//
// A title's own DVD low-level library (DVDLowInit, the cover and status
// registers, resets, seeks) talks to the drive through this device; with the
// device here that library runs as it is, in every title, with nothing per
// title to locate. Reads go to libdol-nx's disc index (the same files its
// DVDLowRead replacement serves). A title with no disc behind it - the Wii
// Menu, a channel - sees an empty drive: cover open, no medium.
//
// Command numbers, arguments and results follow Dolphin's IOS/DI/DI.cpp
// (GPL-2.0-or-later, Copyright 2008 Dolphin Emulator Project).
#include "hle_stubs.h"
#include "memory.h"
#include "../nand/nand_internal.h"

#include <cstdint>
#include <cstring>

extern "C" bool DVD_HLE_DiscPresent();
extern "C" void DVDInit_8015EA1C();
extern "C" int32_t DVDLowReadDiskID_80164AAC(uint32_t diskIdPtr, uint32_t callback);
extern "C" int32_t DVDLowRead_80166330(uint32_t buffer, uint32_t length, uint32_t offset, uint32_t callback);

namespace {

constexpr uint32_t kDiFd = 0x44490000;  // "DI"

enum : uint32_t {
    DI_INQUIRY = 0x12,
    DI_READ_DISK_ID = 0x70,
    DI_READ = 0x71,
    DI_WAIT_FOR_COVER_CLOSE = 0x79,
    DI_GET_COVER_REGISTER = 0x7A,
    DI_NOTIFY_RESET = 0x7E,
    DI_CLEAR_COVER_INTERRUPT = 0x86,
    DI_GET_COVER_STATUS = 0x88,
    DI_RESET = 0x8A,
    DI_UNENCRYPTED_READ = 0x8D,
    DI_GET_STATUS_REGISTER = 0x95,
    DI_GET_CONTROL_REGISTER = 0x96,
    DI_REQUEST_ERROR = 0xE0,
};

// what an ioctl answers (the callback's first argument)
enum : int32_t {
    DI_SUCCESS = 1,
    DI_DRIVE_ERROR = 2,
};

// the drive's error code with no medium in it
constexpr uint32_t kErrorNoMedium = 0x01023A00;

uint32_t g_lastError = 0;
uint32_t g_reported[8];  // commands already logged as unknown, a bit each

void Answer(uint32_t out, uint32_t outLen, uint32_t value) {
    if (out && outLen >= 4) {
        Memory::Write32(out, value);
    }
}

int32_t Fail(uint32_t error) {
    g_lastError = error;
    return DI_DRIVE_ERROR;
}

}  // namespace

extern "C" int32_t Di_HLE_Open(const char* path) {
    if (std::strcmp(path, "/dev/di") != 0) {
        return 0;
    }
    // (the title's disc, indexed once: what reads through here are served from)
    DVDInit_8015EA1C();
    g_lastError = 0;
    return static_cast<int32_t>(kDiFd);
}

extern "C" bool Di_HLE_IsFd(uint32_t fd) { return fd == kDiFd; }

// IOS_Ioctl on the drive. `held` is set for a request that only completes
// later (waiting for a disc to be put in an empty drive); its callback is then
// not to be called.
extern "C" int32_t Di_HLE_Ioctl(uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out,
                                uint32_t outLen, bool* held) {
    const bool disc = DVD_HLE_DiscPresent();
    // (the command is also the in buffer's top byte; its arguments follow)
    const uint32_t arg1 = in && inLen >= 8 ? Memory::Read32(in + 4) : 0;
    const uint32_t arg2 = in && inLen >= 12 ? Memory::Read32(in + 8) : 0;

    if (held) {
        *held = false;
    }
    switch (cmd) {
    case DI_INQUIRY:
        if (out && outLen >= 0x20) {
            for (uint32_t offset = 0; offset < 0x20; offset += 4) {
                Memory::Write32(out + offset, 0);
            }
            Memory::Write32(out + 0x00, 0x00000002);  // revision
            Memory::Write32(out + 0x04, 0x20060526);  // drive date
            Memory::Write32(out + 0x08, 0x41000000);
        }
        return DI_SUCCESS;
    case DI_READ_DISK_ID:
        if (!disc) {
            return Fail(kErrorNoMedium);
        }
        DVDLowReadDiskID_80164AAC(out, 0);
        return DI_SUCCESS;
    case DI_READ:
    case DI_UNENCRYPTED_READ:
        // length in bytes, offset in 4-byte words
        if (!disc) {
            return Fail(kErrorNoMedium);
        }
        return DVDLowRead_80166330(out, arg1, arg2 << 2, 0) ? DI_SUCCESS : Fail(kErrorNoMedium);
    case DI_WAIT_FOR_COVER_CLOSE:
        if (!disc && held) {
            *held = true;
        }
        return DI_SUCCESS;
    case DI_GET_COVER_REGISTER:
        // DICVR bit 0: the cover is open, which is how an empty drive reads
        Answer(out, outLen, disc ? 0 : 1);
        return DI_SUCCESS;
    case DI_GET_COVER_STATUS:
        Answer(out, outLen, disc ? 2 : 1);  // 2: a disc is in, 1: none
        return DI_SUCCESS;
    case DI_GET_STATUS_REGISTER:
    case DI_GET_CONTROL_REGISTER:
        Answer(out, outLen, 0);
        return DI_SUCCESS;
    case DI_REQUEST_ERROR:
        Answer(out, outLen, disc ? g_lastError : kErrorNoMedium);
        return DI_SUCCESS;
    case DI_RESET:
        g_lastError = 0;
        return DI_SUCCESS;
    case DI_NOTIFY_RESET:
    case DI_CLEAR_COVER_INTERRUPT:
        return DI_SUCCESS;
    default:
        // the rest (seeks, motor, audio streaming setup) succeed without a drive
        // to move; each is named once so a title that needs one shows it
        if (cmd < 256 && !(g_reported[cmd / 32] & (1u << (cmd % 32)))) {
            g_reported[cmd / 32] |= 1u << (cmd % 32);
            LogNandWarning("/dev/di", "ioctl 0x%02X (args 0x%08X 0x%08X) answered as done", cmd,
                           arg1, arg2);
        }
        return DI_SUCCESS;
    }
}
