// /dev/sdio/slot0: the Wii's front SD slot, backed by an image file.
//
// The Wii's own software runs FAT itself over the slot's block commands, so
// what IOS serves is sectors. They come from sd.img beside the shared NAND
// (sdmc:/wii-nx/sd.img on Switch); with no image there is no card. Mount the
// image on a PC to put files on it.
//
// Ported from Dolphin's IOS/SDIO/SDIOSlot0.cpp (GPL-2.0-or-later, Copyright
// 2008 Dolphin Emulator Project): the command set, the CSD layouts and the
// quirks (responses in reverse word order, CRC never checked) are its.
#include "hle_stubs.h"
#include "memory.h"
#include "../nand/nand_internal.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace {

constexpr uint32_t kSdioFd = 0x53440000;  // "SD"

enum : uint32_t {
    HCR_CLOCKCONTROL = 0x2C,
    HCR_SOFTWARERESET = 0x2F,

    IOCTL_WRITEHCR = 0x01,
    IOCTL_READHCR = 0x02,
    IOCTL_RESETCARD = 0x04,
    IOCTL_SETCLK = 0x06,
    IOCTL_SENDCMD = 0x07,
    IOCTL_GETSTATUS = 0x0B,
    IOCTL_GETOCR = 0x0C,
    IOCTLV_SENDCMD = 0x07,

    CARD_NOT_EXIST = 0,
    CARD_INSERTED = 0x1,
    CARD_INITIALIZED = 0x10000,
    CARD_SDHC = 0x100000,

    GO_IDLE_STATE = 0x00,
    ALL_SEND_CID = 0x02,
    SEND_RELATIVE_ADDR = 0x03,
    SELECT_CARD = 0x07,
    SEND_IF_COND = 0x08,
    SEND_CSD = 0x09,
    SEND_CID = 0x0A,
    SET_BLOCKLEN = 0x10,
    READ_MULTIPLE_BLOCK = 0x12,
    WRITE_MULTIPLE_BLOCK = 0x19,
    APP_CMD_NEXT = 0x37,
    ACMD_SETBUSWIDTH = 0x06,
    ACMD_SENDOPCOND = 0x29,
    EVENT_REGISTER = 0x40,
    EVENT_UNREGISTER = 0x41,

    EVENT_INSERT = 1,
    EVENT_REMOVE = 2,
    EVENT_INVALID = 0xc210000,
};
constexpr int32_t RET_OK = 0;
constexpr int32_t RET_FAIL = 1;
constexpr int32_t RET_EVENT_REGISTER = 2;  // internal: held, not answered
constexpr uint64_t kSdscMaxSize = 0x80000000;

struct Slot {
    std::FILE* card = nullptr;
    uint64_t size = 0;
    uint32_t status = CARD_NOT_EXIST;
    bool v2 = false;
    uint32_t blockLength = 0;
    std::array<uint32_t, 0x200 / 4> registers{};
    // A registered insert/remove event, held until it is true.
    uint32_t eventType = 0;
    uint32_t eventCallback = 0;
    uint32_t eventArg = 0;
    bool eventHeld = false;
} g_slot;

std::filesystem::path ImagePath() { return GetNandBasePath().parent_path() / "sd.img"; }

void OpenCard() {
    if (g_slot.card != nullptr) {
        return;
    }
    const std::filesystem::path path = ImagePath();
    g_slot.card = std::fopen(path.string().c_str(), "r+b");
    if (g_slot.card == nullptr) {
        std::fprintf(stderr, "[SDIO] no SD card: %s is not there\n", path.string().c_str());
        return;
    }
    std::fseek(g_slot.card, 0, SEEK_END);
    g_slot.size = static_cast<uint64_t>(std::ftell(g_slot.card));
    std::fprintf(stderr, "[SDIO] SD card %s, %llu MiB\n", path.string().c_str(),
                 static_cast<unsigned long long>(g_slot.size >> 20));
}

bool Inserted() { return g_slot.card != nullptr; }

void InitSdhc() {
    g_slot.v2 = true;
    g_slot.status |= CARD_INITIALIZED;
}

uint32_t Ocr() {
    uint32_t ocr = 0x00ff8000;
    if (g_slot.status & CARD_INITIALIZED) ocr |= 0x80000000;
    if (g_slot.status & CARD_SDHC) ocr |= 0x40000000;
    return ocr;
}

