// NAND/ISFS HLE: the IOS_* device layer.
//
// Shared state and helpers live in nand_internal.h.

#include "nand_internal.h"

#include "discord_presence.h"
#include "runtime_log.h"

extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx);

// ============================================================================
// SHA device handles
// ============================================================================

struct ShaHandle {
    CryptoPP::SHA1 hash;
    uint64_t byteCount = 0;

    void Restart() {
        hash.Restart();
        byteCount = 0;
    }
};

static std::map<int32_t, ShaHandle> g_shaHandles;
static int32_t g_nextShaFd = 0x10001;
static std::mutex g_shaMutex;

static int32_t AllocateShaFd() {
    std::lock_guard<std::mutex> lock(g_shaMutex);
    const int32_t fd = g_nextShaFd++;
    g_shaHandles.try_emplace(fd);
    return fd;
}

static ShaHandle* GetShaHandle(int32_t fd) {
    auto it = g_shaHandles.find(fd);
    if (it == g_shaHandles.end()) {
        return nullptr;
    }
    return &it->second;
}

static void CloseShaFd(int32_t fd) {
    std::lock_guard<std::mutex> lock(g_shaMutex);
    g_shaHandles.erase(fd);
}

struct ISFSFileStats {
    uint32_t length;    // File size in bytes
    uint32_t position;  // Current file position
};

// ============================================================================
// Device identifiers and ioctl commands
// ============================================================================

// Special FD for /dev/fs (the ISFS device)
static constexpr int32_t ISFS_DEV_FD = 1;
static constexpr int32_t ES_DEV_FD = 3;
static constexpr int32_t DOLPHIN_DEV_FD = 4;
static constexpr uint32_t ES_IOCTL_GETDEVICEID = 0x07;
static constexpr uint32_t ES_IOCTL_GETTITLECNT = 0x0E;
static constexpr uint32_t ES_IOCTL_GETTITLES = 0x0F;
static constexpr uint32_t ES_IOCTL_GETTITLECONTENTSCNT = 0x10;
static constexpr uint32_t ES_IOCTL_GETTITLECONTENTS = 0x11;
static constexpr uint32_t ES_IOCTL_GETTITLEDIR = 0x1D;
static constexpr uint32_t ES_IOCTL_GETDEVICECERT = 0x1E;
static constexpr uint32_t ES_IOCTL_GETTITLEID = 0x20;
static constexpr uint32_t ES_IOCTL_SIGN = 0x30;
// A title's own contents, read the way the SDK's CNT library reads them.
static constexpr uint32_t ES_IOCTL_OPENCONTENT = 0x09;       // index of the running title
static constexpr uint32_t ES_IOCTL_READCONTENT = 0x0A;
static constexpr uint32_t ES_IOCTL_CLOSECONTENT = 0x0B;
static constexpr uint32_t ES_IOCTL_SEEKCONTENT = 0x23;
static constexpr uint32_t ES_IOCTL_OPENTITLECONTENT = 0x24;  // another title's, by id
// What the Wii Menu reads for every title it lists.
static constexpr uint32_t ES_IOCTL_GETOWNEDTITLECNT = 0x0C;
static constexpr uint32_t ES_IOCTL_GETOWNEDTITLES = 0x0D;
static constexpr uint32_t ES_IOCTL_GETVIEWCNT = 0x12;        // ticket views
static constexpr uint32_t ES_IOCTL_GETVIEWS = 0x13;
static constexpr uint32_t ES_IOCTL_GETTMDVIEWCNT = 0x14;     // the TMD view's size
static constexpr uint32_t ES_IOCTL_GETTMDVIEWS = 0x15;
static constexpr uint32_t ES_IOCTL_GETSTOREDCONTENTCNT = 0x32;
static constexpr uint32_t ES_IOCTL_GETSTOREDCONTENTS = 0x33;
static constexpr uint32_t ES_IOCTL_GETSTOREDTMDSIZE = 0x34;
static constexpr uint32_t ES_IOCTL_GETSTOREDTMD = 0x35;
static constexpr int32_t ES_ENOENT = -106;
static constexpr int32_t ES_EINVAL = -1017;
static constexpr uint32_t DOLPHIN_IOCTL_GET_ELAPSED_TIME = 0x01;
static constexpr uint32_t DOLPHIN_IOCTL_GET_VERSION = 0x02;
static constexpr uint32_t DOLPHIN_IOCTL_GET_SPEED_LIMIT = 0x03;
static constexpr uint32_t DOLPHIN_IOCTL_SET_SPEED_LIMIT = 0x04;
static constexpr uint32_t DOLPHIN_IOCTL_GET_CPU_SPEED = 0x05;
static constexpr uint32_t DOLPHIN_IOCTL_GET_REAL_PRODUCT_CODE = 0x06;
static constexpr uint32_t DOLPHIN_IOCTL_DISCORD_SET_CLIENT = 0x07;
static constexpr uint32_t DOLPHIN_IOCTL_DISCORD_SET_PRESENCE = 0x08;
static constexpr uint32_t DOLPHIN_IOCTL_DISCORD_RESET = 0x09;
static constexpr uint32_t DOLPHIN_IOCTL_GET_SYSTEM_TIME = 0x0A;
static constexpr uint32_t SHA_IOCTL_INIT = 0;
static constexpr uint32_t SHA_IOCTL_UPDATE = 1;
static constexpr uint32_t SHA_IOCTL_FINAL = 2;

static std::string ReadGuestCString(uint32_t address, size_t maxLength = 1024) {
    std::string text;
    if (address == 0) {
        return text;
    }

    for (size_t i = 0; i < maxLength; ++i) {
        const uint32_t current = address + static_cast<uint32_t>(i);
        if (!Memory::Contains(current, 1)) {
            break;
        }
        const char ch = static_cast<char>(Memory::Read8(current));
        if (ch == '\0') {
            break;
        }
        text.push_back(ch);
    }
    return text;
}
static constexpr uint32_t SHA_CONTEXT_SIZE = 0x1c;
static constexpr uint32_t SHA_DIGEST_SIZE = 0x14;

// Same guest layout as the /dev/net ioctlv descriptors; see runtime_parse_helpers.h.
using IosVector = RuntimeHle::IoVector;
using RuntimeHle::ReadIoVector;

static IosVector ReadIosVector(uint32_t vectorPtr, uint32_t index) {
    return ReadIoVector(vectorPtr, index);
}

// Every title the NAND holds, as a console would enumerate them: a directory
// under /title/<high>/<low> counts when it has a TMD, because that is what
// makes a title installed rather than merely present. Sorted, so two calls in a
// row agree - a caller asks for the count and then for the list, and would be
// entitled to a different answer otherwise.
// Which of a title's contents are actually here. The TMD lists what the title
// is made of; a content counts as stored when its file can be opened, which for
// a shared one means through /shared1 - TranslateNandPath already resolves that,
// so the same question answers both kinds.
static std::vector<uint32_t> StoredContents(uint64_t titleId) {
    std::vector<uint32_t> contents;
    char tmdPath[64];
    std::snprintf(tmdPath, sizeof(tmdPath), "/title/%08x/%08x/content/title.tmd",
                  static_cast<uint32_t>(titleId >> 32), static_cast<uint32_t>(titleId));
    std::ifstream tmdFile(TranslateNandPath(tmdPath), std::ios::binary);
    if (!tmdFile) {
        return contents;
    }
    const std::vector<uint8_t> tmd((std::istreambuf_iterator<char>(tmdFile)),
                                   std::istreambuf_iterator<char>());
    if (tmd.size() < 0x1E4) {
        return contents;
    }
    const auto be16 = [&tmd](size_t at) {
        return static_cast<uint16_t>((tmd[at] << 8) | tmd[at + 1]);
    };
    const auto be32 = [&tmd](size_t at) {
        return (static_cast<uint32_t>(tmd[at]) << 24) | (static_cast<uint32_t>(tmd[at + 1]) << 16) |
               (static_cast<uint32_t>(tmd[at + 2]) << 8) | static_cast<uint32_t>(tmd[at + 3]);
    };
    const size_t count = be16(0x1DE);
    if (tmd.size() < 0x1E4 + count * 36) {
        return contents;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t contentId = be32(0x1E4 + i * 36);
        char appPath[64];
        std::snprintf(appPath, sizeof(appPath), "/title/%08x/%08x/content/%08x.app",
                      static_cast<uint32_t>(titleId >> 32), static_cast<uint32_t>(titleId),
                      contentId);
        std::error_code ec;
        if (std::filesystem::exists(TranslateNandPath(appPath), ec)) {
            contents.push_back(contentId);
        }
    }
    return contents;
}

static std::vector<uint64_t> InstalledTitles() {
    std::vector<uint64_t> titles;
    std::error_code ec;
    const std::filesystem::path titleRoot = TranslateNandPath("/title");
    for (const auto& high : std::filesystem::directory_iterator(titleRoot, ec)) {
        if (ec) break;
        std::error_code highEc;
        if (!high.is_directory(highEc)) continue;
        const std::string highName = HostPathText(high.path().filename());
        if (highName.size() != 8) continue;
        for (const auto& low : std::filesystem::directory_iterator(high.path(), highEc)) {
            if (highEc) break;
            std::error_code lowEc;
            if (!low.is_directory(lowEc)) continue;
            const std::string lowName = HostPathText(low.path().filename());
            if (lowName.size() != 8) continue;
            if (!std::filesystem::exists(low.path() / "content" / "title.tmd", lowEc)) {
                continue;
            }
            try {
                const uint64_t id = (std::stoull(highName, nullptr, 16) << 32) |
                                    std::stoull(lowName, nullptr, 16);
                titles.push_back(id);
            } catch (const std::exception&) {
                // not a title id; a stray directory
            }
        }
    }
    std::sort(titles.begin(), titles.end());
    return titles;
}

static uint64_t CurrentTitleId() {
    return (static_cast<uint64_t>(CurrentTitleIdHi()) << 32) | CurrentTitleIdLo();
}

// ============================================================================
// Title metadata views (ES GetTMDViews / GetViews / GetStoredTMD)
// ============================================================================

