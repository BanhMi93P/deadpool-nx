// banjo-nx (2026-10-29): reNut upstream's DLC installer (ab60251, src/renut_engine/app/dlc_install.cpp; GPL-3.0, credit reNut),
// the DLC folder next to the NRO / exe (sdmc:/switch/banjo-nx/dlc on the Switch). katamari-nx (2026-10-31): the same
// file, game-agnostic (the running title's packages only): sdmc:/switch/katamari-nx/dlc, Beautiful Katamari's 16 packs.
#include "renut_engine/dlc_install.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>


#include "renut_engine/renut_logging.h"

REXCVAR_DEFINE_STRING(renut_dlc_folder, "dlc", "Nuts&Bolts/DLC",
    "Folder of DLC packages (Xbox Live content files) reNut installs when the game starts, relative to "
    "reNut's folder.");
// katamari-nx (2026-11-01, the user's request): Beautiful Katamari's packs that only unlock presents and cousins the
// game unlocks by itself, which takes the fun out of unlocking them; off by default (the user, the same day). The
// stage packs (Shopping Spree, Milky Way Constellations, High-calorie, Dangerous, Mechanical and Perfectly Sized
// Katamari, The Weaver Girl) are not in it.
REXCVAR_DEFINE_BOOL(katamari_dlc_unlock_packs, false, "Nuts&Bolts/DLC",
    "Beautiful Katamari's DLC packs that only unlock presents and cousins (Accessory Sets A-D, Character Sets A-D, "
    "Special Accessory): false (the default) keeps them from the game, though they stay installed; true shows them "
    "too. The stage packs are shown either way.");
REXCVAR_DECLARE(std::string, dlc_hidden);  // sdk/src/system/xam/content_manager.cpp
REXCVAR_DECLARE(bool, dlc_enabled);  // sdk/src/system/xam/content_manager.cpp