std::array<uint32_t, 4> CsdV1() {
    uint64_t size = g_slot.size;
    uint32_t readBlLen = 9;
    uint32_t cSizeMult = 0;
    while (size > 4096) {
        size >>= 1;
        if (++cSizeMult > 7 + 2 + readBlLen && ++readBlLen > 15) {
            size = 4096;
            cSizeMult = 7 + 2 + readBlLen;
        }
    }
    cSizeMult -= 2 + readBlLen;
    const uint32_t cSize = static_cast<uint32_t>(size - 1);
    return {{0x000007f0, 0x035b5080 | (readBlLen << 8) | (cSize >> 10),
             0x003ffc7f | (cSize << 22) | (cSizeMult << 7), 0x80040040 | (readBlLen << 18)}};
}

std::array<uint32_t, 4> CsdV2() {
    const uint32_t cSize = static_cast<uint32_t>(g_slot.size / (512 * 1024) - 1);
    return {{0x00400e00, 0x5a5f5900, 0x0000007f | (cSize << 8), 0x800a4000}};
}

uint64_t ByteAddress(uint32_t arg) {
    return (g_slot.status & CARD_SDHC) ? static_cast<uint64_t>(arg) * 512 : arg;
}

void Write32(uint32_t address, uint32_t value) {
    if (address != 0 && Memory::Contains(address, 4)) Memory::Write32(address, value);
}

// Answers a held event once it is true: an insert event while a card is in.
void EventNotify() {
    if (!g_slot.eventHeld) return;
    if ((Inserted() && g_slot.eventType == EVENT_INSERT) ||
        (!Inserted() && g_slot.eventType == EVENT_REMOVE)) {
        NandQueueIosCallback(g_slot.eventCallback, static_cast<int32_t>(g_slot.eventType),
                             g_slot.eventArg);
        g_slot.eventHeld = false;
    }
}

int32_t Execute(uint32_t in, uint32_t out, uint32_t callback, uint32_t callbackArg) {
    if (!Memory::Contains(in, 36)) return RET_FAIL;
    const uint32_t command = Memory::Read32(in + 0);
    const uint32_t arg = Memory::Read32(in + 12);
    const uint32_t blocks = Memory::Read32(in + 16);
    const uint32_t bsize = Memory::Read32(in + 20);
    const uint32_t addr = Memory::Read32(in + 24);
    int32_t ret = RET_OK;

    switch (command) {
    case GO_IDLE_STATE: Write32(out, 0); break;
    case SEND_RELATIVE_ADDR: Write32(out, 0x9f62); break;
    case SELECT_CARD: Write32(out, (arg >> 16) ? 0x700 : 0x900); break;
    case SEND_IF_COND:
        InitSdhc();
        Write32(out, arg);
        break;
    case SEND_CSD: {
        const auto csd = g_slot.v2 ? CsdV2() : CsdV1();
        Write32(out + 12, csd[0]);
        Write32(out + 8, csd[1]);
        Write32(out + 4, csd[2]);
        Write32(out + 0, csd[3]);
        break;
    }
    case ALL_SEND_CID:
    case SEND_CID:
        Write32(out + 12, 0x00D0444F);
        Write32(out + 8, 0x4C504849);
        Write32(out + 4, 0x4E430403);
        Write32(out + 0, 0xAC68006B);
        break;
    case SET_BLOCKLEN:
        g_slot.blockLength = arg;
        Write32(out, 0x900);
        break;
    case APP_CMD_NEXT:
    case ACMD_SETBUSWIDTH: Write32(out, 0x920); break;
    case ACMD_SENDOPCOND: Write32(out, Ocr()); break;
    case READ_MULTIPLE_BLOCK:
    case WRITE_MULTIPLE_BLOCK: {
        const uint32_t size = bsize * blocks;
        const bool write = command == WRITE_MULTIPLE_BLOCK;
        if (!Inserted() || size == 0 || !Memory::Contains(addr, size) ||
            std::fseek(g_slot.card, static_cast<long>(ByteAddress(arg)), SEEK_SET) != 0) {
            ret = RET_FAIL;
        } else {
            uint8_t* data = Memory::GetPointer(addr, size);
            const size_t done = write ? std::fwrite(data, 1, size, g_slot.card)
                                      : std::fread(data, 1, size, g_slot.card);
            if (done != size) ret = RET_FAIL;
            if (write) std::fflush(g_slot.card);
        }
        Write32(out, 0x900);
        break;
    }
    case EVENT_REGISTER:
        g_slot.eventType = arg;
        g_slot.eventCallback = callback;
        g_slot.eventArg = callbackArg;
        g_slot.eventHeld = true;
        ret = RET_EVENT_REGISTER;
        break;
    case EVENT_UNREGISTER:
        if (!g_slot.eventHeld) return -4;  // IPC_EINVAL
        NandQueueIosCallback(g_slot.eventCallback, static_cast<int32_t>(EVENT_INVALID),
                             g_slot.eventArg);
        g_slot.eventHeld = false;
        break;
    default:
        std::fprintf(stderr, "[SDIO] SD command 0x%02X not handled\n", command);
        break;
    }
    return ret;
}

}  // namespace