static std::vector<uint8_t> ReadNandFile(const char* wiiPath) {
    std::ifstream file(TranslateNandPath(wiiPath), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

static std::vector<uint8_t> StoredTmd(uint64_t titleId) {
    char path[64];
    std::snprintf(path, sizeof(path), "/title/%08x/%08x/content/title.tmd",
                  static_cast<uint32_t>(titleId >> 32), static_cast<uint32_t>(titleId));
    std::vector<uint8_t> tmd = ReadNandFile(path);
    if (tmd.size() < 0x1E4) {
        return {};
    }
    const size_t count = (static_cast<size_t>(tmd[0x1DE]) << 8) | tmd[0x1DF];
    return tmd.size() >= 0x1E4 + count * 36 ? tmd : std::vector<uint8_t>{};
}

// A TMD as ES shows it to a title: the header from its version up to the
// access rights, the title version and content count, then per content its id,
// index, type and size - no hashes, no signature. 0x5C + 16 per content.
static std::vector<uint8_t> TmdView(const std::vector<uint8_t>& tmd) {
    std::vector<uint8_t> view(tmd.begin() + 0x180, tmd.begin() + 0x1D8);
    view.insert(view.end(), tmd.begin() + 0x1DC, tmd.begin() + 0x1E0);
    const size_t count = (static_cast<size_t>(tmd[0x1DE]) << 8) | tmd[0x1DF];
    for (size_t i = 0; i < count; ++i) {
        const auto record = tmd.begin() + 0x1E4 + i * 36;
        view.insert(view.end(), record, record + 16);
    }
    return view;
}

static std::vector<uint8_t> StoredTickets(uint64_t titleId) {
    char path[64];
    std::snprintf(path, sizeof(path), "/ticket/%08x/%08x.tik",
                  static_cast<uint32_t>(titleId >> 32), static_cast<uint32_t>(titleId));
    return ReadNandFile(path);
}

static constexpr size_t kTicketSize = 0x2A4;
static constexpr size_t kTicketViewSize = 0xD8;

// A ticket as ES shows it: its version, then everything from the ticket id to
// the end - never the title key.
static std::vector<uint8_t> TicketView(const uint8_t* ticket) {
    std::vector<uint8_t> view{0, 0, 0, ticket[0x1BC]};
    view.insert(view.end(), ticket + 0x1D0, ticket + kTicketSize);
    return view;
}

// Titles with a ticket, which is what "owned" means to ES.
static std::vector<uint64_t> OwnedTitles() {
    std::vector<uint64_t> owned;
    std::error_code ec;
    const std::filesystem::path root = TranslateNandPath("/ticket");
    for (const auto& high : std::filesystem::directory_iterator(root, ec)) {
        std::error_code inner;
        for (const auto& ticket : std::filesystem::directory_iterator(high.path(), inner)) {
            const std::string name = ticket.path().filename().string();
            if (name.size() != 12 || name.substr(8) != ".tik") {
                continue;
            }
            owned.push_back((std::strtoull(high.path().filename().string().c_str(), nullptr, 16) << 32) |
                            std::strtoul(name.substr(0, 8).c_str(), nullptr, 16));
        }
    }
    std::sort(owned.begin(), owned.end());
    return owned;
}

static uint64_t ReadTitleIdVector(uint32_t address) {
    return (static_cast<uint64_t>(Memory::Read32(address)) << 32) | Memory::Read32(address + 4);
}

// ============================================================================
// Title contents (ES OpenContent / ReadContent / SeekContent / CloseContent)
// ============================================================================

// Content fds are ES's own small numbers, as on a console; sixteen at once.
static constexpr int kMaxContentFds = 16;
static std::FILE* g_contentFiles[kMaxContentFds] = {};

// The host file of content `index` (its TMD index, not its id) of a title:
// the title's own folder, or /shared1 through the map when it is shared -
// TranslateNandPath does that part.
static std::filesystem::path ContentHostPath(uint64_t titleId, uint32_t index) {
    char tmdPath[64];
    std::snprintf(tmdPath, sizeof(tmdPath), "/title/%08x/%08x/content/title.tmd",
                  static_cast<uint32_t>(titleId >> 32), static_cast<uint32_t>(titleId));
    std::ifstream tmdFile(TranslateNandPath(tmdPath), std::ios::binary);
    const std::vector<uint8_t> tmd((std::istreambuf_iterator<char>(tmdFile)),
                                   std::istreambuf_iterator<char>());
    if (tmd.size() < 0x1E4) {
        return {};
    }
    const size_t count = (static_cast<size_t>(tmd[0x1DE]) << 8) | tmd[0x1DF];
    for (size_t i = 0; i < count && 0x1E4 + (i + 1) * 36 <= tmd.size(); ++i) {
        const uint8_t* record = tmd.data() + 0x1E4 + i * 36;
        if (((static_cast<uint32_t>(record[4]) << 8) | record[5]) != index) {
            continue;
        }
        const uint32_t id = (static_cast<uint32_t>(record[0]) << 24) | (record[1] << 16) |
                            (record[2] << 8) | record[3];
        char appPath[64];
        std::snprintf(appPath, sizeof(appPath), "/title/%08x/%08x/content/%08x.app",
                      static_cast<uint32_t>(titleId >> 32), static_cast<uint32_t>(titleId), id);
        return TranslateNandPath(appPath);
    }
    return {};
}

static int32_t OpenTitleContent(uint64_t titleId, uint32_t index) {
    const std::filesystem::path path = ContentHostPath(titleId, index);
    std::FILE* file = path.empty() ? nullptr : std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        LogNandWarning("ES_OpenContent", "title %016llx content index %u is not in the NAND",
                       static_cast<unsigned long long>(titleId), index);
        return ES_ENOENT;
    }
    for (int cfd = 0; cfd < kMaxContentFds; ++cfd) {
        if (g_contentFiles[cfd] == nullptr) {
            g_contentFiles[cfd] = file;
            return cfd;
        }
    }
    std::fclose(file);
    LogNandWarning("ES_OpenContent", "all %d content fds are open", kMaxContentFds);
    return ES_EINVAL;
}

static std::FILE* ContentFile(uint32_t cfd) {
    return cfd < static_cast<uint32_t>(kMaxContentFds) ? g_contentFiles[cfd] : nullptr;
}

static bool WriteGuestBytes(uint32_t address, uint32_t size, const uint8_t* data, size_t dataSize) {
    if (address == 0 || size < dataSize || !Memory::Contains(address, dataSize)) {
        return false;
    }
    uint8_t* out = Memory::GetPointer(address, dataSize);
    std::memcpy(out, data, dataSize);
    return true;
}

static bool IsValidGuestRange(uint32_t address, uint32_t size) {
    return size == 0 || (address != 0 && Memory::Contains(address, size));
}

using DolphinClock = std::chrono::steady_clock;
// Dolphin starts this clock when the emulation device is constructed, not on its first ioctl.
static const DolphinClock::time_point g_dolphinElapsedStart = DolphinClock::now();

static uint32_t DolphinElapsedMilliseconds() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(DolphinClock::now() - g_dolphinElapsedStart).count());
}

