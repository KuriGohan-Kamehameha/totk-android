// SPDX-License-Identifier: GPL-3.0-or-later
// What duo_shim.h declares, implemented over the port's service table (duo_port.h). See that header for the
// meaning each emulator name takes in the port.

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>

#include <pthread.h>
#include <sys/resource.h>

#include "common/device_power_state.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "duo_host.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/textures/workers.h"

namespace Duo {
namespace {
const DuoPort* g_port;
std::filesystem::path g_data_dir;
const auto g_started = std::chrono::steady_clock::now();
} // namespace

void SetPort(const DuoPort* port) {
    g_port = port;
    g_data_dir = port != nullptr && port->data_dir != nullptr ? port->data_dir : "";
}

const DuoPort* Port() {
    return g_port;
}

} // namespace Duo

// ---- logging ---------------------------------------------------------------------------------------------------
namespace Common::Log {
std::atomic<bool> verbose{false};

void Write(Level level, std::string_view text) {
    const DuoPort* const port = Duo::Port();
    // Eden Duo's runtime narrates every asset it loads (several hundred lines for this package: one per text table,
    // composite and picture). The game's log keeps the first few of each kind and says once that it stopped;
    // TOTK_DUO_DEBUG=1 keeps them all. The host's own lines ("duo: ...") and errors are never dropped.
    if (!verbose.load(std::memory_order_relaxed) && level < Level::Error && !text.starts_with("duo: ")) {
        static std::mutex mutex;
        static auto* const seen = new std::map<std::string, u32, std::less<>>;
        // The kind of a line: up to its first quote or digit ("DSMod composite '", "DSMod: loaded asset '").
        const std::string_view kind = text.substr(0, std::min<std::size_t>(text.find_first_of("'0123456789"), 28));
        const u32 budget = level == Level::Warning ? 12 : 6;
        std::scoped_lock lock{mutex};
        auto it = seen->find(kind);
        if (it == seen->end()) {
            it = seen->emplace(std::string{kind}, 0).first;
        }
        if (++it->second > budget) {
            if (it->second == budget + 1 && port != nullptr && port->log != nullptr) {
                const std::string note =
                    std::format("duo: further '{}...' lines are not logged (TOTK_DUO_DEBUG=1 logs all)", kind);
                port->log(static_cast<int>(Level::Info), note.c_str());
            }
            return;
        }
    }
    const std::string line{text};
    if (port != nullptr && port->log != nullptr) {
        port->log(static_cast<int>(level), line.c_str());
    } else {
        std::fprintf(stderr, "[duo] %s\n", line.c_str());
    }
}
} // namespace Common::Log

namespace Common {

void SetCurrentThreadName(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name); // truncated by the kernel to 15 characters
#endif
}

void SetCurrentThreadPriority(ThreadPriority priority) {
#if defined(__linux__)
    // A Linux thread has its own nice value. The companion's workers stay below the game's threads.
    if (priority == ThreadPriority::Low) {
        setpriority(PRIO_PROCESS, 0, 10);
    }
#else
    (void)priority;
#endif
}

/// Battery state for the page's @device.* values. The port has no reader for it yet: "no battery" hides them.
PowerStatus GetPowerStatus() {
    return PowerStatus{.percentage = -1, .charging = false, .has_battery = false};
}

namespace FS {
const std::filesystem::path& GetEdenPath(EdenPath path) {
    // One folder each, made on first use. Never destroyed: asked for from worker threads at shutdown.
    static std::mutex mutex;
    static auto* const paths = new std::map<EdenPath, std::filesystem::path>;
    std::scoped_lock lock{mutex};
    auto it = paths->find(path);
    if (it == paths->end()) {
        const char* const leaf = path == EdenPath::EdenDir     ? ""
                                 : path == EdenPath::CacheDir  ? "cache"
                                 : path == EdenPath::ConfigDir ? "config"
                                 : path == EdenPath::LoadDir   ? "load"
                                 : path == EdenPath::LogDir    ? "log"
                                 : path == EdenPath::NANDDir   ? "nand"
                                                               : "sdmc";
        std::filesystem::path full = *leaf ? Duo::g_data_dir / leaf : Duo::g_data_dir;
        std::error_code ignored;
        std::filesystem::create_directories(full, ignored);
        it = paths->emplace(path, std::move(full)).first;
    }
    return it->second;
}
} // namespace FS

} // namespace Common

namespace Tegra::Texture {
Common::ThreadWorker& GetThreadWorkers() {
    static Common::ThreadWorker workers;
    return workers;
}
} // namespace Tegra::Texture