namespace renut::dlc {

namespace {

namespace fs = std::filesystem;

// The display names of the packs katamari_dlc_unlock_packs = false hides (their package headers' names).
constexpr std::array<const char *, 9> kKatamariUnlockPacks = {
    "Accessory Set A", "Accessory Set B", "Accessory Set C", "Accessory Set D", "Character Set A",
    "Character Set B", "Character Set C", "Character Set D", "Special Accessory"};

// An Xbox 360 package's header (big-endian): the magic, then at 0x344 the
// content type, 0x360 the title ID and 0x411 the display name (UTF-16).
constexpr size_t kHeaderBytes = 0x500;
constexpr size_t kContentTypeOffset = 0x344;
constexpr size_t kTitleIdOffset = 0x360;
constexpr size_t kDisplayNameOffset = 0x411;
constexpr size_t kDisplayNameChars = 64;
constexpr uint32_t kContentTypeMarketplace = 2;
// Where an installed package's header goes in the user data folder: the
// default profile's content, by title and content type.
constexpr const char kProfileFolder[] = "0000000000000000";
constexpr const char kContentTypeFolder[] = "00000002";
// Packages installed before, by file name and size, so an unchanged one is
// not installed again when the user data folder isn't known.
constexpr const char kInstalledList[] = "installed.txt";

fs::path g_user_data_root;

uint32_t LoadBe32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct Package {
    uint32_t content_type = 0;
    uint32_t title_id = 0;
    std::string name;  // display name (ASCII part)
};

// The package's header, or nothing if the file isn't an Xbox 360 package.
bool ReadPackage(const fs::path &path, Package &out) {
    std::ifstream f(path, std::ios::binary);
    std::array<uint8_t, kHeaderBytes> h{};
    if (!f.read(reinterpret_cast<char *>(h.data()), h.size()))
        return false;
    const std::string magic(reinterpret_cast<const char *>(h.data()), 4);
    if (magic != "LIVE" && magic != "PIRS" && magic != "CON ")
        return false;
    out.content_type = LoadBe32(h.data() + kContentTypeOffset);
    out.title_id = LoadBe32(h.data() + kTitleIdOffset);
    out.name.clear();
    for (size_t i = 0; i < kDisplayNameChars; ++i) {
        const uint8_t *c = h.data() + kDisplayNameOffset + 2 * i;
        const uint16_t ch = uint16_t(c[0] << 8 | c[1]);
        if (!ch)
            break;
        out.name.push_back(ch < 0x80 ? char(ch) : '?');
    }
    return true;
}

std::string HexTitle(uint32_t title_id) {
    char s[9];
    std::snprintf(s, sizeof(s), "%08X", title_id);
    return s;
}

// The header the install writes for 'file_name', if the user data folder is known.
fs::path InstalledHeader(uint32_t title_id, const std::string &file_name) {
    if (g_user_data_root.empty())
        return {};
    return g_user_data_root / kProfileFolder / HexTitle(title_id) / "Headers" / kContentTypeFolder /
           (file_name + ".header");
}

std::vector<std::string> ReadInstalledList(const fs::path &list) {
    std::vector<std::string> lines;
    std::ifstream f(list);
    for (std::string line; std::getline(f, line);)
        if (!line.empty())
            lines.push_back(line);
    return lines;
}

std::string LowerAscii(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void CollectFilesRecursive(const fs::path &dir, std::vector<fs::path> &files,
                           std::vector<std::string> &seen_keys) {
    for (const auto &info : rex::filesystem::ListFiles(dir)) {
        const std::string fname = rex::path_to_utf8(info.name);
        if (fname.empty() || fname.front() == '.' || fname == kInstalledList)
            continue;
        const fs::path full = dir / info.name;
        if (info.type == rex::filesystem::FileInfo::Type::kDirectory) {
            CollectFilesRecursive(full, files, seen_keys);
            continue;
        }
        if (info.type != rex::filesystem::FileInfo::Type::kFile)
            continue;
        std::string key = rex::path_to_utf8(full);
        std::replace(key.begin(), key.end(), '\\', '/');
        key = LowerAscii(std::move(key));
        if (std::find(seen_keys.begin(), seen_keys.end(), key) == seen_keys.end()) {
            seen_keys.push_back(std::move(key));
            files.push_back(full);
        }
    }
}

std::string ListEntry(const std::string &file_name, uintmax_t size) {
    return file_name + " " + std::to_string(size);
}

} // namespace

void SetUserDataRoot(const fs::path &root) {
    g_user_data_root = root;
}

void InstallPackages(rex::Runtime *runtime) {
    if (!runtime || !runtime->kernel_state())
        return;
    if (!REXCVAR_GET(dlc_enabled))
        return;
    fs::path folder = rex::to_path(REXCVAR_GET(renut_dlc_folder));
    if (folder.is_relative())
        folder = rex::filesystem::GetExecutableFolder() / folder;  // banjo-nx: next to the NRO / exe
    while (folder.filename().empty() && !folder.parent_path().empty())
        folder = folder.parent_path();
    std::error_code ec;
    fs::create_directories(folder, ec);

    std::vector<fs::path> scan_roots = {folder};
    if (LowerAscii(rex::path_to_utf8(folder.filename())) == "dlc") {
        const fs::path alt = folder.parent_path() / "DLC";
        rex::filesystem::FileInfo alt_info{};
        if (rex::filesystem::GetInfo(alt, &alt_info) &&
            alt_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
            scan_roots.push_back(alt);
        }
    }

    std::vector<fs::path> files;
    std::vector<std::string> seen_keys;
    for (const fs::path &root_dir : scan_roots)
        CollectFilesRecursive(root_dir, files, seen_keys);
    std::sort(files.begin(), files.end());

    const uint32_t title_id = runtime->kernel_state()->title_id();
    std::fprintf(stderr, "[dlc] scan folder='%s' alt='%s' user_root='%s' title=%08X -> %zu file(s)\n",
                 rex::path_to_utf8(folder).c_str(),
                 scan_roots.size() > 1 ? rex::path_to_utf8(scan_roots[1]).c_str() : "",
                 rex::path_to_utf8(g_user_data_root).c_str(), title_id, files.size());
    std::fflush(stderr);
    if (files.empty())
        return;

    const fs::path list_path = folder / kInstalledList;
    std::vector<std::string> installed = ReadInstalledList(list_path);
    bool list_changed = false;
    int newly = 0, present = 0;

    for (const fs::path &path : files) {
        const std::string file_name = rex::path_to_utf8(path.filename());
        Package package;
        if (!ReadPackage(path, package)) {
            RNUT_WARN("DLC: {} is not an Xbox 360 package; left alone", file_name);
            std::fprintf(stderr, "[dlc] skip '%s': not an Xbox 360 package\n", file_name.c_str());
            continue;
        }
        if (package.title_id != title_id) {
            RNUT_WARN("DLC: {} ({}) is for another game (title {:08X}); left alone", file_name, package.name,
                      package.title_id);
            std::fprintf(stderr, "[dlc] skip '%s' (%s): other title %08X (expected %08X)\n", file_name.c_str(),
                         package.name.c_str(), package.title_id, title_id);
            continue;
        }
        if (package.content_type != kContentTypeMarketplace) {
            RNUT_WARN("DLC: {} ({}) is not DLC (content type {:X}); left alone", file_name, package.name,
                      package.content_type);
            std::fprintf(stderr, "[dlc] skip '%s' (%s): not DLC (type 0x%X)\n", file_name.c_str(),
                         package.name.c_str(), package.content_type);
            continue;
        }
        rex::filesystem::FileInfo file_info{};
        const uintmax_t size =
            rex::filesystem::GetInfo(path, &file_info) ? static_cast<uintmax_t>(file_info.total_size) : 0;
        const std::string entry = ListEntry(file_name, size);
        const fs::path header = InstalledHeader(title_id, file_name);
        const fs::path data_dir =
            header.empty() ? fs::path{}
                           : header.parent_path().parent_path().parent_path() / kContentTypeFolder / file_name;
        const bool listed = std::find(installed.begin(), installed.end(), entry) != installed.end();
        rex::filesystem::FileInfo header_info{};
        const bool header_there = !header.empty() && rex::filesystem::GetInfo(header, &header_info) &&
                                  header_info.type == rex::filesystem::FileInfo::Type::kFile &&
                                  header_info.total_size > 0;
        rex::filesystem::FileInfo data_info{};
        const bool data_there = !data_dir.empty() && rex::filesystem::GetInfo(data_dir, &data_info) &&
                                data_info.type == rex::filesystem::FileInfo::Type::kDirectory &&
                                !rex::filesystem::ListFiles(data_dir).empty();
        if ((header_there && data_there) || (header.empty() && listed)) {
            std::fprintf(stderr, "[dlc] already installed: '%s' (%s) dir='%s'\n", package.name.c_str(),
                         file_name.c_str(), rex::path_to_utf8(data_dir).c_str());
            ++present;
            continue;
        }

        RNUT_INFO("DLC: installing {} ({})...", package.name, file_name);
        std::fprintf(stderr, "[dlc] installing '%s' (%s)...\n", package.name.c_str(), file_name.c_str());
        std::fflush(stderr);
        const std::string utf8 = rex::path_to_utf8(path);
        const auto result = runtime->kernel_state()->content_manager()->InstallContent(utf8.c_str());
        if (result != 0) {
            RNUT_ERROR("DLC: {} ({}) did not install (error {:08X})", package.name, file_name, unsigned(result));
            std::fprintf(stderr, "[dlc] FAILED '%s' (%s): error=0x%08X\n", package.name.c_str(),
                         file_name.c_str(), unsigned(result));
            std::fflush(stderr);
            continue;
        }
        RNUT_INFO("DLC: {} installed", package.name);
        std::fprintf(stderr, "[dlc] installed '%s' (%s)\n", package.name.c_str(), file_name.c_str());
        std::fflush(stderr);
        ++newly;
        if (!listed) {
            installed.push_back(entry);
            list_changed = true;
        }
    }

    if (list_changed) {
        std::ofstream f(list_path, std::ios::trunc);
        for (const std::string &line : installed)
            f << line << "\n";
    }
    if (newly || present) {
        RNUT_INFO("DLC: {} package(s) installed now, {} already installed", newly, present);
        std::fprintf(stderr, "[dlc] summary: %d newly installed, %d already installed\n", newly, present);
    }
    std::fflush(stderr);
}

} // namespace renut::dlc
