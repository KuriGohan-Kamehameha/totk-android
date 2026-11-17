// SPDX-FileCopyrightText: Copyright 2026 P1R4N351
// SPDX-License-Identifier: GPL-3.0-or-later

#include "totk_romfs.h"

#include <cstring>
#include <mutex>

#include <zstd.h>

namespace TotkRomfs {
namespace {

constexpr std::uint32_t ZstdMagic = 0xFD2FB528u;
constexpr std::size_t MaxOutput = 128u * 1024 * 1024;

std::uint32_t Le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
           std::uint32_t(p[3]) << 24;
}
std::uint16_t Le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}
bool IsZstd(const std::vector<std::uint8_t>& b) {
    return b.size() >= 4 && Le32(b.data()) == ZstdMagic;
}

// One zstd frame, with a dictionary when one is given. The content size is in every TotK frame;
// a frame without one is decoded by streaming into a growing buffer.
std::optional<std::vector<std::uint8_t>> Decompress(const std::vector<std::uint8_t>& in,
                                                    const ZSTD_DDict* dict) {
    ZSTD_DCtx* const dctx = ZSTD_createDCtx();
    if (!dctx)
        return std::nullopt;
    std::optional<std::vector<std::uint8_t>> result;
    const unsigned long long size = ZSTD_getFrameContentSize(in.data(), in.size());
    if (size != ZSTD_CONTENTSIZE_ERROR && size != ZSTD_CONTENTSIZE_UNKNOWN && size <= MaxOutput) {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
        const std::size_t got =
            dict ? ZSTD_decompress_usingDDict(dctx, out.data(), out.size(), in.data(), in.size(), dict)
                 : ZSTD_decompressDCtx(dctx, out.data(), out.size(), in.data(), in.size());
        if (!ZSTD_isError(got) && got == out.size())
            result = std::move(out);
    } else if (size == ZSTD_CONTENTSIZE_UNKNOWN) {
        if (dict)
            ZSTD_DCtx_refDDict(dctx, dict);
        std::vector<std::uint8_t> out;
        std::vector<std::uint8_t> chunk(ZSTD_DStreamOutSize());
        ZSTD_inBuffer src{in.data(), in.size(), 0};
        bool ok = true;
        while (ok && src.pos < src.size) {
            ZSTD_outBuffer dst{chunk.data(), chunk.size(), 0};
            const std::size_t r = ZSTD_decompressStream(dctx, &dst, &src);
            ok = !ZSTD_isError(r) && out.size() + dst.pos <= MaxOutput;
            out.insert(out.end(), chunk.data(), chunk.data() + dst.pos);
            if (r == 0)
                break;
        }
        if (ok)
            result = std::move(out);
    }
    ZSTD_freeDCtx(dctx);
    return result;
}

std::string_view Trim(std::string_view path) {
    while (!path.empty() && path.front() == '/')
        path.remove_prefix(1);
    return path;
}

void AppendUtf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

} // namespace

struct Reader::Impl {
    RawReader raw;
    mutable std::mutex mutex;
    std::vector<ZSTD_DDict*> dicts;
    bool ready{};

    ~Impl() {
        for (auto* d : dicts)
            ZSTD_freeDDict(d);
    }
    void LoadDictionaries() {
        auto pack = raw("Pack/ZsDic.pack.zs");
        if (!pack || !IsZstd(*pack))
            return;
        auto sarc = Decompress(*pack, nullptr);
        if (!sarc)
            return;
        for (const auto& name : WonderAssets::SarcNames(*sarc)) {
            if (!name.ends_with(".zsdic"))
                continue;
            auto bytes = WonderAssets::SarcMember(*sarc, name);
            if (!bytes || bytes->empty())
                continue;
            if (auto* d = ZSTD_createDDict(bytes->data(), bytes->size()))
                dicts.push_back(d);
        }
        ready = !dicts.empty();
    }
    const ZSTD_DDict* DictFor(const std::vector<std::uint8_t>& frame) const {
        const unsigned id = ZSTD_getDictID_fromFrame(frame.data(), frame.size());
        if (id == 0)
            return nullptr;
        for (const auto* d : dicts)
            if (ZSTD_getDictID_fromDDict(d) == id)
                return d;
        return nullptr;
    }
};

