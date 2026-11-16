// SPDX-License-Identifier: GPL-3.0-or-later
// Where the companion's assets come from in the native port. Replaces the two functions of Eden Duo's
// mod_assets.cpp that open the emulator's content (they are compiled out there under DUO_PORT) and stands in for
// mod_romfs_sources.cpp, mod_user_source.cpp and mod_sysfont_source.cpp, which are not vendored.
//
//   file:    the package's own dualscreen/ folder (a real directory)
//   romfs:   the game's RomFS as the port mounts it, the player's own files: fonts, icons, text, map tiles
//   aoc:     the add-on content volume, when the port has one mounted
//   module:  bytes the title module generates
// Not offered: "base:" (the port has one RomFS, already without an update layer), "user:" and "sysfont:". A package
// that names them reads nothing, which is the registry's normal answer for a missing file.

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {

void ModRuntime::RegisterAssetSources() {
    asset_sources->Register({.prefix = "file", .open_dir = [this] { return manifest.asset_dir; }});
    // Asked again at most once a second while the port has not mounted the RomFS yet.
    asset_sources->Register(
        {.prefix = "romfs", .open_dir = [this] { return OpenGameRomFS(); }, .retry_ms = 1000});
    asset_sources->Register(
        {.prefix = "module",
         .read_bytes = [this](const std::string& src) { return LoadModuleData(src); }});
    asset_sources->Register({.prefix = "aoc",
                             .open_dir = [] { return FileSys::OpenPortAocRomFS(); },
                             .capability = EDEN_DSMOD_CAP_SOURCE_AOC,
                             .retry_ms = 5000});
}

FileSys::VirtualDir ModRuntime::OpenGameRomFS() {
    if (!IsOwnerContext()) {
        return nullptr;
    }
    auto root = FileSys::OpenPortRomFS();
    static std::atomic<u32> unavailable_logged{0};
    if (root || unavailable_logged.fetch_add(1, std::memory_order_relaxed) < 3) {
        LOG_INFO(Core, "DSMod: game romfs {}", root ? "opened" : "unavailable");
    }
    return root;
}

} // namespace Core::Mods
