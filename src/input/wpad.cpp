#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "wii_remote_input.h"

#include <cstdint>

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace {

constexpr uint32_t kDefaultWorkMemSize = 0x20000;
constexpr uint8_t kDefaultDpdSensitivity = 3;
constexpr int32_t kStatusOk = 0;

struct WpadStubState {
    bool initSubRan = false;
    bool simpleSyncActive = false;
    uint32_t syncDeviceCallback = 0;
    uint32_t workMemSize = kDefaultWorkMemSize;
    uint8_t dpdSensitivity = kDefaultDpdSensitivity;
    WpadContract::State contract{};
};

WpadStubState g_state{};

void InvokeWpadCallback(uint32_t callback, uint32_t chan, int32_t result)
{
    if (callback == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = chan;
    cpu.gpr[4] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callback, &cpu);
}

int32_t InitializeWpadLibrary()
{
    g_state.contract.Initialize();
    return kStatusOk;
}

// The async WPAD entry points all report their outcome to the guest callback and
// then return the same value.
int32_t CompleteWpadRequest(uint32_t chan, uint32_t callback, int32_t result)
{
    InvokeWpadCallback(callback, chan, result);
    return result;
}

} // namespace

extern "C" int32_t WPADGetStatus_HLE()
{
    // RVL::WPADGetStatus takes no channel arg; it reports global WUD library state.
    // Reading r3 here would leak a caller's stale register value into the result.
    return g_state.contract.GetLibraryStatus();
}
PPC_NATIVE_OVERRIDE(801BF64C, WPADGetStatus_HLE, int32_t, (), ());

extern "C" uint32_t WPADGetDpdSensitivity_HLE()
{
    return static_cast<uint32_t>(g_state.dpdSensitivity);
}
PPC_NATIVE_OVERRIDE(801C329C, WPADGetDpdSensitivity_HLE, uint32_t, (), ());

extern "C" int32_t WPADInitSub_HLE()
{
    if (!g_state.initSubRan) {
        g_state.initSubRan = true;
        return InitializeWpadLibrary();
    }
    return kStatusOk;
}
PPC_NATIVE_OVERRIDE(801BF3B4, WPADInitSub_HLE, int32_t, (), ());

extern "C" int32_t WPADInit_HLE()
{
    return InitializeWpadLibrary();
}
PPC_NATIVE_OVERRIDE(801BF5C4, WPADInit_HLE, int32_t, (), ());

extern "C" int32_t WUDGetStatus_HLE()
{
    return WpadContract::kStatusReady;
}
PPC_NATIVE_OVERRIDE(801CDB84, WUDGetStatus_HLE, int32_t, (), ());

// WPADGetDataFormat reads the per-channel format set by WPADSetDataFormat. Can't reuse the
// translated SDK implementation: it dereferences Bluetooth control blocks that HLE'd WPADInit
// never constructs.
extern "C" int32_t WPADGetDataFormat_HLE(uint32_t chan)
{
    return g_state.contract.GetDataFormat(chan);
}
PPC_NATIVE_OVERRIDE(801C0B54, WPADGetDataFormat_HLE, int32_t, (uint32_t chan), (chan));

// WPADSetDataFormat: records the per-channel data format the game asked for.
extern "C" int32_t WPADSetDataFormat_HLE(uint32_t chan, int32_t format)
{
    return g_state.contract.SetDataFormat(chan, format);
}
PPC_NATIVE_OVERRIDE(801C0B9C, WPADSetDataFormat_HLE, int32_t, (uint32_t chan, int32_t format), (chan, format));