Reader::Reader(RawReader raw) : impl{std::make_unique<Impl>()} {
    impl->raw = std::move(raw);
    impl->LoadDictionaries();
}
Reader::~Reader() = default;

bool Reader::Ready() const {
    return impl->ready;
}

std::optional<std::vector<std::uint8_t>> Reader::Read(std::string_view path) const {
    const std::string p{Trim(path)};
    auto bytes = impl->raw(p);
    if (!bytes || !p.ends_with(".zs") || !IsZstd(*bytes))
        return bytes;
    const ZSTD_DDict* dict = impl->DictFor(*bytes);
    if (!dict && ZSTD_getDictID_fromFrame(bytes->data(), bytes->size()) != 0)
        return std::nullopt; // a dictionary this romfs does not have
    return Decompress(*bytes, dict);
}

WonderAssets::RomfsReader Reader::AsWonderReader() const {
    return [this](const std::string& path) { return Read(path); };
}

std::unordered_map<std::string, std::string> ParseMsbt(const std::vector<std::uint8_t>& b) {
    std::unordered_map<std::string, std::string> out;
    if (b.size() < 0x20 || std::memcmp(b.data(), "MsgStdBn", 8) != 0 || b[8] != 0xFF || b[9] != 0xFE ||
        b[0x0C] != 1) // little-endian UTF-16 only (what TotK ships)
        return out;
    const std::size_t sections = Le16(&b[0x0E]);
    std::vector<std::pair<std::string, std::uint32_t>> labels;
    const std::uint8_t* txt = nullptr;
    std::size_t txt_size = 0;
    std::size_t at = 0x20;
    for (std::size_t s = 0; s < sections && at + 16 <= b.size(); ++s) {
        const std::uint32_t size = Le32(&b[at + 4]);
        const std::size_t data = at + 16;
        if (size > b.size() - data)
            break;
        const std::uint8_t* d = &b[data];
        if (std::memcmp(&b[at], "LBL1", 4) == 0 && size >= 4) {
            const std::uint32_t groups = Le32(d);
            for (std::uint32_t g = 0; g < groups && 4 + 8ull * (g + 1) <= size; ++g) {
                std::uint32_t count = Le32(d + 4 + 8 * g), off = Le32(d + 8 + 8 * g);
                for (std::uint32_t i = 0; i < count && off < size; ++i) {
                    const std::uint8_t len = d[off];
                    if (off + 1ull + len + 4 > size)
                        break;
                    labels.emplace_back(std::string(reinterpret_cast<const char*>(d + off + 1), len),
                                        Le32(d + off + 1 + len));
                    off += 1u + len + 4u;
                }
            }
        } else if (std::memcmp(&b[at], "TXT2", 4) == 0) {
            txt = d;
            txt_size = size;
        }
        at = (data + size + 15) & ~std::size_t{15};
    }
    if (!txt || txt_size < 4)
        return out;
    const std::uint32_t count = Le32(txt);
    if (4ull + 4ull * count > txt_size)
        return out;
    for (const auto& [label, index] : labels) {
        if (index >= count)
            continue;
        std::size_t p = Le32(txt + 4 + 4 * index);
        const std::size_t end = index + 1 < count ? Le32(txt + 8 + 4 * index) : txt_size;
        std::string text;
        while (p + 2 <= end && end <= txt_size) {
            std::uint32_t c = Le16(txt + p);
            p += 2;
            if (c == 0)
                break;
            if (c == 0x0E) { // tag: group, type, parameter size, parameters
                if (p + 6 > end)
                    break;
                p += 6 + Le16(txt + p + 4);
                continue;
            }
            if (c == 0x0F) { // closing tag: group, type
                p += 4;
                continue;
            }
            if (c >= 0xD800 && c < 0xDC00 && p + 2 <= end) {
                const std::uint32_t lo = Le16(txt + p);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    p += 2;
                }
            }
            AppendUtf8(text, c);
        }
        out.emplace(label, std::move(text));
    }
    return out;
}

} // namespace TotkRomfs