// ---- settings and language -------------------------------------------------------------------------------------
namespace Settings {
Values values;

std::string_view CanonicalizeEnum(Language language) {
    static constexpr std::array<std::string_view, 20> names{
        "ja", "en_US", "fr", "de", "it", "es", "zh_CN", "ko", "nl", "pt", "ru", "zh_TW", "en_GB", "fr_CA", "es_419",
        "zh_Hans", "zh_Hant", "pt_BR", "pl", "th"};
    const auto index = static_cast<std::size_t>(language);
    return index < names.size() ? names[index] : std::string_view{};
}
} // namespace Settings

namespace Service::NS {
std::optional<ApplicationLanguage> ConvertToApplicationLanguage(Set::LanguageCode code) {
    using L = ApplicationLanguage;
    // Indexed by Settings::Language (the shim's language codes are those indices).
    static constexpr std::array<L, 20> map{
        L::Japanese, L::AmericanEnglish, L::French, L::German, L::Italian, L::Spanish, L::SimplifiedChinese,
        L::Korean, L::Dutch, L::Portuguese, L::Russian, L::TraditionalChinese, L::BritishEnglish, L::CanadianFrench,
        L::LatinAmericanSpanish, L::SimplifiedChinese, L::TraditionalChinese, L::BrazilianPortuguese, L::Polish,
        L::Thai};
    const auto index = static_cast<std::size_t>(code);
    return index < map.size() ? std::optional{map[index]} : std::nullopt;
}

const std::array<ApplicationLanguage, 1>* GetApplicationLanguagePriorityList(ApplicationLanguage lang) {
    static std::array<std::array<ApplicationLanguage, 1>, static_cast<std::size_t>(ApplicationLanguage::Count)> lists =
        [] {
            std::array<std::array<ApplicationLanguage, 1>, static_cast<std::size_t>(ApplicationLanguage::Count)> out{};
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i][0] = static_cast<ApplicationLanguage>(i);
            }
            return out;
        }();
    const auto index = static_cast<std::size_t>(lang);
    return index < lists.size() ? &lists[index] : nullptr;
}
} // namespace Service::NS

// ---- guest memory ----------------------------------------------------------------------------------------------
namespace Core::Memory {

bool Memory::ReadBlock(VAddr address, void* out, std::size_t size) {
    if (size == 0) {
        return true;
    }
    const DuoPort* const port = Duo::Port();
    if (port != nullptr && port->safe_read(address, out, size)) {
        return true;
    }
    std::memset(out, 0, size);
    return false;
}

bool Memory::WriteBlock(VAddr, const void*, std::size_t) {
    // The companion is read-only until the load plan's mailbox exists (milestone 3): every guest write the
    // runtime knows (enforce rules, patches, the mailbox) is refused here, in one place.
    return false;
}

namespace {
// Pages already found readable during the current tick, per thread. The module asks "is this mapped?" before every
// read and each answer costs the reader a system call or two, so within one tick a page is probed once. The
// answer can go stale only as it could between the check and the read anyway, and the read itself never faults.
std::atomic<u64> g_tick_epoch{1};
struct PageCache {
    u64 epoch{};
    std::vector<u64> slots; // open addressing, 0 = empty (page 0 is never valid)
    u32 used{};
};
thread_local PageCache t_pages;
constexpr u64 PageSize = 4096;

bool PageReadable(const DuoPort& port, u64 page) {
    PageCache& cache = t_pages;
    const u64 epoch = g_tick_epoch.load(std::memory_order_relaxed);
    if (cache.epoch != epoch || cache.slots.empty() || cache.used * 2 > cache.slots.size()) {
        cache.slots.assign(std::max<std::size_t>(cache.slots.size(), 4096), 0);
        cache.used = 0;
        cache.epoch = epoch;
    }
    const std::size_t mask = cache.slots.size() - 1;
    std::size_t at = static_cast<std::size_t>(page * 0x9E3779B97F4A7C15ull >> 20) & mask;
    for (; cache.slots[at] != 0; at = (at + 1) & mask) {
        if (cache.slots[at] == page) {
            return true;
        }
    }
    u8 probe;
    if (!port.safe_read(page * PageSize, &probe, 1)) {
        return false; // not remembered: a page mapped later in the tick is found then
    }
    cache.slots[at] = page;
    ++cache.used;
    return true;
}
} // namespace

bool Memory::IsValidVirtualAddressRange(VAddr address, u64 size) const {
    const DuoPort* const port = Duo::Port();
    if (port == nullptr || address < PageSize || size == 0 || address + size < address) {
        return false;
    }
    for (u64 page = address / PageSize, last = (address + size - 1) / PageSize; page <= last; ++page) {
        if (!PageReadable(*port, page)) {
            return false;
        }
    }
    return true;
}

} // namespace Core::Memory

void Duo::BeginTick() {
    Core::Memory::g_tick_epoch.fetch_add(1, std::memory_order_relaxed);
}