extern "C" int32_t Sdio_HLE_Open(const char* path) {
    if (std::strcmp(path, "/dev/sdio/slot0") != 0) return 0;
    OpenCard();
    g_slot.registers.fill(0);
    return static_cast<int32_t>(kSdioFd);
}

extern "C" bool Sdio_HLE_IsFd(uint32_t fd) { return fd == kSdioFd; }

extern "C" int32_t Sdio_HLE_Close(uint32_t) {
    g_slot.blockLength = 0;
    return 0;
}

// IOS_Ioctl. `callback` is non-zero for the async form; an event registration
// is then held instead of answered, and `held` says so.
extern "C" int32_t Sdio_HLE_Ioctl(uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out,
                                  uint32_t outLen, uint32_t callback, uint32_t callbackArg,
                                  bool* held) {
    if (held) *held = false;
    if (out != 0 && outLen != 0 && Memory::Contains(out, outLen)) {
        std::memset(Memory::GetPointer(out, outLen), 0, outLen);
    }
    switch (cmd) {
    case IOCTL_WRITEHCR: {
        if (inLen < 20 || !Memory::Contains(in, 20)) return 0;
        const uint32_t reg = Memory::Read32(in);
        const uint32_t val = Memory::Read32(in + 16);
        if (reg < g_slot.registers.size()) {
            g_slot.registers[reg] = (reg == HCR_CLOCKCONTROL && (val & 1)) ? (val | 2)
                                    : (reg == HCR_SOFTWARERESET && val) ? 0
                                                                        : val;
        }
        return 0;
    }
    case IOCTL_READHCR: {
        if (!Memory::Contains(in, 4)) return 0;
        const uint32_t reg = Memory::Read32(in);
        if (reg < g_slot.registers.size()) Write32(out, g_slot.registers[reg]);
        return 0;
    }
    case IOCTL_RESETCARD: Write32(out, g_slot.status); return 0;
    case IOCTL_SETCLK: return 0;
    case IOCTL_SENDCMD: {
        const int32_t ret = Execute(in, out, callback, callbackArg);
        if (ret == RET_EVENT_REGISTER) {
            if (held) *held = true;
            EventNotify();  // already true: answered now
            return 0;
        }
        return 0;
    }
    case IOCTL_GETSTATUS: {
        if (g_slot.size <= kSdscMaxSize) {
            g_slot.status |= CARD_INITIALIZED;
        } else {
            InitSdhc();  // IOS does the SDHC setup itself
            g_slot.status |= CARD_SDHC;
        }
        Write32(out, Inserted() ? (g_slot.status | CARD_INSERTED) : CARD_NOT_EXIST);
        return 0;
    }
    case IOCTL_GETOCR: Write32(out, Ocr()); return 0;
    default:
        std::fprintf(stderr, "[SDIO] ioctl 0x%02X not handled\n", cmd);
        return 0;
    }
}

// IOS_Ioctlv SENDCMD: in[0] the request, in[1] the data buffer, io[0] the response.
extern "C" int32_t Sdio_HLE_Ioctlv(uint32_t cmd, uint32_t numIn, uint32_t numOut,
                                   uint32_t vectorPtr) {
    if (cmd != IOCTLV_SENDCMD || numIn < 1 || numOut < 1 ||
        !Memory::Contains(vectorPtr, (numIn + numOut) * 8)) {
        std::fprintf(stderr, "[SDIO] ioctlv 0x%02X not handled\n", cmd);
        return 0;
    }
    const uint32_t request = Memory::Read32(vectorPtr);
    const uint32_t response = Memory::Read32(vectorPtr + numIn * 8);
    const uint32_t responseSize = Memory::Read32(vectorPtr + numIn * 8 + 4);
    if (response != 0 && responseSize != 0 && Memory::Contains(response, responseSize)) {
        std::memset(Memory::GetPointer(response, responseSize), 0, responseSize);
    }
    return Execute(request, response, 0, 0);
}