// WPADProbe: reports the extension type of a Bluetooth remote on `chan`, or no controller.
extern "C" int32_t WPADProbe_HLE(uint32_t chan, uint32_t typePtr)
{
    if (chan >= WpadContract::kChannelCount) {
        return WpadContract::kErrorBadChannel;
    }

    // Drive the rescan state machine here too: a reconnect probe can arrive
    // before the next PADRead, and only Poll() brings a dropped remote back.
    WiiRemoteInput::Poll();

    // A real Bluetooth remote: WPAD_DEV_CORE (0) for a bare remote,
    // WPAD_DEV_FREESTYLE (1) with a Nunchuk, WPAD_DEV_CLASSIC (2) with a Classic
    // Controller. The game reads the type from here (not from
    // KPADStatus.dev_type) to pick its control scheme, and re-reads it when it
    // changes, which is what makes an extension swap mid-game work like on the
    // console. EffectiveKind keeps the last type through SDL's re-creation of
    // the joystick after a swap.
    const WiiRemoteInput::Kind kind = WiiRemoteInput::EffectiveKind(chan);
    if (WiiRemoteInput::IsRemoteChannel(chan)) {
        if (typePtr != 0) {
            uint32_t type = WpadContract::kExtensionCore;
            if (kind == WiiRemoteInput::Kind::RemoteWithNunchuk) type = 1u;
            if (kind == WiiRemoteInput::Kind::RemoteWithClassic) type = 2u;
            Memory::Write32(typePtr, type);
        }
        return kStatusOk;
    }
    if (typePtr != 0) {
        Memory::Write32(typePtr, WpadContract::kExtensionCore);
    }
    return WpadContract::kErrorNoController;
}
PPC_NATIVE_OVERRIDE(801C0990, WPADProbe_HLE, int32_t, (uint32_t chan, uint32_t typePtr), (chan, typePtr));

extern "C" void WPADControlMotor_HLE(uint32_t chan, uint32_t command)
{
    (void)chan;
    (void)command;
}
PPC_NATIVE_OVERRIDE(801C0EC4, WPADControlMotor_HLE, void, (uint32_t chan, uint32_t command), (chan, command));

extern "C" int32_t WPADGetInfoAsync_HLE(uint32_t chan, uint32_t infoPtr, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }

    (void)infoPtr;
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
PPC_NATIVE_OVERRIDE(801C0CA4, WPADGetInfoAsync_HLE, int32_t,
         (uint32_t chan, uint32_t infoPtr, uint32_t callback), (chan, infoPtr, callback));

extern "C" int32_t WPADControlLed_HLE(uint32_t chan, uint32_t ledMask, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }
    (void)ledMask;
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
PPC_NATIVE_OVERRIDE(801C0FF8, WPADControlLed_HLE, int32_t,
         (uint32_t chan, uint32_t ledMask, uint32_t callback), (chan, ledMask, callback));

extern "C" int32_t WPADStartSimpleSync_HLE()
{
    if (g_state.simpleSyncActive) {
        return 0;
    }
    g_state.simpleSyncActive = true;
    return 1;
}
PPC_NATIVE_OVERRIDE(801BF634, WPADStartSimpleSync_HLE, int32_t, (), ()); // WUDStartSyncSimple
PPC_NATIVE_OVERRIDE(801BF638, WPADStartSimpleSync_HLE, int32_t, (), ()); // WPADStartSimpleSync (HBM)

// due to multiplayer controller screen hle this to avoid startsyncdevice to return fail every frame
// causing you to get softlocked in the game
extern "C" int32_t WPADStopSimpleSync_HLE()
{
    if (g_state.simpleSyncActive) {
        g_state.simpleSyncActive = false;
        const uint32_t callback = g_state.syncDeviceCallback;
        if (callback != 0 && TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
            NandQueueIosCallback(callback, 1, 0); // callback(WUD_SYNC_DONE, devicesSynced=0)
        }
    }
    return 1;
}
PPC_NATIVE_OVERRIDE(801BF63C, WPADStopSimpleSync_HLE, int32_t, (), ());

extern "C" uint32_t WPADSetSyncDeviceCallback_HLE(uint32_t callback)
{
    const uint32_t previous = g_state.syncDeviceCallback;
    g_state.syncDeviceCallback = callback;
    return previous;
}
PPC_NATIVE_OVERRIDE(801BF640, WPADSetSyncDeviceCallback_HLE, uint32_t, (uint32_t callback), (callback));

