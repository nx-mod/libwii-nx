#include "hle_stubs.h"

#include "console_identity.h"
#include "sc_serial_contract.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "nand_path.h"

#include <filesystem>
#include <system_error>

namespace {

// Use the SDK's own value tables, including its unknown-region result.
uint32_t LookupProductRegion(uint32_t table, uint32_t stride, uint32_t count,
                             const std::string& value) {
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t entry = table + index * stride;
        if (!Memory::Contains(entry, stride)) {
            break;
        }
        const auto* bytes = static_cast<const uint8_t*>(Memory::GetPointer(entry, stride));
        if (bytes[0] == 0xFF) {
            break;
        }
        if (value.size() < stride - 1 &&
            std::memcmp(bytes + 1, value.c_str(), value.size() + 1) == 0) {
            return bytes[0];
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace

// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC); we have no async IPC callbacks, so return 0 (SUCCESS) immediately.

// 0x801B0220 -> SCCheckStatus()
// Returns: 0 = success, 1 = busy, 2 = error
extern "C" uint32_t SCCheckStatus_HLE()
{
    // Return 0 (success) immediately to avoid infinite busy-wait in OSInit
    return 0;
}

PPC_NATIVE_OVERRIDE(801B0220, SCCheckStatus_HLE, uint32_t, (), ());

// 0x801B1BE4 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return RuntimeConfigFile::WidescreenEnabled(true) ? 1u : 0u;
}

PPC_NATIVE_OVERRIDE(801B1BE4, SCGetAspectRatio_HLE, uint32_t, (), ());

// 0x801B1CAC -> SCGetEuRgb60Mode()
// Returns: 0 = PAL50, 1 = PAL60/RGB60
extern "C" uint32_t SCGetEuRgb60Mode_HLE()
{
    // Default to PAL60 so PAL builds do not fall back to the half-rate PAL50
    // sync path on modern displays. Actual texture/cache correctness is handled
    // elsewhere; this only exposes the intended SYSCONF setting.
    return 1;
}

PPC_NATIVE_OVERRIDE(801B1CAC, SCGetEuRgb60Mode_HLE, uint32_t, (), ());

// Expose the selected emulated NAND identity through the SDK SC APIs.

extern "C" uint32_t SCGetProductArea_HLE()
{
    return LookupProductRegion(0x8029CEB0u, 5, 13,
                               RuntimeConsoleIdentity::Current().area);
}

PPC_NATIVE_OVERRIDE(801B23A0, SCGetProductArea_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductCode_HLE()
{
    // Original PAL SC storage for the six-byte CODE value.
    constexpr uint32_t kProductCodeAddress = 0x803869E0u;
    const std::string& productCode = RuntimeConsoleIdentity::Current().productCode;
    const size_t size = productCode.size() + 1;
    if (!Memory::Contains(kProductCodeAddress, size)) {
        return 0;
    }
    std::memcpy(Memory::GetPointer(kProductCodeAddress, size),
                productCode.c_str(), size);
    return kProductCodeAddress;
}

PPC_NATIVE_OVERRIDE(801B2424, SCGetProductCode_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductSN_HLE(uint32_t serialAddress)
{
    const std::string& serial = RuntimeConsoleIdentity::Current().serial;
    return RuntimeScSerial::Write(serial, serialAddress,
        [](uint32_t address, size_t size) { return Memory::Contains(address, size); },
        [](uint32_t address, uint32_t value) { Memory::Write32(address, value); });
}

PPC_NATIVE_OVERRIDE(801B2460, SCGetProductSN_HLE, uint32_t, (uint32_t serialAddress), (serialAddress));

extern "C" uint32_t SCGetProductGameRegion_HLE()
{
    return LookupProductRegion(0x8029CEF8u, 4, 4,
                               RuntimeConsoleIdentity::Current().gameRegion);
}

PPC_NATIVE_OVERRIDE(801B24C8, SCGetProductGameRegion_HLE, uint32_t, (), ());

// 0x801AE4A0 -> OS__IsTitleInstalled(titleIdHi, titleIdLo)
// Returns: 1 = installed, 0 = not installed
//
// This answered yes to everything, because a game that is told it is not itself
// installed gives up: Mario Kart checks for 0x00010004524d4350 ("RMCP") and
// reports error code 5. Saying yes to everything is worse for anything that
// asks about titles other than itself - the System Menu builds its channel list
// out of these answers, and would believe in every channel it thought to ask
// about.
//
// So answer from the NAND, which is the thing being asked about: a title is
// installed when its TMD is there. The one exception is a title asking about
// itself, which is installed by definition, because it is running.
extern "C" uint32_t OS__IsTitleInstalled(uint32_t titleIdHi, uint32_t titleIdLo)
{
    char relative[64];
    std::snprintf(relative, sizeof(relative), "title/%08x/%08x/content/title.tmd",
                  titleIdHi, titleIdLo);
    std::error_code ec;
    const std::filesystem::path tmd = RuntimeNandPath::ResolveNandRootPath() / relative;
    if (std::filesystem::exists(tmd, ec)) {
        RT_LOGF(RT_TAG_HLE, "CINS: OSIsTitleInstalled(0x%08X%08X) -> 1 (its TMD is in the NAND)\n",
                titleIdHi, titleIdLo);
        return 1;
    }

    if (Memory::Contains(0x80000000u, 4u) && Memory::Read32(0x80000000u) == titleIdLo) {
        RT_LOGF(RT_TAG_HLE,
                "CINS: OSIsTitleInstalled(0x%08X%08X) -> 1 (this is the running title; its TMD "
                "is not in the NAND)\n",
                titleIdHi, titleIdLo);
        return 1;
    }

    RT_LOGF(RT_TAG_HLE, "CINS: OSIsTitleInstalled(0x%08X%08X) -> 0 (no TMD in the NAND)\n",
            titleIdHi, titleIdLo);
    return 0;
}

PPC_NATIVE_OVERRIDE(801AE4A0, OS__IsTitleInstalled, uint32_t, (uint32_t titleIdHi, uint32_t titleIdLo), (titleIdHi, titleIdLo));

// 0x801AD1D4 -> OS__CheckInstall(requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr): returns 0 with
// outFlagsPtr = 0x3 (bit0 has data, bit1 has update; bit2 would be needs-blocks) i.e. fully installed.
extern "C" uint32_t OS__CheckInstall(uint32_t requiredBlocks, uint32_t titleIdHi, 
                                      uint32_t titleIdLo, uint32_t outFlagsPtr)
{
    // bit0: the title's data is here. bit1: its update is here. bit2: room is
    // still needed. This used to answer 0x3 for anything - data and update
    // present, nothing needed - which is a promise about a NAND nobody looked
    // at. The flags are answerable now: whether its own directory holds
    // anything, and whether the blocks asked for would fit.
    uint32_t flags = 0;
    std::error_code ec;
    char relative[48];
    std::snprintf(relative, sizeof(relative), "title/%08x/%08x", titleIdHi, titleIdLo);
    const std::filesystem::path titleDir = RuntimeNandPath::ResolveNandRootPath() / relative;
    if (std::filesystem::exists(titleDir / "content" / "title.tmd", ec)) {
        flags |= 0x1;
    }
    if (std::filesystem::exists(titleDir / "data", ec) &&
        !std::filesystem::is_empty(titleDir / "data", ec)) {
        flags |= 0x2;
    }

    // A block is eight clusters of sixteen kilobytes, and the NAND holds
    // 0x7ec0 of them less 0x300 reserved.
    constexpr uint64_t kClusterBytes = 16384;
    constexpr uint64_t kClustersPerBlock = 8;
    constexpr uint64_t kUsableClusters = 0x7ec0 - 0x300;
    uint64_t usedClusters = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             RuntimeNandPath::ResolveNandRootPath(), ec)) {
        if (ec) break;
        std::error_code entryEc;
        if (entry.is_regular_file(entryEc)) {
            const uint64_t size = static_cast<uint64_t>(entry.file_size(entryEc));
            if (!entryEc) usedClusters += (size + kClusterBytes - 1) / kClusterBytes;
        }
    }
    const uint64_t freeBlocks = usedClusters >= kUsableClusters
                                    ? 0
                                    : (kUsableClusters - usedClusters) / kClustersPerBlock;
    if (requiredBlocks > freeBlocks) {
        flags |= 0x4;  // room is needed
    }

    RT_LOGF(RT_TAG_HLE,
            "OS__CheckInstall(blocks=%u, 0x%08X%08X) -> flags 0x%X (%llu block(s) free)\n",
            requiredBlocks, titleIdHi, titleIdLo, flags,
            static_cast<unsigned long long>(freeBlocks));
    if (outFlagsPtr != 0) {
        Memory::Write32(outFlagsPtr, flags);
    }
    return 0; // the query itself succeeded; the flags say what it found
}

PPC_NATIVE_OVERRIDE(801AD1D4, OS__CheckInstall, uint32_t, (uint32_t requiredBlocks, uint32_t titleIdHi, uint32_t titleIdLo, uint32_t outFlagsPtr), (requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr));