namespace Core::Memory {

u8* Memory::GetPointerSilent(VAddr address) {
    const DuoPort* const port = Duo::Port();
    u8 probe;
    if (port == nullptr || address == 0 || !port->safe_read(address, &probe, 1)) {
        return nullptr;
    }
    return reinterpret_cast<u8*>(static_cast<uintptr_t>(address));
}

std::string Memory::ReadCString(VAddr address, std::size_t max_length) {
    std::string out;
    const DuoPort* const port = Duo::Port();
    if (port == nullptr) {
        return out;
    }
    // Up to the end of each page at a time, so a string that ends just before an unmapped page still reads.
    while (out.size() < max_length) {
        char chunk[256];
        const u64 at = address + out.size();
        const std::size_t want =
            std::min<std::size_t>({sizeof chunk, max_length - out.size(), 4096 - static_cast<std::size_t>(at & 4095)});
        if (!port->safe_read(at, chunk, want)) {
            break;
        }
        const void* const nul = std::memchr(chunk, 0, want);
        out.append(chunk, nul != nullptr ? static_cast<const char*>(nul) - chunk : want);
        if (nul != nullptr) {
            break;
        }
    }
    return out;
}

} // namespace Core::Memory

// ---- timing, GPU, perf -----------------------------------------------------------------------------------------
namespace Core::Timing {

std::shared_ptr<EventType> CreateEvent(std::string name, TimedCallback&& callback) {
    return std::make_shared<EventType>(std::move(callback), std::move(name));
}

void CoreTiming::ScheduleLoopingEvent(std::chrono::nanoseconds, std::chrono::nanoseconds,
                                      const std::shared_ptr<EventType>& event) {
    std::scoped_lock lock{mutex};
    looping = event;
}

void CoreTiming::UnscheduleEvent(const std::shared_ptr<EventType>& event, UnscheduleEventType) {
    std::scoped_lock lock{mutex};
    if (looping == event) {
        looping.reset();
    }
}

std::shared_ptr<EventType> CoreTiming::Looping() const {
    std::scoped_lock lock{mutex};
    return looping;
}

std::chrono::nanoseconds CoreTiming::GetGlobalTimeNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Duo::g_started);
}

std::chrono::microseconds CoreTiming::GetGlobalTimeUs() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(GetGlobalTimeNs());
}

} // namespace Core::Timing

namespace Tegra {
GPU::GPU() : aux{std::make_unique<VideoCore::DSMod::AuxRouting>()} {}
GPU::~GPU() = default;
} // namespace Tegra

namespace Core {
PerfStatsTotals PerfStats::GetTotals() const {
    const DuoPort* const port = Duo::Port();
    const u64 frames = port != nullptr && port->frames != nullptr ? port->frames() : 0;
    return PerfStatsTotals{.game_frames = frames, .system_frames = frames, .frametime = {}};
}
} // namespace Core

// ---- files -----------------------------------------------------------------------------------------------------
namespace FileSys {
namespace {

bool SafeRelative(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos) {
        return false;
    }
    for (std::size_t at = 0; at <= path.size();) {
        const std::size_t end = std::min(path.find('/', at), path.size());
        const std::string_view part = path.substr(at, end - at);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        at = end + 1;
    }
    return true;
}

class ReadOnlyFile : public VfsFile {
public:
    bool Resize(std::size_t) override {
        return false;
    }
    VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }
};

/// A file of a real folder. Opened for each read: package files are read once and cached decoded by the runtime,
/// so nothing is gained by holding 300 descriptors.
class RealFile final : public ReadOnlyFile {
public:
    RealFile(std::filesystem::path path_, std::size_t size_) : path{std::move(path_)}, size{size_} {}
    std::string GetName() const override {
        return path.filename().string();
    }
    std::string GetFullPath() const override {
        return path.string();
    }
    std::size_t GetSize() const override {
        return size;
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        std::FILE* const file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            return 0;
        }
        std::size_t got = 0;
        if (std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0) {
            got = std::fread(data, 1, length, file);
        }
        std::fclose(file);
        return got;
    }

private:
    std::filesystem::path path;
    std::size_t size;
};

