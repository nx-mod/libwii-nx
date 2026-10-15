// The boot ROM's two system fonts (rom.h).

#include "rom.h"

#include "runtime_log.h"
#include "switch_layout.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace wiinx::system::rom {
namespace {

constexpr std::uint32_t kRomSize = 0x200000;
constexpr std::uint32_t kJapaneseFontOffset = 0x1AFF00;
constexpr std::uint32_t kJapaneseFontSize = 0x4D000;
constexpr std::uint32_t kWesternFontOffset = 0x1FCF00;
constexpr std::uint32_t kWesternFontSize = 0x3000;

std::once_flag g_loadOnce;
std::vector<std::uint8_t> g_image;  // kRomSize when anything was found, else empty

// Reads a whole file, or nothing when it is missing or larger than `limit`.
std::vector<std::uint8_t> ReadFile(const std::string& path, std::size_t limit) {
    std::vector<std::uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return bytes;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size > 0 && static_cast<std::size_t>(size) <= limit) {
        bytes.resize(static_cast<std::size_t>(size));
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
            bytes.clear();
        }
    } else if (size > 0) {
        RT_LOG(RT_TAG_OS) << "rom: " << path << " is " << size << " bytes, more than its " << limit
                          << "-byte slot; not used" << std::endl;
    }
    std::fclose(file);
    return bytes;
}

// Places a font file at its slot in the image; true when it was there.
bool PlaceFont(const char* name, std::uint32_t offset, std::uint32_t size) {
    const std::vector<std::uint8_t> font = ReadFile(SwitchLayout::System(name), size);
    if (font.empty()) {
        return false;
    }
    std::memcpy(g_image.data() + offset, font.data(), font.size());
    RT_LOG(RT_TAG_OS) << "rom: " << name << " (" << font.size() << " bytes) at 0x" << std::hex << offset
                      << std::dec << std::endl;
    return true;
}

void Load() {
    // (the fonts are stored plainly in a dump; only the boot code before them is
    // scrambled, and no game reads that)
    std::vector<std::uint8_t> dump = ReadFile(SwitchLayout::System("ipl.bin"), kRomSize);
    if (dump.size() == kRomSize) {
        g_image = std::move(dump);
        RT_LOG(RT_TAG_OS) << "rom: fonts from system/ipl.bin" << std::endl;
        return;
    }
    g_image.assign(kRomSize, 0);
    const bool japanese = PlaceFont("font_japanese.bin", kJapaneseFontOffset, kJapaneseFontSize);
    const bool western = PlaceFont("font_western.bin", kWesternFontOffset, kWesternFontSize);
    if (!japanese && !western) {
        g_image.clear();
        RT_LOG(RT_TAG_OS) << "rom: no system font on the SD card (system/ipl.bin, or system/font_western.bin "
                             "and system/font_japanese.bin); ROM reads answer zeros"
                          << std::endl;
    }
}

}  // namespace

bool Read(std::uint32_t offset, std::uint8_t* out, std::size_t length) {
    std::call_once(g_loadOnce, Load);
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint64_t at = static_cast<std::uint64_t>(offset) + i;
        out[i] = at < g_image.size() ? g_image[at] : 0;
    }
    return !g_image.empty();
}

}  // namespace wiinx::system::rom