static int32_t HandleDolphinIoctlv(uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    if (vectorPtr != 0 && !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
        return ISFS_EINVAL;
    }

    // Every /dev/dolphin command answers through exactly one output vector.
    // `minimumSize` 0 means the case validates the buffer itself (WriteGuestBytes).
    const auto singleOut = [&](uint32_t minimumSize, IosVector& out) -> bool {
        if (numOut != 1 || vectorPtr == 0) {
            return false;
        }
        out = ReadIosVector(vectorPtr, numIn);
        return minimumSize == 0 ||
               (out.size >= minimumSize && Memory::Contains(out.address, minimumSize));
    };

    IosVector out;
    switch (cmd) {
        case DOLPHIN_IOCTL_GET_ELAPSED_TIME: {
            if (!singleOut(4u, out)) {
                return ISFS_EINVAL;
            }
            Memory::Write32(out.address, DolphinElapsedMilliseconds());
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_GET_VERSION: {
            if (!singleOut(0u, out)) {
                return ISFS_EINVAL;
            }
            static constexpr char kVersion[] = "WiiCompiled-DolphinDevice";
            if (!WriteGuestBytes(out.address, out.size,
                                 reinterpret_cast<const uint8_t*>(kVersion), sizeof(kVersion))) {
                return ISFS_EINVAL;
            }
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_GET_SPEED_LIMIT:
        case DOLPHIN_IOCTL_GET_CPU_SPEED: {
            if (!singleOut(4u, out)) {
                return ISFS_EINVAL;
            }
            Memory::Write32(out.address, cmd == DOLPHIN_IOCTL_GET_SPEED_LIMIT ? 100u : 729000000u);
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_GET_REAL_PRODUCT_CODE: {
            if (!singleOut(0u, out)) {
                return ISFS_EINVAL;
            }
            char productCode[8] = {};
            uint32_t discId = Memory::Contains(0x80000000u, 4u) ? Memory::Read32(0x80000000u) : kNandTitleIdLo;
            productCode[0] = static_cast<char>((discId >> 24) & 0xffu);
            productCode[1] = static_cast<char>((discId >> 16) & 0xffu);
            productCode[2] = static_cast<char>((discId >> 8) & 0xffu);
            productCode[3] = static_cast<char>(discId & 0xffu);
            productCode[4] = '0';
            productCode[5] = '1';
            if (!WriteGuestBytes(out.address, out.size,
                                 reinterpret_cast<const uint8_t*>(productCode), sizeof(productCode))) {
                return ISFS_EINVAL;
            }
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_SET_SPEED_LIMIT:
            return ISFS_OK;

        case DOLPHIN_IOCTL_DISCORD_SET_CLIENT: {
            if (numIn != 1 || numOut != 0 || vectorPtr == 0) {
                return ISFS_EINVAL;
            }
            const IosVector client = ReadIosVector(vectorPtr, 0);
            if (!IsValidGuestRange(client.address, client.size)) {
                return ISFS_EINVAL;
            }
            if (RuntimeConfigFile::DiscordPresenceEnabled()) {
                DiscordPresence::SetClient(ReadGuestCString(client.address, client.size));
            }
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_DISCORD_SET_PRESENCE: {
            if (numIn != 10 || numOut != 0 || vectorPtr == 0) {
                return ISFS_EINVAL;
            }
            std::array<IosVector, 10> values{};
            for (uint32_t index = 0; index < values.size(); ++index) {
                values[index] = ReadIosVector(vectorPtr, index);
                if (!IsValidGuestRange(values[index].address, values[index].size)) {
                    return ISFS_EINVAL;
                }
            }
            if (RuntimeConfigFile::DiscordPresenceEnabled()) {
                DiscordPresence::Activity activity;
                activity.details = ReadGuestCString(values[0].address, values[0].size);
                activity.state = ReadGuestCString(values[1].address, values[1].size);
                activity.largeImageKey = ReadGuestCString(values[2].address, values[2].size);
                activity.largeImageText = ReadGuestCString(values[3].address, values[3].size);
                activity.smallImageKey = ReadGuestCString(values[4].address, values[4].size);
                activity.smallImageText = ReadGuestCString(values[5].address, values[5].size);
                if (values[6].size >= 8 && Memory::Contains(values[6].address, 8)) {
                    activity.startTimestamp = static_cast<int64_t>(
                        (static_cast<uint64_t>(Memory::Read32(values[6].address)) << 32) |
                        Memory::Read32(values[6].address + 4));
                }
                if (values[7].size >= 8 && Memory::Contains(values[7].address, 8)) {
                    activity.endTimestamp = static_cast<int64_t>(
                        (static_cast<uint64_t>(Memory::Read32(values[7].address)) << 32) |
                        Memory::Read32(values[7].address + 4));
                }
                if (values[8].size >= 4) {
                    activity.partySize = Memory::Read32(values[8].address);
                }
                if (values[9].size >= 4) {
                    activity.partyMax = Memory::Read32(values[9].address);
                }
                DiscordPresence::SetActivity(std::move(activity));
            }
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_DISCORD_RESET:
            if (numIn != 0 || numOut != 0) {
                return ISFS_EINVAL;
            }
            if (RuntimeConfigFile::DiscordPresenceEnabled()) {
                DiscordPresence::Reset();
            }
            return ISFS_OK;

        case DOLPHIN_IOCTL_GET_SYSTEM_TIME: {
            if (!singleOut(8u, out)) {
                return ISFS_EINVAL;
            }
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const uint64_t value = static_cast<uint64_t>(nowMs);
            Memory::Write32(out.address, static_cast<uint32_t>(value >> 32));
            Memory::Write32(out.address + 4u, static_cast<uint32_t>(value));
            return ISFS_OK;
        }

        default:
            LogNandWarning("IOS_Ioctlv", "/dev/dolphin unsupported cmd=%u", cmd);
            return ISFS_EINVAL;
    }
}

static bool WriteShaOutputs(const ShaHandle& handle, const IosVector& context, const IosVector& hash) {
    CryptoPP::SHA1 snapshot = handle.hash;
    std::array<uint8_t, CryptoPP::SHA1::DIGESTSIZE> digest{};
    snapshot.Final(digest.data());
    if (!WriteGuestBytes(hash.address, hash.size, digest.data(), digest.size())) {
        return false;
    }

    if (context.address != 0 && context.size >= SHA_CONTEXT_SIZE && Memory::Contains(context.address, SHA_CONTEXT_SIZE)) {
        Memory::Write32(context.address + 0x00, 0x67452301u);
        Memory::Write32(context.address + 0x04, 0xEFCDAB89u);
        Memory::Write32(context.address + 0x08, 0x98BADCFEu);
        Memory::Write32(context.address + 0x0c, 0x10325476u);
        Memory::Write32(context.address + 0x10, 0xC3D2E1F0u);
        const uint64_t bitCount = handle.byteCount * 8u;
        Memory::Write32(context.address + 0x14, static_cast<uint32_t>(bitCount >> 32));
        Memory::Write32(context.address + 0x18, static_cast<uint32_t>(bitCount));
    }

    return true;
}

static int32_t HandleShaIoctlv(int32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
        return ISFS_EINVAL;
    }
    if (numIn != 1 || numOut != 2) {
        LogNandWarning("IOS_Ioctlv", "/dev/sha unsupported vector shape cmd=%u numIn=%u numOut=%u",
                cmd, numIn, numOut);
        return ISFS_EINVAL;
    }

    ShaHandle* handle = GetShaHandle(fd);
    if (!handle) {
        return ISFS_EINVAL;
    }

    const IosVector input = ReadIosVector(vectorPtr, 0);
    const IosVector context = ReadIosVector(vectorPtr, 1);
    const IosVector hash = ReadIosVector(vectorPtr, 2);
    if (!IsValidGuestRange(input.address, input.size) ||
        context.size < SHA_CONTEXT_SIZE || hash.size < SHA_DIGEST_SIZE ||
        !IsValidGuestRange(context.address, SHA_CONTEXT_SIZE) ||
        !IsValidGuestRange(hash.address, SHA_DIGEST_SIZE)) {
        LogNandWarning("IOS_Ioctlv",
                "/dev/sha invalid buffers cmd=%u in=0x%08X/%u ctx=0x%08X/%u hash=0x%08X/%u",
                cmd, input.address, input.size, context.address, context.size, hash.address, hash.size);
        return ISFS_EINVAL;
    }

    if (cmd == SHA_IOCTL_INIT) {
        handle->Restart();
    } else if (cmd != SHA_IOCTL_UPDATE && cmd != SHA_IOCTL_FINAL) {
        LogNandWarning("IOS_Ioctlv", "/dev/sha unsupported cmd=%u", cmd);
        return ISFS_EINVAL;
    }

    if (input.size != 0) {
        const uint8_t* bytes = Memory::GetPointer(input.address, input.size);
        handle->hash.Update(bytes, input.size);
        handle->byteCount += input.size;
    }

    if (!WriteShaOutputs(*handle, context, hash)) {
        return ISFS_EINVAL;
    }

    if (cmd == SHA_IOCTL_FINAL) {
        handle->Restart();
    }
    return ISFS_OK;
}

// /dev/stm lives in ios/ios.cpp.
extern "C" int32_t Stm_HLE_Open(const char* path);
extern "C" bool Stm_HLE_IsFd(uint32_t fd);
extern "C" int32_t Stm_HLE_Ioctl(uint32_t fd, uint32_t cmd, uint32_t outBuf, uint32_t outLen);
// /dev/sdio/slot0 lives in ios/sdio.cpp.
extern "C" int32_t Sdio_HLE_Open(const char* path);
extern "C" int32_t Di_HLE_Open(const char* path);
extern "C" bool Di_HLE_IsFd(uint32_t fd);
extern "C" int32_t Di_HLE_Ioctl(uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out,
                                uint32_t outLen, bool* held);
extern "C" bool Sdio_HLE_IsFd(uint32_t fd);
extern "C" int32_t Sdio_HLE_Close(uint32_t fd);
extern "C" int32_t Sdio_HLE_Ioctl(uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out,
                                  uint32_t outLen, uint32_t callback, uint32_t callbackArg,
                                  bool* held);
extern "C" int32_t Sdio_HLE_Ioctlv(uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr);

extern "C" int32_t NAND_IOS_Open_HLE(uint32_t pathPtr, uint32_t mode) {
    const std::string pathStorage = ReadGuestCString(pathPtr);
    const char* path = pathPtr == 0 ? nullptr : pathStorage.c_str();
    
    if (!path) {
        LogNandError("IOS_Open", "null path");
        return ISFS_EINVAL;
    }
    
    // Handle special device paths
    if (std::strncmp(path, "/dev/", 5) == 0) {
        if (std::strcmp(path, "/dev/fs") == 0) {
            return ISFS_DEV_FD;
        }
        if (std::strcmp(path, "/dev/es") == 0) {
            return ES_DEV_FD;
        }
        if (std::strcmp(path, "/dev/sha") == 0) {
            const int32_t fd = AllocateShaFd();
            return fd;
        }
        if (const int32_t netFd = Network_HLE_OpenDevice(path, mode)) {
            return netFd;
        }
        if (std::strcmp(path, "/dev/dolphin") == 0) {
            return DOLPHIN_DEV_FD;
        }
        if (const int32_t stmFd = Stm_HLE_Open(path)) {
            return stmFd;
        }
        if (const int32_t sdFd = Sdio_HLE_Open(path)) {
            return sdFd;
        }
        if (const int32_t diFd = Di_HLE_Open(path)) {
            return diFd;
        }
        LogNandWarning("IOS_Open", "unknown device '%s' mode=%u", path, mode);
        return ISFS_ENOENT;
    }
    
    // It's a NAND file path
    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (const auto result = NandCheckSystemSaveRead("IOS_Open", hostPath, mode, true))
        return *result;
    
    // Seed FaceLib resources before the existence check so every open mode can
    // still find them on a fresh managed NAND.
    if (!PathExists(hostPath) && IsFaceLibResourcePath(path)) {
        SeedFaceLibResource(hostPath);
    }

    // Determine file mode. IOS never creates files on open - creation happens
    // exclusively through ISFS CreateFile (which we implement). The previous
    // create-on-open fallback ("w+b") silently materialized 0-byte files (for
    // example /shared2/sys/net/02/config.dat) that later reads treated as
    // valid, poisoning persistent state across sessions.
    const char* fopenMode = "rb";
    if (mode == 2 || mode == 3) {
        if (!PathExists(hostPath)) {
            LogNandWarning("IOS_Open", "'%s' does not exist; open mode %u never creates it",
                    HostPathText(hostPath).c_str(), mode);
            return ISFS_ENOENT;
        }
        fopenMode = "r+b";      // Write-only opens still need read for seeks
    }

    FILE* file = NandFopen(hostPath, fopenMode);

    if (!file) {
        LogNandError("IOS_Open", "FAILED to open '%s'", HostPathText(hostPath).c_str());
        return ISFS_ENOENT;
    }
    
    int32_t fd = AllocateFd(hostPath, file, mode);
    if (fd < 0) {
        std::fclose(file);  // no handle to own it
        return fd;
    }
    return fd;
}
PPC_NATIVE_OVERRIDE(801938F8, NAND_IOS_Open_HLE, int32_t, (uint32_t pathPtr, uint32_t mode), (pathPtr, mode));

extern "C" void NAND_IOS_OpenBody_HLE_801938FC(CpuContext* ctx) {
    const int32_t result = NAND_IOS_Open_HLE(ctx->gpr[3], ctx->gpr[4]);
    ctx->gpr[3] = static_cast<uint32_t>(result);
    ctx->gpr[1] = ctx->gpr[1] + 32u;
}
REGISTER_NATIVE_FUNCTION_AS(0x801938FC, NAND_IOS_OpenBody_HLE_801938FC, "NAND_IOS_OpenBody_HLE_801938FC");

extern "C" int32_t NAND_IOS_Close_HLE(uint32_t fd) {
    if (Stm_HLE_IsFd(fd)) {
        return ISFS_OK;
    }
    if (Sdio_HLE_IsFd(fd)) {
        return Sdio_HLE_Close(fd);
    }
    if (Di_HLE_IsFd(fd)) {
        return ISFS_OK;
    }
    if (fd == ISFS_DEV_FD) {
        return ISFS_OK;
    }
    if (fd == ES_DEV_FD) {
        return ISFS_OK;
    }
    if (fd == DOLPHIN_DEV_FD) {
        return ISFS_OK;
    }
    if (GetShaHandle(static_cast<int32_t>(fd))) {
        CloseShaFd(static_cast<int32_t>(fd));
        return ISFS_OK;
    }
    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Close(fd);
    }
    
    auto* handle = GetHandle(fd);
    if (!handle) {
        LogNandError("IOS_Close", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    CloseFd(fd);
    return ISFS_OK;
}
PPC_NATIVE_OVERRIDE(80193AD8, NAND_IOS_Close_HLE, int32_t, (uint32_t fd), (fd));

extern "C" int32_t NAND_IOS_Read_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Read", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    uint8_t* buffer = (uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        LogNandError("IOS_Read", "invalid buffer ptr 0x%08X", bufferPtr);
        return ISFS_EINVAL;
    }
    
    size_t bytesRead = std::fread(buffer, 1, length, handle->file);
    handle->position += static_cast<uint32_t>(bytesRead);
    
    return static_cast<int32_t>(bytesRead);
}
PPC_NATIVE_OVERRIDE(80193C80, NAND_IOS_Read_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

extern "C" int32_t NAND_IOS_Write_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Write", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    const uint8_t* buffer = (const uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        LogNandError("IOS_Write", "invalid buffer ptr 0x%08X", bufferPtr);
        return ISFS_EINVAL;
    }

    if (!NandHasRoomFor(length)) {
        LogNandWarning("IOS_Write", "refusing %u byte(s): the NAND is full", length);
        return ISFS_ENOSPC;
    }
    
    size_t bytesWritten = std::fwrite(buffer, 1, length, handle->file);
    std::fflush(handle->file);
    handle->position += static_cast<uint32_t>(bytesWritten);
    
    return static_cast<int32_t>(bytesWritten);
}
PPC_NATIVE_OVERRIDE(80193E88, NAND_IOS_Write_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

extern "C" int32_t NAND_IOS_Seek_HLE(uint32_t fd, int32_t offset, int32_t whence) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Seek", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (std::fseek(handle->file, offset, NandSeekOrigin(whence)) != 0) {
        LogNandError("IOS_Seek", "fd=%d offset=%d whence=%d FAILED", fd, offset, whence);
        return ISFS_EIO;
    }
    
    handle->position = static_cast<uint32_t>(std::ftell(handle->file));
    return static_cast<int32_t>(handle->position);
}
PPC_NATIVE_OVERRIDE(80194070, NAND_IOS_Seek_HLE, int32_t, (uint32_t fd, int32_t offset, int32_t whence), (fd, offset, whence));

// ============================================================================
// IOS_Ioctl HLE - Handles filesystem commands
// ============================================================================

// ISFS Ioctl commands
enum ISFSCommand {
    ISFS_IOCTL_FORMAT = 1,
    ISFS_IOCTL_GETSTATS = 2,
    ISFS_IOCTL_CREATEDIR = 3,
    ISFS_IOCTL_READDIR = 4,
    ISFS_IOCTL_SETATTR = 5,
    ISFS_IOCTL_GETATTR = 6,
    ISFS_IOCTL_DELETE = 7,
    ISFS_IOCTL_RENAME = 8,
    ISFS_IOCTL_CREATEFILE = 9,
    ISFS_IOCTL_SETFILEVERCTRL = 10,
    ISFS_IOCTL_GETFILESTATS = 11,
    ISFS_IOCTL_GETUSAGE = 12,
    ISFS_IOCTL_SHUTDOWN = 13,
};

// The NAND's geometry, as Dolphin's Source/Core/Core/IOS/FS/FileSystem.h states
// it. A title asks how much room a directory takes before it writes a save, and
// answers invented on the spot let it believe it has no space, or all of it.
constexpr uint32_t kNandClusterSize = 16384;
constexpr uint32_t kNandTotalClusters = 0x7ec0;
constexpr uint32_t kNandReservedClusters = 0x0300;
constexpr uint32_t kNandUsableClusters = kNandTotalClusters - kNandReservedClusters;
constexpr uint32_t kNandTotalInodes = 0x17ff;

// Counts a directory the way the console does: every file and directory under it
// is one inode, and a file occupies whole clusters.
static void CountNandUsage(const std::filesystem::path& directory,
                           uint64_t& inodes, uint64_t& clusters) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = HostPathText(entry.path().filename());
        if (name.empty() || name.size() > 12) {
            continue;  // cannot exist on a real NAND; also hides write shadows
        }
        ++inodes;
        std::error_code entryEc;
        if (entry.is_directory(entryEc)) {
            CountNandUsage(entry.path(), inodes, clusters);
            continue;
        }
        const auto size = static_cast<uint64_t>(entry.file_size(entryEc));
        if (!entryEc) {
            clusters += (size + kNandClusterSize - 1) / kNandClusterSize;
        }
    }
}