class RealDirectory final : public VfsDirectory {
public:
    explicit RealDirectory(std::filesystem::path path_) : path{std::move(path_)} {}
    VirtualFile GetFileRelative(std::string_view relative) const override {
        if (!SafeRelative(relative)) {
            return nullptr;
        }
        const auto full = path / relative;
        std::error_code error;
        if (!std::filesystem::is_regular_file(full, error)) {
            return nullptr;
        }
        const auto size = std::filesystem::file_size(full, error);
        return error ? nullptr : std::make_shared<RealFile>(full, static_cast<std::size_t>(size));
    }
    VirtualDir GetDirectoryRelative(std::string_view relative) const override {
        if (!SafeRelative(relative)) {
            return nullptr;
        }
        const auto full = path / relative;
        std::error_code error;
        return std::filesystem::is_directory(full, error) ? std::make_shared<RealDirectory>(full) : nullptr;
    }
    std::vector<VirtualFile> GetFiles() const override {
        std::vector<VirtualFile> out;
        std::error_code error;
        for (std::filesystem::directory_iterator it{path, error}, end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) {
                out.push_back(std::make_shared<RealFile>(it->path(), static_cast<std::size_t>(it->file_size(error))));
            }
        }
        return out;
    }
    std::vector<VirtualDir> GetSubdirectories() const override {
        std::vector<VirtualDir> out;
        std::error_code error;
        for (std::filesystem::directory_iterator it{path, error}, end; !error && it != end; it.increment(error)) {
            if (it->is_directory(error)) {
                out.push_back(std::make_shared<RealDirectory>(it->path()));
            }
        }
        return out;
    }
    std::string GetName() const override {
        return path.filename().string();
    }
    std::string GetFullPath() const override {
        return path.string();
    }

private:
    std::filesystem::path path;
};

using PortReader = int64_t (*)(const char* path, uint64_t offset, void* out, uint64_t n);

/// One file of the game's RomFS (or of its add-on content). Holds a path, not a handle: the port's reader is
/// stateless, so nothing here can fill the game's own file-handle table.
class PortFile final : public ReadOnlyFile {
public:
    PortFile(PortReader reader_, std::string path_, std::size_t size_)
        : reader{reader_}, path{std::move(path_)}, size{size_} {}
    std::string GetName() const override {
        const auto slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
    std::string GetFullPath() const override {
        return path;
    }
    std::size_t GetSize() const override {
        return size;
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (offset >= size || length == 0) {
            return 0;
        }
        const int64_t got = reader(path.c_str(), offset, data, std::min(length, size - offset));
        return got > 0 ? static_cast<std::size_t>(got) : 0;
    }

private:
    PortReader reader;
    std::string path;
    std::size_t size;
};

/// The root of a port volume. Files are found by full relative path; the runtime never lists the game's folders
/// (its sources only ever open files, mod_sources.h), so listings are empty.
class PortDirectory final : public VfsDirectory {
public:
    PortDirectory(PortReader reader_, std::string name_) : reader{reader_}, name{std::move(name_)} {}
    VirtualFile GetFileRelative(std::string_view relative) const override {
        if (!SafeRelative(relative)) {
            return nullptr;
        }
        std::string path{relative};
        const int64_t size = reader(path.c_str(), 0, nullptr, 0);
        return size < 0 ? nullptr : std::make_shared<PortFile>(reader, std::move(path), static_cast<std::size_t>(size));
    }
    VirtualDir GetDirectoryRelative(std::string_view) const override {
        return nullptr;
    }
    std::vector<VirtualFile> GetFiles() const override {
        return {};
    }
    std::vector<VirtualDir> GetSubdirectories() const override {
        return {};
    }
    std::string GetName() const override {
        return name;
    }

private:
    PortReader reader;
    std::string name;
};

VirtualDir OpenPort(PortReader reader, const char* name, const char* probe) {
    // A volume the port has not mounted answers -1 for everything; then the source is "unavailable" and, for the
    // game's RomFS, asked again a second later (mod_sources.h retry_ms).
    if (reader == nullptr || (probe != nullptr && reader(probe, 0, nullptr, 0) < 0)) {
        return nullptr;
    }
    return std::make_shared<PortDirectory>(reader, name);
}

} // namespace

VirtualDir OpenRealDirectory(const std::filesystem::path& path, bool) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) ? std::make_shared<RealDirectory>(path) : nullptr;
}

VirtualDir OpenPortRomFS() {
    const DuoPort* const port = Duo::Port();
    // "" asks the reader whether the volume is there at all (duo_port.h: -1 = no such volume).
    return port != nullptr ? OpenPort(port->romfs_read, "romfs", "") : nullptr;
}

VirtualDir OpenPortAocRomFS() {
    const DuoPort* const port = Duo::Port();
    return port != nullptr ? OpenPort(port->aoc_read, "aoc", "") : nullptr;
}

} // namespace FileSys

namespace Service::FileSystem {
FileSys::VirtualDir FileSystemController::GetModificationLoadRoot(u64) const {
    const DuoPort* const port = Duo::Port();
    return port != nullptr && port->package_root != nullptr ? FileSys::OpenRealDirectory(port->package_root)
                                                            : nullptr;
}
} // namespace Service::FileSystem
