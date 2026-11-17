// SPDX-FileCopyrightText: Copyright 2026 P1R4N351
// SPDX-License-Identifier: GPL-3.0-or-later

// Tears of the Kingdom's romfs as the companion reads it: every *.zs file is a zstd frame
// compressed with one of the dictionaries in Pack/ZsDic.pack.zs (a plain zstd frame holding a
// SARC of *.zsdic). Reads go through the host's read_romfs; nothing is cached on disk.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wonder_assets.h"

namespace TotkRomfs {

/// The raw bytes of a romfs path (host read_romfs), at most 64 MiB.
using RawReader = WonderAssets::RomfsReader;

class Reader {
public:
    explicit Reader(RawReader raw);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// A file's bytes; "*.zs" files decompressed (with the frame's dictionary). A leading '/' is
    /// ignored. Thread-safe.
    std::optional<std::vector<std::uint8_t>> Read(std::string_view path) const;
    /// The same as a WonderAssets reader: hands Wonder's SARC/BNTX/font code decompressed bytes
    /// (its own Zstd() leaves bytes that are not a zstd frame unchanged).
    WonderAssets::RomfsReader AsWonderReader() const;
    /// True once the dictionaries are loaded (false: no TotK romfs, or a broken one).
    bool Ready() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// The display text of an MSBT label set: label -> UTF-8 (control tags dropped).
std::unordered_map<std::string, std::string> ParseMsbt(const std::vector<std::uint8_t>& msbt);

} // namespace TotkRomfs