extern "C" int32_t NAND_IOS_Ioctl_HLE(
    uint32_t fd,
    uint32_t cmd,
    uint32_t inBufPtr, uint32_t inLen,
    uint32_t outBufPtr, uint32_t outLen)
{

    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Ioctl(fd, cmd, inBufPtr, inLen, outBufPtr, outLen);
    }
    if (Stm_HLE_IsFd(fd)) {
        return Stm_HLE_Ioctl(fd, cmd, outBufPtr, outLen);
    }
    if (Sdio_HLE_IsFd(fd)) {
        return Sdio_HLE_Ioctl(cmd, inBufPtr, inLen, outBufPtr, outLen, 0, 0, nullptr);
    }
    if (Di_HLE_IsFd(fd)) {
        return Di_HLE_Ioctl(cmd, inBufPtr, inLen, outBufPtr, outLen, nullptr);
    }

    if (GetShaHandle(static_cast<int32_t>(fd))) {
        LogNandWarning("IOS_Ioctl", "/dev/sha does not support scalar ioctl cmd=%u", cmd);
        return ISFS_EINVAL;
    }
    if (fd == DOLPHIN_DEV_FD) {
        LogNandWarning("IOS_Ioctl", "/dev/dolphin does not support scalar ioctl cmd=%u", cmd);
        return ISFS_EINVAL;
    }
    
    // Handle /dev/fs ISFS commands
    if (fd == ISFS_DEV_FD) {
        switch (cmd) {
            case ISFS_IOCTL_CREATEDIR: {
                // ISFSParams: the path sits at 0x06, after uid and gid, and the
                // whole struct is 0x4a. Asking for more than that refuses a
                // request of exactly the right size.
                if (!inBufPtr || inLen < 0x4a || !Memory::Contains(inBufPtr, 0x4a)) {
                    return ISFS_EINVAL;
                }
                const std::string wiiPath = ReadGuestCString(inBufPtr + 6, 64);
                if (!NandPathIsValid(wiiPath) ||
                    !NandFilenameIsValid(NandPathBasename(wiiPath))) {
                    return ISFS_EINVAL;
                }
                if (NandPathDepth(wiiPath) > kNandMaxPathDepth) {
                    return ISFS_EINVAL;
                }
                const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());

                if (CreateDirectoryPath(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_DELETE: {
                if (!inBufPtr || inLen < 0x40) {
                    return ISFS_EINVAL;
                }
                const std::string wiiPath = ReadGuestCString(inBufPtr, 64);
                if (!NandPathIsValid(wiiPath)) {
                    return ISFS_EINVAL;
                }
                const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());

                // fs::remove refuses a non-empty directory, matching rmdir.
                if (NandRemove(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_ENOENT;
            }
            
            case ISFS_IOCTL_GETATTR: {
                if (!inBufPtr || !outBufPtr) {
                    return ISFS_EINVAL;
                }
                const std::string wiiPath = ReadGuestCString(inBufPtr, 64);
                if (!NandPathIsValid(wiiPath)) {
                    return ISFS_EINVAL;
                }
                const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());

                if (!PathExists(hostPath)) {
                    return ISFS_ENOENT;
                }
                
                // ISFSParams, packed, as Dolphin's FileSystemProxy.cpp declares it:
                //   uid u32 at 0x00, gid u16 at 0x04, path[64] at 0x06,
                //   modes (owner, group, other) at 0x46, attribute u8 at 0x49.
                //
                // The fields here were shifted by one: the value meant for owner
                // landed in the attribute, and whether the entry is a directory
                // was written into other's permissions. ISFSParams carries no
                // such flag - IOS keeps is_file in its own metadata, and a
                // caller learns it from GetFileStats or ReadDir instead.
                //
                // IOS does not even clear this struct, so a real console returns
                // stack leftovers in the rest of it. We zero it, as Dolphin does,
                // because a deterministic answer is worth more than a faithful
                // one nobody can rely on.
                if (outLen < 0x4a || !Memory::Contains(outBufPtr, 0x4a)) {
                    return ISFS_EINVAL;
                }
                for (uint32_t i = 0; i < 0x4a; ++i) {
                    Memory::Write8(outBufPtr + i, 0);
                }
                const NandMetadata meta = NandGetMetadata(wiiPath);
                Memory::Write32(outBufPtr + 0x00, meta.uid);
                Memory::Write16(outBufPtr + 0x04, meta.gid);
                Memory::Write8(outBufPtr + 0x46, meta.ownerMode);
                Memory::Write8(outBufPtr + 0x47, meta.groupMode);
                Memory::Write8(outBufPtr + 0x48, meta.otherMode);
                Memory::Write8(outBufPtr + 0x49, meta.attribute);
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_CREATEFILE: {
                if (!inBufPtr || inLen < 0x4a || !Memory::Contains(inBufPtr, 0x4a)) {
                    return ISFS_EINVAL;
                }
                const std::string wiiPath = ReadGuestCString(inBufPtr + 6, 64);
                // A name the FST cannot hold is refused here, not silently
                // written to a host filesystem that happens to allow it - and
                // then invisible to ReadDir, which drops names over twelve.
                if (!NandPathIsValid(wiiPath) ||
                    !NandFilenameIsValid(NandPathBasename(wiiPath))) {
                    return ISFS_EINVAL;
                }
                if (NandPathDepth(wiiPath) > kNandMaxPathDepth) {
                    return ISFS_EINVAL;
                }
                const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());
                CreateParentDirectories(hostPath);

                // Create empty file
                FILE* f = NandFopen(hostPath, "wb");
                if (f) {
                    std::fclose(f);
                    NandMarkSaveCreated(hostPath);
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETFILESTATS: {
                // GETFILESTATS is addressed to a file fd, never to /dev/fs.
                LogNandWarning("IOS_Ioctl", "GETFILESTATS on ISFS device - unexpected");
                return ISFS_EINVAL;
            }
            
            case ISFS_IOCTL_RENAME: {
                if (!inBufPtr || inLen < 0x80) {
                    return ISFS_EINVAL;
                }
                const std::string srcWii = ReadGuestCString(inBufPtr, 64);
                const std::string dstWii = ReadGuestCString(inBufPtr + 0x40, 64);
                if (!NandPathIsValid(srcWii) || !NandPathIsValid(dstWii)) {
                    return ISFS_EINVAL;
                }
                const std::filesystem::path srcHost = TranslateNandPath(srcWii.c_str());
                const std::filesystem::path dstHost = TranslateNandPath(dstWii.c_str());
                
                if (NandRename(srcHost, dstHost)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETSTATS: {
                // ISFSNandStats: seven counts, in this order. A title reads
                // cluster_size from the first of them, so getting the order
                // wrong tells it the NAND is built of 2 MB blocks.
                if (!outBufPtr || outLen < 0x1c || !Memory::Contains(outBufPtr, 0x1c)) {
                    return ISFS_EINVAL;
                }
                uint64_t inodes = 0;
                uint64_t clusters = 0;
                CountNandUsage(TranslateNandPath("/"), inodes, clusters);
                const uint32_t usedClusters =
                    static_cast<uint32_t>(std::min<uint64_t>(clusters, kNandUsableClusters));
                const uint32_t usedInodes =
                    static_cast<uint32_t>(std::min<uint64_t>(inodes, kNandTotalInodes));
                Memory::Write32(outBufPtr + 0x00, kNandClusterSize);
                Memory::Write32(outBufPtr + 0x04, kNandUsableClusters - usedClusters);
                Memory::Write32(outBufPtr + 0x08, usedClusters);
                NAND_APPROXIMATION("ISFS_GetStats bad clusters",
                                   "always zero; we have no flash to go bad");
                Memory::Write32(outBufPtr + 0x0C, 0);  // bad clusters
                Memory::Write32(outBufPtr + 0x10, kNandReservedClusters);
                Memory::Write32(outBufPtr + 0x14, kNandTotalInodes - usedInodes);
                Memory::Write32(outBufPtr + 0x18, usedInodes);
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_SETATTR: {
                // Kept in the metadata store, which GetAttr and NANDGetStatus
                // read back. The input is checked, so a malformed request is
                // answered as malformed rather than as success.
                if (!inBufPtr || inLen < 0x4a || !Memory::Contains(inBufPtr, 0x4a)) {
                    return ISFS_EINVAL;
                }
                const std::string wiiPath = ReadGuestCString(inBufPtr + 6, 64);
                if (!NandPathIsValid(wiiPath)) {
                    return ISFS_EINVAL;
                }
                if (!PathExists(TranslateNandPath(wiiPath.c_str()))) {
                    return ISFS_ENOENT;
                }
                NandMetadata meta{};
                meta.uid = Memory::Read32(inBufPtr + 0x00);
                meta.gid = Memory::Read16(inBufPtr + 0x04);
                meta.ownerMode = Memory::Read8(inBufPtr + 0x46);
                meta.groupMode = Memory::Read8(inBufPtr + 0x47);
                meta.otherMode = Memory::Read8(inBufPtr + 0x48);
                meta.attribute = Memory::Read8(inBufPtr + 0x49);
                NandSetMetadata(wiiPath, meta);
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_GETUSAGE:
                // GetUsage is a vectored call - a path in, two counts out - and
                // is answered in NAND_IOS_Ioctlv_HLE. Reaching it here means the
                // caller used a shape the console does not, and inventing counts
                // would have it size a save against numbers we made up.
                LogNandWarning("IOS_Ioctl", "GETUSAGE is ioctlv, not ioctl");
                return ISFS_EINVAL;
            
            case ISFS_IOCTL_READDIR:
                // Likewise vectored. Answering "zero entries, success" told the
                // Wii Menu its channel directory was empty.
                LogNandWarning("IOS_Ioctl", "READDIR is ioctlv, not ioctl");
                return ISFS_EINVAL;
            
            default:
                LogNandWarning("IOS_Ioctl", "unknown ISFS cmd=%u", cmd);
                return ISFS_OK;
        }
    }
    
    // Handle file-specific commands
    auto* handle = GetHandle(fd);
    if (handle && handle->file) {
        if (cmd == ISFS_IOCTL_GETFILESTATS) {
            // Get file stats
            if (!outBufPtr || outLen < 8) {
                return ISFS_EINVAL;
            }
            
            const NandFileExtent extent = NandProbeFileExtent(handle->file);
            Memory::Write32(outBufPtr, static_cast<uint32_t>(extent.size));
            Memory::Write32(outBufPtr + 4, static_cast<uint32_t>(extent.position));
            
            return ISFS_OK;
        }
    }
    
    // Unknown command - return success to not block game
    return ISFS_OK;
}

// The stack frame a guest thread parks on while a deferred network ioctl runs.
// `newStack` is always oldStack - kFrameSize, even when the frame could not be
// built, because the sleep path installs it unconditionally.
struct IosWaitFrame {
    bool valid = false;
    uint32_t oldStack = 0;
    uint32_t newStack = 0;
    uint32_t waitQueue = 0;
};

static IosWaitFrame InitializeIosWaitQueueFrame(CpuContext* ctx) {
    constexpr uint32_t kFrameSize = 0x40u;
    constexpr uint32_t kWaitQueueOffset = 0x30u;

    IosWaitFrame frame;
    frame.oldStack = ctx->gpr[1];
    frame.newStack = frame.oldStack - kFrameSize;
    if (frame.oldStack < kFrameSize || !Memory::Contains(frame.newStack, kFrameSize)) {
        return frame;
    }
    // Preserve the PPC linkage area and the required r3-r10 outgoing-argument
    // save area. The queue lives in local storage beyond sp+0x28 so a guest
    // switch callback cannot legally spill over it while this thread sleeps.
    Memory::Write32(frame.newStack, frame.oldStack);
    Memory::Write32(frame.newStack + 4u, 0);
    Memory::Write32(frame.newStack + kWaitQueueOffset, 0);
    Memory::Write32(frame.newStack + kWaitQueueOffset + 4u, 0);
    frame.waitQueue = frame.newStack + kWaitQueueOffset;
    frame.valid = true;
    return frame;
}

static void FinishDeferredIosWait(CpuContext* ctx, uint32_t oldStack, uint64_t token) {
    int32_t result = -101;
    if (!Network_HLE_TakeSyncResult(token, &result)) {
        RT_LOGF(RT_TAG_NAND,
                     "deferred network waiter resumed without result token=%llu\n",
                     static_cast<unsigned long long>(token));
    }
    ctx->gpr[1] = oldStack;
    ctx->gpr[3] = static_cast<uint32_t>(result);
}

// IOS_Ioctl and IOS_Ioctlv park a network request the same way: build the wait
// frame, hand its queue to the network layer, and either sleep on it or take the
// immediate answer. True when the request was handled here.
template <typename StartSync>
static bool TryDeferredNetworkIosSync(CpuContext* ctx, StartSync&& startSync) {
    const IosWaitFrame frame = InitializeIosWaitQueueFrame(ctx);
    const auto deferred = startSync(frame.valid ? frame.waitQueue : 0u);
    if (deferred.disposition == NetworkDeferredContract::StartDisposition::Started) {
        ctx->gpr[1] = frame.newStack;
        ctx->gpr[3] = frame.waitQueue;
        OSSleepThread_HLE_801aa9b8(ctx);
        FinishDeferredIosWait(ctx, frame.oldStack, deferred.token);
        return true;
    }
    if (deferred.disposition == NetworkDeferredContract::StartDisposition::ImmediateResult) {
        ctx->gpr[3] = static_cast<uint32_t>(deferred.result);
        return true;
    }
    return false;
}

extern "C" void NAND_IOS_Ioctl_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t inBufPtr = ctx->gpr[5];
    const uint32_t inLen = ctx->gpr[6];
    const uint32_t outBufPtr = ctx->gpr[7];
    const uint32_t outLen = ctx->gpr[8];

    if (Network_HLE_IsFd(fd)) {
        const bool handled = TryDeferredNetworkIosSync(ctx, [&](uint32_t waitQueue) {
            return Network_HLE_StartIoctlSync(fd, cmd, inBufPtr, inLen, outBufPtr, outLen, waitQueue);
        });
        if (handled) {
            return;
        }
    }

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctl_HLE(fd, cmd, inBufPtr, inLen, outBufPtr, outLen));
}
PPC_NATIVE_OVERRIDE_VOID(80194290, NAND_IOS_Ioctl_Entry_HLE, (CpuContext* ctx), (ctx));

// ============================================================================
// ISFS_OpenLib - Initialize ISFS
// ============================================================================

// Global state for ISFS initialization
static bool g_isfsInitialized = false;

// The ISFS/IPC globals ISFS_OpenLib touches, as negative r13 (SDA1) offsets.
// These are address-exact: they name the SDK's own variables, so the numbers are
// load-bearing and must not be "tidied". Names come from the RVL IPC/ISFS
// sources; only the naming changed here, never a value.
namespace {
constexpr uint32_t kIsfsFdSda1Offset = 29408u;              // __ISFS_fd
constexpr uint32_t kIsfsPathSda1Offset = 29400u;            // __ISFS_path ("/dev/fs")
constexpr uint32_t kIpcBufferLoSda1Offset = 25620u;         // IPC buffer window, low
constexpr uint32_t kIpcBufferHiSda1Offset = 25616u;         // IPC buffer window, high
constexpr uint32_t kIpcArenaLoSda1Offset = 25732u;          // __IPCArenaLo
constexpr uint32_t kIpcArenaHiSda1Offset = 25728u;          // __IPCArenaHi
constexpr uint32_t kIsfsHeapHandleSda1Offset = 25724u;      // ISFS heap handle
constexpr uint32_t kIsfsHeapBaseSda1Offset = 25740u;        // ISFS heap base address
constexpr uint32_t kIsfsHeapInitializedSda1Offset = 25744u; // ISFS heap created flag
} // namespace

static void WriteGuestString(uint32_t address, const char* value) {
    if (!value) {
        return;
    }
    const size_t length = std::strlen(value) + 1;
    if (!Memory::Contains(address, length)) {
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        Memory::Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(value[i]));
    }
}

int32_t ISFS_OpenLib_Initialize(CpuContext* ctx) {
    g_isfsInitialized = true;
    
    // Create the title data directory if it doesn't exist
    char titleId[32];
    std::snprintf(titleId, sizeof(titleId), "%08x", CurrentTitleIdHi());
    char gameId[32];
    std::snprintf(gameId, sizeof(gameId), "%08x", CurrentTitleIdLo());
    CreateDirectoryPath(GetNandBasePath() / "title" / titleId / gameId / "data");

    if (!ctx) {
        return ISFS_OK;
    }

    const uint32_t r13 = ctx->gpr[13];
    if (r13 == 0) {
        return ISFS_OK;
    }

    const uint32_t isfsFdGlobal = r13 - kIsfsFdSda1Offset;
    const uint32_t isfsPathGlobal = r13 - kIsfsPathSda1Offset;
    const uint32_t ipcBufferLoGlobal = r13 - kIpcBufferLoSda1Offset;
    const uint32_t ipcBufferHiGlobal = r13 - kIpcBufferHiSda1Offset;
    const uint32_t ipcArenaLoGlobal = r13 - kIpcArenaLoSda1Offset;
    const uint32_t ipcArenaHiGlobal = r13 - kIpcArenaHiSda1Offset;
    const uint32_t isfsHeapGlobal = r13 - kIsfsHeapHandleSda1Offset;
    const uint32_t isfsHeapBaseGlobal = r13 - kIsfsHeapBaseSda1Offset;
    const uint32_t isfsHeapInitializedGlobal = r13 - kIsfsHeapInitializedSda1Offset;

    WriteGuestString(isfsPathGlobal, "/dev/fs");

    if (Memory::Contains(isfsFdGlobal, 4)) {
        Memory::Write32(isfsFdGlobal, static_cast<uint32_t>(ISFS_DEV_FD));
    }

    // The heap bring-up below reads and writes all seven IPC globals, so it only
    // runs when every one of them is inside guest memory.
    for (const uint32_t global : {ipcBufferLoGlobal, ipcBufferHiGlobal, ipcArenaLoGlobal,
                                  ipcArenaHiGlobal, isfsHeapGlobal, isfsHeapBaseGlobal,
                                  isfsHeapInitializedGlobal}) {
        if (!Memory::Contains(global, 4)) {
            return ISFS_OK;
        }
    }

    uint32_t ipcLo = Memory::Read32(ipcBufferLoGlobal);
    uint32_t ipcHi = Memory::Read32(ipcBufferHiGlobal);
    if (ipcLo == 0 || ipcHi == 0 || ipcLo >= ipcHi) {
        return ISFS_OK;
    }

    if (Memory::Read32(isfsHeapInitializedGlobal) == 0) {
        Memory::Write32(ipcArenaLoGlobal, ipcLo);
        Memory::Write32(ipcArenaHiGlobal, ipcHi);

        const uint32_t heapBase = (ipcLo + 31u) & ~31u;
        const uint32_t heapSize = 5440u;
        if (heapBase + heapSize <= ipcHi) {
            Memory::Write32(isfsHeapBaseGlobal, heapBase);

            const uint32_t savedR3 = ctx->gpr[3];
            const uint32_t savedR4 = ctx->gpr[4];
            const uint32_t savedR5 = ctx->gpr[5];
            const uint32_t savedLr = ctx->lr;

            ctx->gpr[3] = heapBase;
            ctx->gpr[4] = heapSize;
            ctx->lr = 0x80169BCCu;
            InvokeDirectCpu<0x801949B8u>(ctx);
            const uint32_t heapHandle = ctx->gpr[3];

            ctx->gpr[3] = heapBase + heapSize;
            ctx->lr = 0x80169BCCu;
            InvokeDirectCpu<0x80193040u>(ctx);

            ctx->gpr[3] = savedR3;
            ctx->gpr[4] = savedR4;
            ctx->gpr[5] = savedR5;
            ctx->lr = savedLr;

            Memory::Write32(isfsHeapGlobal, heapHandle);
            Memory::Write32(isfsHeapInitializedGlobal, 1u);
        }
    }

    return ISFS_OK;
}

extern "C" void ISFS_OpenLib_HLE_80169BCC(CpuContext* ctx) {
    ctx->gpr[3] = static_cast<uint32_t>(ISFS_OpenLib_Initialize(ctx));
}

REGISTER_NATIVE_FUNCTION_AS(0x80169BCC, ISFS_OpenLib_HLE_80169BCC, "ISFS_OpenLib_HLE_80169BCC");

// ============================================================================
// IOS_Ioctlv HLE - Vector Ioctl for complex ISFS operations
// ============================================================================

// ISFS_GetUsage: one path in, two counts out - clusters first, then inodes.
static int32_t HandleIsfsGetUsage(uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    if (numIn != 1 || numOut != 2) {
        LogNandWarning("IOS_Ioctlv", "GETUSAGE unsupported vector shape numIn=%u numOut=%u",
                numIn, numOut);
        return ISFS_EINVAL;
    }
    const IosVector pathVec = ReadIosVector(vectorPtr, 0);
    const IosVector clusterOut = ReadIosVector(vectorPtr, 1);
    const IosVector inodeOut = ReadIosVector(vectorPtr, 2);
    if (pathVec.size != 64 || clusterOut.size < 4 || inodeOut.size < 4 ||
        !Memory::Contains(clusterOut.address, 4) || !Memory::Contains(inodeOut.address, 4)) {
        return ISFS_EINVAL;
    }
    const std::string wiiPath = ReadGuestCString(pathVec.address, 64);
    if (!NandPathIsValid(wiiPath)) {
        return ISFS_EINVAL;
    }
    if (NandPathDepth(wiiPath) > kNandMaxPathDepth) {
        return ISFS_EINVAL;  // a console reports too many components here
    }
    const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());
    if (!IsDirectory(hostPath)) {
        return ISFS_ENOENT;
    }

    uint64_t inodes = 0;
    uint64_t clusters = 0;
    CountNandUsage(hostPath, inodes, clusters);
    NAND_APPROXIMATION("ISFS_GetUsage",
                       "counts host files; a console counts its own FST entries, "
                       "which differ where a name could not be stored");
    Memory::Write32(clusterOut.address,
                    static_cast<uint32_t>(std::min<uint64_t>(clusters, kNandUsableClusters)));
    Memory::Write32(inodeOut.address,
                    static_cast<uint32_t>(std::min<uint64_t>(inodes, kNandTotalInodes)));
    return ISFS_OK;
}

static int32_t HandleIsfsReadDir(uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    const bool countOnly = (numIn == 1 && numOut == 1);
    if (!countOnly && !(numIn == 2 && numOut == 2)) {
        LogNandWarning("IOS_Ioctlv", "READDIR unsupported vector shape numIn=%u numOut=%u",
                numIn, numOut);
        return ISFS_EINVAL;
    }

    const IosVector pathVec = ReadIosVector(vectorPtr, 0);
    const std::string wiiPath = ReadGuestCString(pathVec.address, 64);
    if (!NandPathIsValid(wiiPath)) {
        return ISFS_EINVAL;
    }
    if (NandPathDepth(wiiPath) > kNandMaxPathDepth) {
        return ISFS_EINVAL;  // a console reports too many components here
    }
    const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());
    if (!IsDirectory(hostPath)) {
        return ISFS_ENOENT;
    }

    // NAND names are at most 12 characters; longer host names cannot exist on
    // a real NAND (this also hides *.nandsafe.tmp write shadows).
    constexpr size_t kMaxNandNameLength = 12;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(hostPath, ec)) {
        std::string name = HostPathText(entry.path().filename());
        if (name.empty() || name.size() > kMaxNandNameLength) {
            continue;
        }
        names.push_back(std::move(name));
    }
    // A console hands these back in the order its FST holds them, which is
    // newest first: Nintendo walks a linked list that new entries are pushed
    // onto the front of, and at least one game is known to depend on it
    // (Dolphin's ReadDirectory cites issue 10234). We keep no FST, so the
    // closest thing we have is when each file was written - which is the same
    // order for anything the game itself created, and arbitrary for the files
    // a title was installed with.
    std::sort(names.begin(), names.end());
    std::stable_sort(names.begin(), names.end(),
                     [&hostPath](const std::string& a, const std::string& b) {
                         std::error_code ec;
                         const auto ta = std::filesystem::last_write_time(hostPath / a, ec);
                         if (ec) return false;
                         const auto tb = std::filesystem::last_write_time(hostPath / b, ec);
                         if (ec) return false;
                         return ta > tb;  // newest first
                     });
    NAND_APPROXIMATION("ISFS_ReadDir order",
                       "newest first by write time; a console orders by its FST");

    if (countOnly) {
        const IosVector countOut = ReadIosVector(vectorPtr, 1);
        if (countOut.size < 4 || !Memory::Contains(countOut.address, 4)) {
            return ISFS_EINVAL;
        }
        Memory::Write32(countOut.address, static_cast<uint32_t>(names.size()));
        return ISFS_OK;
    }

    const IosVector maxVec = ReadIosVector(vectorPtr, 1);
    const IosVector namesOut = ReadIosVector(vectorPtr, 2);
    const IosVector countOut = ReadIosVector(vectorPtr, 3);
    if (maxVec.size < 4 || !Memory::Contains(maxVec.address, 4) ||
        countOut.size < 4 || !Memory::Contains(countOut.address, 4) ||
        !IsValidGuestRange(namesOut.address, namesOut.size)) {
        return ISFS_EINVAL;
    }
    const uint32_t maxCount = Memory::Read32(maxVec.address);

    constexpr uint32_t kEntryWindow = 13; // 12 chars + terminator
    uint32_t cursor = 0;
    uint32_t written = 0;
    for (const std::string& name : names) {
        if (written >= maxCount || cursor + kEntryWindow > namesOut.size) {
            break;
        }
        uint8_t* out = Memory::GetPointer(namesOut.address + cursor, kEntryWindow);
        std::memset(out, 0, kEntryWindow);
        std::memcpy(out, name.data(), name.size());
        cursor += static_cast<uint32_t>(name.size()) + 1;
        ++written;
    }
    Memory::Write32(countOut.address, written);
    return ISFS_OK;
}

extern "C" int32_t NAND_IOS_Ioctlv_HLE(
    uint32_t fd,
    uint32_t cmd,
    uint32_t numIn,
    uint32_t numOut,
    uint32_t vectorPtr)
{

    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Ioctlv(fd, cmd, numIn, numOut, vectorPtr);
    }
    if (Sdio_HLE_IsFd(fd)) {
        return Sdio_HLE_Ioctlv(cmd, numIn, numOut, vectorPtr);
    }

    if (GetShaHandle(static_cast<int32_t>(fd))) {
        return HandleShaIoctlv(static_cast<int32_t>(fd), cmd, numIn, numOut, vectorPtr);
    }

    if (fd == DOLPHIN_DEV_FD) {
        return HandleDolphinIoctlv(cmd, numIn, numOut, vectorPtr);
    }

    if (fd == ISFS_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }
        if (cmd == ISFS_IOCTL_READDIR) {
            return HandleIsfsReadDir(numIn, numOut, vectorPtr);
        }
        if (cmd == ISFS_IOCTL_GETUSAGE) {
            return HandleIsfsGetUsage(numIn, numOut, vectorPtr);
        }
        // Reporting success for a command nothing was done for is worse than
        // refusing it: the caller believes the filesystem changed, or reads the
        // buffer it asked us to fill and finds whatever was already there.
        LogNandWarning("IOS_Ioctlv", "/dev/fs cmd=%u not implemented", cmd);
        return ISFS_EINVAL;
    }

    if (fd == ES_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }

        switch (cmd) {
            case ES_IOCTL_GETOWNEDTITLECNT:
            case ES_IOCTL_GETOWNEDTITLES: {
                const auto owned = OwnedTitles();
                if (cmd == ES_IOCTL_GETOWNEDTITLECNT) {
                    const IosVector out = ReadIosVector(vectorPtr, 0);
                    if (numOut != 1 || out.size < 4 || !Memory::Contains(out.address, 4)) {
                        return ES_EINVAL;
                    }
                    Memory::Write32(out.address, static_cast<uint32_t>(owned.size()));
                    return ISFS_OK;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                const IosVector out = ReadIosVector(vectorPtr, 1);
                if (numIn != 1 || numOut != 1 || !Memory::Contains(in.address, 4)) {
                    return ES_EINVAL;
                }
                const uint32_t count = std::min<uint32_t>(Memory::Read32(in.address),
                                                          static_cast<uint32_t>(owned.size()));
                if (!IsValidGuestRange(out.address, count * 8u)) {
                    return ES_EINVAL;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    Memory::Write32(out.address + i * 8, static_cast<uint32_t>(owned[i] >> 32));
                    Memory::Write32(out.address + i * 8 + 4, static_cast<uint32_t>(owned[i]));
                }
                return ISFS_OK;
            }

            // Ticket views: a title id in, how many (a count out) or the views.
            case ES_IOCTL_GETVIEWCNT:
            case ES_IOCTL_GETVIEWS: {
                const IosVector idIn = ReadIosVector(vectorPtr, 0);
                if (numIn < 1 || idIn.size < 8 || !Memory::Contains(idIn.address, 8)) {
                    return ES_EINVAL;
                }
                const std::vector<uint8_t> tickets = StoredTickets(ReadTitleIdVector(idIn.address));
                const uint32_t have = static_cast<uint32_t>(tickets.size() / kTicketSize);
                if (cmd == ES_IOCTL_GETVIEWCNT) {
                    const IosVector out = ReadIosVector(vectorPtr, 1);
                    if (numOut != 1 || out.size < 4 || !Memory::Contains(out.address, 4)) {
                        return ES_EINVAL;
                    }
                    Memory::Write32(out.address, have);
                    return ISFS_OK;
                }
                const IosVector countIn = ReadIosVector(vectorPtr, 1);
                const IosVector out = ReadIosVector(vectorPtr, 2);
                if (numIn != 2 || numOut != 1 || !Memory::Contains(countIn.address, 4)) {
                    return ES_EINVAL;
                }
                const uint32_t count = std::min(Memory::Read32(countIn.address), have);
                if (!IsValidGuestRange(out.address, static_cast<uint32_t>(count * kTicketViewSize))) {
                    return ES_EINVAL;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    const auto view = TicketView(tickets.data() + i * kTicketSize);
                    std::memcpy(Memory::GetPointer(out.address + i * kTicketViewSize, kTicketViewSize),
                                view.data(), kTicketViewSize);
                }
                return ISFS_OK;
            }

            // The TMD view: its size for a title id, then the view itself.
            case ES_IOCTL_GETTMDVIEWCNT:
            case ES_IOCTL_GETTMDVIEWS:
            case ES_IOCTL_GETSTOREDTMDSIZE:
            case ES_IOCTL_GETSTOREDTMD: {
                const IosVector idIn = ReadIosVector(vectorPtr, 0);
                if (numIn < 1 || idIn.size < 8 || !Memory::Contains(idIn.address, 8)) {
                    return ES_EINVAL;
                }
                const uint64_t titleId = ReadTitleIdVector(idIn.address);
                const std::vector<uint8_t> tmd = StoredTmd(titleId);
                if (tmd.empty()) {
                    LogNandWarning("ES", "no TMD for title %016llx (cmd 0x%02x)",
                                   static_cast<unsigned long long>(titleId), cmd);
                    return ES_ENOENT;
                }
                const bool whole = cmd == ES_IOCTL_GETSTOREDTMDSIZE || cmd == ES_IOCTL_GETSTOREDTMD;
                const std::vector<uint8_t> bytes = whole ? tmd : TmdView(tmd);
                if (cmd == ES_IOCTL_GETTMDVIEWCNT || cmd == ES_IOCTL_GETSTOREDTMDSIZE) {
                    const IosVector out = ReadIosVector(vectorPtr, 1);
                    if (numOut != 1 || out.size < 4 || !Memory::Contains(out.address, 4)) {
                        return ES_EINVAL;
                    }
                    Memory::Write32(out.address, static_cast<uint32_t>(bytes.size()));
                    return ISFS_OK;
                }
                const IosVector out = ReadIosVector(vectorPtr, numIn);
                if (numOut != 1 || out.size < bytes.size() ||
                    !WriteGuestBytes(out.address, out.size, bytes.data(), bytes.size())) {
                    return ES_EINVAL;
                }
                return ISFS_OK;
            }

            // The contents of a title that are really in the NAND: same as
            // GetTitleContents, asked by id alone.
            case ES_IOCTL_GETSTOREDCONTENTCNT:
            case ES_IOCTL_GETSTOREDCONTENTS: {
                const IosVector idIn = ReadIosVector(vectorPtr, 0);
                if (numIn < 1 || !Memory::Contains(idIn.address, 8)) {
                    return ES_EINVAL;
                }
                const auto contents = StoredContents(ReadTitleIdVector(idIn.address));
                if (cmd == ES_IOCTL_GETSTOREDCONTENTCNT) {
                    const IosVector out = ReadIosVector(vectorPtr, 1);
                    if (numOut != 1 || !Memory::Contains(out.address, 4)) {
                        return ES_EINVAL;
                    }
                    Memory::Write32(out.address, static_cast<uint32_t>(contents.size()));
                    return ISFS_OK;
                }
                const IosVector countIn = ReadIosVector(vectorPtr, 1);
                const IosVector out = ReadIosVector(vectorPtr, 2);
                if (numIn != 2 || numOut != 1 || !Memory::Contains(countIn.address, 4)) {
                    return ES_EINVAL;
                }
                const uint32_t count = std::min<uint32_t>(Memory::Read32(countIn.address),
                                                          static_cast<uint32_t>(contents.size()));
                if (!IsValidGuestRange(out.address, count * 4u)) {
                    return ES_EINVAL;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    Memory::Write32(out.address + i * 4, contents[i]);
                }
                return ISFS_OK;
            }

            case ES_IOCTL_OPENCONTENT: {
                if (numIn != 1) {
                    return ES_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                if (in.size < 4 || !Memory::Contains(in.address, 4)) {
                    return ES_EINVAL;
                }
                return OpenTitleContent(CurrentTitleId(), Memory::Read32(in.address));
            }

            case ES_IOCTL_OPENTITLECONTENT: {
                if (numIn != 3) {
                    return ES_EINVAL;
                }
                const IosVector idIn = ReadIosVector(vectorPtr, 0);
                const IosVector indexIn = ReadIosVector(vectorPtr, 2);
                if (idIn.size < 8 || !Memory::Contains(idIn.address, 8) ||
                    indexIn.size < 4 || !Memory::Contains(indexIn.address, 4)) {
                    return ES_EINVAL;
                }
                const uint64_t titleId =
                    (static_cast<uint64_t>(Memory::Read32(idIn.address)) << 32) |
                    Memory::Read32(idIn.address + 4);
                return OpenTitleContent(titleId, Memory::Read32(indexIn.address));
            }

            case ES_IOCTL_READCONTENT: {
                if (numIn != 1 || numOut != 1) {
                    return ES_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                const IosVector out = ReadIosVector(vectorPtr, 1);
                if (in.size < 4 || !Memory::Contains(in.address, 4) || !IsValidGuestRange(out.address, out.size)) {
                    return ES_EINVAL;
                }
                std::FILE* file = ContentFile(Memory::Read32(in.address));
                if (file == nullptr) {
                    return ES_EINVAL;
                }
                if (out.size == 0) {
                    return 0;
                }
                return static_cast<int32_t>(
                    std::fread(Memory::GetPointer(out.address, out.size), 1, out.size, file));
            }

            case ES_IOCTL_SEEKCONTENT: {
                if (numIn != 3) {
                    return ES_EINVAL;
                }
                const IosVector cfdIn = ReadIosVector(vectorPtr, 0);
                const IosVector offsetIn = ReadIosVector(vectorPtr, 1);
                const IosVector whenceIn = ReadIosVector(vectorPtr, 2);
                if (!Memory::Contains(cfdIn.address, 4) || !Memory::Contains(offsetIn.address, 4) ||
                    !Memory::Contains(whenceIn.address, 4)) {
                    return ES_EINVAL;
                }
                std::FILE* file = ContentFile(Memory::Read32(cfdIn.address));
                const uint32_t whence = Memory::Read32(whenceIn.address);  // 0 set, 1 current, 2 end
                if (file == nullptr || whence > 2) {
                    return ES_EINVAL;
                }
                const int origin = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
                if (std::fseek(file, static_cast<int32_t>(Memory::Read32(offsetIn.address)), origin) != 0) {
                    return ES_EINVAL;
                }
                return static_cast<int32_t>(std::ftell(file));
            }

            case ES_IOCTL_CLOSECONTENT: {
                if (numIn != 1) {
                    return ES_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                if (!Memory::Contains(in.address, 4)) {
                    return ES_EINVAL;
                }
                const uint32_t cfd = Memory::Read32(in.address);
                std::FILE* file = ContentFile(cfd);
                if (file == nullptr) {
                    return ES_EINVAL;
                }
                std::fclose(file);
                g_contentFiles[cfd] = nullptr;
                return ISFS_OK;
            }

            case ES_IOCTL_GETDEVICEID: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                if (out.size < 4 || out.address == 0 || !Memory::Contains(out.address, 4)) {
                    return ISFS_EINVAL;
                }
                const WiiEsCrypto::Identity& identity = WiiEsCrypto::CurrentIdentity();
                Memory::Write32(out.address, identity.deviceId);
                return ISFS_OK;
            }

            // How many titles are installed. One out vector of four bytes.
            case ES_IOCTL_GETTITLECNT: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                if (out.size != 4 || !Memory::Contains(out.address, 4)) {
                    return ISFS_EINVAL;
                }
                const auto titles = InstalledTitles();
                Memory::Write32(out.address, static_cast<uint32_t>(titles.size()));
                LogNandWarning("ES_GetTitleCount", "%zu title(s) in the NAND", titles.size());
                return ISFS_OK;
            }

            // The ids themselves: how many were asked for comes in, and that
            // many - or as many as there are - go out, eight bytes each.
            case ES_IOCTL_GETTITLES: {
                if (numIn != 1 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                const IosVector out = ReadIosVector(vectorPtr, 1);
                if (in.size < 4 || !Memory::Contains(in.address, 4)) {
                    return ISFS_EINVAL;
                }
                const uint32_t wanted = Memory::Read32(in.address);
                const auto titles = InstalledTitles();
                const uint32_t count =
                    static_cast<uint32_t>(std::min<size_t>(wanted, titles.size()));
                if (!Memory::Contains(out.address, static_cast<size_t>(count) * 8u)) {
                    return ISFS_EINVAL;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    Memory::Write32(out.address + i * 8, static_cast<uint32_t>(titles[i] >> 32));
                    Memory::Write32(out.address + i * 8 + 4,
                                    static_cast<uint32_t>(titles[i] & 0xFFFFFFFFu));
                }
                return ISFS_OK;
            }

            // How many of a title's contents are here. A title id in, a count out.
            case ES_IOCTL_GETTITLECONTENTSCNT: {
                if (numIn != 1 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                const IosVector out = ReadIosVector(vectorPtr, 1);
                if (in.size < 8 || !Memory::Contains(in.address, 8) ||
                    out.size != 4 || !Memory::Contains(out.address, 4)) {
                    return ISFS_EINVAL;
                }
                const uint64_t titleId = (static_cast<uint64_t>(Memory::Read32(in.address)) << 32) |
                                         Memory::Read32(in.address + 4);
                const auto contents = StoredContents(titleId);
                Memory::Write32(out.address, static_cast<uint32_t>(contents.size()));
                return ISFS_OK;
            }

            // Their ids: the title in the first vector, how many were asked for
            // in the second, and that many four-byte ids out.
            case ES_IOCTL_GETTITLECONTENTS: {
                if (numIn != 2 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector idIn = ReadIosVector(vectorPtr, 0);
                const IosVector countIn = ReadIosVector(vectorPtr, 1);
                const IosVector out = ReadIosVector(vectorPtr, 2);
                if (idIn.size < 8 || !Memory::Contains(idIn.address, 8) ||
                    countIn.size < 4 || !Memory::Contains(countIn.address, 4)) {
                    return ISFS_EINVAL;
                }
                const uint64_t titleId =
                    (static_cast<uint64_t>(Memory::Read32(idIn.address)) << 32) |
                    Memory::Read32(idIn.address + 4);
                const uint32_t wanted = Memory::Read32(countIn.address);
                const auto contents = StoredContents(titleId);
                const uint32_t count =
                    static_cast<uint32_t>(std::min<size_t>(wanted, contents.size()));
                if (!Memory::Contains(out.address, static_cast<size_t>(count) * 4u)) {
                    return ISFS_EINVAL;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    Memory::Write32(out.address + i * 4, contents[i]);
                }
                return ISFS_OK;
            }

            // Where a title keeps its saves. A title id in, the path out - and
            // the directory is created if it is not there, because a channel
            // that is told where to save then tries to, and a console has the
            // directory already from when the title was installed.
            case ES_IOCTL_GETTITLEDIR: {
                if (numIn != 1 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                const IosVector out = ReadIosVector(vectorPtr, 1);
                if (in.size < 8 || !Memory::Contains(in.address, 8)) {
                    return ISFS_EINVAL;
                }
                const uint32_t high = Memory::Read32(in.address);
                const uint32_t low = Memory::Read32(in.address + 4);
                char wiiPath[40];
                const int written = std::snprintf(wiiPath, sizeof(wiiPath),
                                                  "/title/%08x/%08x/data", high, low);
                if (written <= 0 || out.size < static_cast<uint32_t>(written) + 1u) {
                    return ISFS_EINVAL;
                }
                CreateDirectoryPath(TranslateNandPath(wiiPath));
                if (!WriteGuestBytes(out.address, out.size,
                                     reinterpret_cast<const uint8_t*>(wiiPath),
                                     static_cast<size_t>(written) + 1u)) {
                    return ISFS_EINVAL;
                }
                return ISFS_OK;
            }

            case ES_IOCTL_GETDEVICECERT: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                const auto cert = WiiEsCrypto::GetDeviceCertificate();
                if (!WriteGuestBytes(out.address, out.size, cert.data(), cert.size())) {
                    return ISFS_EINVAL;
                }
                return ISFS_OK;
            }

            case ES_IOCTL_GETTITLEID: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                if (out.size < 8 || out.address == 0 || !Memory::Contains(out.address, 8)) {
                    return ISFS_EINVAL;
                }
                const uint64_t titleId = CurrentTitleId();
                Memory::Write32(out.address, static_cast<uint32_t>(titleId >> 32));
                Memory::Write32(out.address + 4u, static_cast<uint32_t>(titleId));
                return ISFS_OK;
            }

            case ES_IOCTL_SIGN: {
                if (numIn != 1 || numOut != 2) {
                    return ISFS_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                const IosVector sigOut = ReadIosVector(vectorPtr, 1);
                const IosVector certOut = ReadIosVector(vectorPtr, 2);
                if (in.address == 0 || !Memory::Contains(in.address, in.size)) {
                    return ISFS_EINVAL;
                }
                const uint8_t* input = Memory::GetPointer(in.address, in.size);
                WiiEsCrypto::EcSignature signature{};
                WiiEsCrypto::EccCert cert{};
                WiiEsCrypto::Sign(CurrentTitleId(), input, in.size, signature, cert);
                if (!WriteGuestBytes(sigOut.address, sigOut.size, signature.data(), signature.size()) ||
                    !WriteGuestBytes(certOut.address, certOut.size, cert.data(), cert.size())) {
                    return ISFS_EINVAL;
                }
                return ISFS_OK;
            }

            default:
                LogNandWarning("IOS_Ioctlv", "unsupported /dev/es cmd=%u", cmd);
                return ISFS_EINVAL;
        }
    }
    
    // Non-device ioctlv has no ISFS command we need to service.
    return ISFS_OK;
}

extern "C" void NAND_IOS_Ioctlv_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t numIn = ctx->gpr[5];
    const uint32_t numOut = ctx->gpr[6];
    const uint32_t vectorPtr = ctx->gpr[7];

    if (Network_HLE_IsFd(fd)) {
        const bool handled = TryDeferredNetworkIosSync(ctx, [&](uint32_t waitQueue) {
            return Network_HLE_StartIoctlvSync(fd, cmd, numIn, numOut, vectorPtr, waitQueue);
        });
        if (handled) {
            return;
        }
    }

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctlv_HLE(fd, cmd, numIn, numOut, vectorPtr));
}
PPC_NATIVE_OVERRIDE_VOID(801945E0, NAND_IOS_Ioctlv_Entry_HLE, (CpuContext* ctx), (ctx));
