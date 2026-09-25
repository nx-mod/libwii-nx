// The Bluetooth stack the Wii Remote talks over: GKI is its kernel shim and BTM
// its device manager, both from the Broadcom stack the SDK carries.
//
// They belong beside wpad.cpp and kpad.cpp, which is what they exist to serve,
// rather than with the OS natives they were first written next to.

#include "hle_stubs.h"
#include "abi_bridge.h"
#include "memory.h"
#include "runtime_log.h"

#include <chrono>
#include <cstdint>
#include <thread>

extern "C" void GKI_delay_HLE_801301b4(CpuContext* ctx)
{
    const uint32_t delayMs = ctx ? static_cast<uint32_t>(ctx->gpr[3]) : 0;
    const uint32_t sleepMs = delayMs == 0 ? 1u : std::min(delayMs, 10u);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
}

extern "C" uint32_t BTM_IsDeviceUp_HLE_8013a300(CpuContext* ctx)
{
    // Force Bluetooth stack to "up" to avoid endless polling loops while we lack
    // real hardware bring-up.
    constexpr uint32_t kBtmCbBase = 0x80336278u;
    constexpr uint32_t kDevStateOffset = 0x64Eu;
    try {
        ::Memory::Write8(kBtmCbBase + kDevStateOffset, 5u);
    } catch (const ::Memory::AccessViolation&) {
        // Ignore; best-effort write
    }

    if (ctx) {
        ctx->gpr[3] = 1;
    }
    return 1;
}

PPC_NATIVE_OVERRIDE_VOID(801301B4, GKI_delay_HLE_801301b4, (CpuContext* ctx), (ctx));
PPC_NATIVE_OVERRIDE(8013A300, BTM_IsDeviceUp_HLE_8013a300, uint32_t, (CpuContext* ctx), (ctx));