// ----------------------------------------------------------------------------
// The callbacks WPAD reports a remote through. A game registers them and then
// waits: the Wii Menu decides a remote is there only when its connect callback
// says so, and reads it no sooner. Nothing on the Switch raises the Bluetooth
// events they hang off, so they are delivered from the vblank (VI_HLE's retrace
// hook, the guest context the hardware's own interrupt would use): connect when
// a channel's controller appears or goes, extension when what is plugged into it
// changes. (Sampling: see DeliverWpadCallbacks.)
// ----------------------------------------------------------------------------
namespace {

constexpr uint32_t kChannels = 4;
constexpr int32_t kWpadErrNoController = -1;
// WPAD_DEV_*: the Switch's controllers arrive as a remote with a Classic
// Controller (wii_remote_input.cpp), a Nunchuk when that is what is held.
constexpr int32_t kDevCore = 0, kDevFreestyle = 1, kDevClassic = 2;

struct WpadCallbacks {
    uint32_t sampling = 0, connect = 0, extension = 0;
    bool announced = false;   // the connect callback has been told "there"
    int32_t device = -1;      // what the extension callback was last told
};
WpadCallbacks g_callbacks[kChannels];

uint32_t Exchange(uint32_t& slot, uint32_t callback) {
    const uint32_t previous = slot;
    slot = callback;
    return previous;
}

void CallGuest(CpuContext* cpu, uint32_t callback, uint32_t chan, int32_t arg) {
    if (callback == 0 || !TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
        return;
    }
    const uint32_t r3 = cpu->gpr[3], r4 = cpu->gpr[4];
    cpu->gpr[3] = chan;
    cpu->gpr[4] = static_cast<uint32_t>(arg);
    InvokeIndirectCpu(callback, cpu);
    cpu->gpr[3] = r3;
    cpu->gpr[4] = r4;
}

void DeliverWpadCallbacks(CpuContext* cpu) {
    for (uint32_t chan = 0; chan < kChannels; ++chan) {
        WpadCallbacks& cb = g_callbacks[chan];
        WiiRemoteInput::KpadSample sample;
        const bool present = WiiRemoteInput::ReadKpadSample(chan, sample);
        if (cb.connect != 0 && present != cb.announced) {
            cb.announced = present;
            CallGuest(cpu, cb.connect, chan, present ? kStatusOk : kWpadErrNoController);
        }
        if (!present) {
            cb.device = -1;
            continue;
        }
        const int32_t device = sample.hasClassic ? kDevClassic : sample.hasNunchuk ? kDevFreestyle : kDevCore;
        if (cb.extension != 0 && device != cb.device) {
            cb.device = device;
            CallGuest(cpu, cb.extension, chan, device);
        }
        // (The sampling callback is kept but not called: the one games register
        // is KPAD's own, which reads WPAD's Bluetooth state - and KPADRead is
        // native here, so nothing waits on it.)
    }
}

const bool g_wpadRetraceHook = [] {
    VI_HLE_AddRetraceHook(DeliverWpadCallbacks);
    return true;
}();

} // namespace

// WPADSetSamplingCallback / WPADSetConnectCallback / WPADSetExtensionCallback:
// (chan, callback) -> the callback replaced, as the SDK's.
extern "C" uint32_t WPADSetSamplingCallback_HLE(uint32_t chan, uint32_t callback)
{
    return chan < kChannels ? Exchange(g_callbacks[chan].sampling, callback) : 0;
}
PPC_NATIVE_OVERRIDE(801C0A1C, WPADSetSamplingCallback_HLE, uint32_t, (uint32_t chan, uint32_t callback), (chan, callback));

extern "C" uint32_t WPADSetConnectCallback_HLE(uint32_t chan, uint32_t callback)
{
    if (chan >= kChannels) {
        return 0;
    }
    // A new callback has not been told anything yet: the next vblank says so.
    g_callbacks[chan].announced = false;
    return Exchange(g_callbacks[chan].connect, callback);
}
PPC_NATIVE_OVERRIDE(801C0A84, WPADSetConnectCallback_HLE, uint32_t, (uint32_t chan, uint32_t callback), (chan, callback));

extern "C" uint32_t WPADSetExtensionCallback_HLE(uint32_t chan, uint32_t callback)
{
    if (chan >= kChannels) {
        return 0;
    }
    g_callbacks[chan].device = -1;
    return Exchange(g_callbacks[chan].extension, callback);
}
PPC_NATIVE_OVERRIDE(801C0AEC, WPADSetExtensionCallback_HLE, uint32_t, (uint32_t chan, uint32_t callback), (chan, callback));
