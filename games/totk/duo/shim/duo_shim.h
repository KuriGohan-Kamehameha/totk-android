// SPDX-License-Identifier: GPL-3.0-or-later
// The emulator, as the vendored Eden Duo runtime sees it in the native port.
//
// The runtime (vendor/emu/src/core/mods) was written against Eden's Core::System: guest memory through a page table,
// a kernel with processes and threads, a virtual filesystem, a timing thread, a virtual gamepad. The port has none of
// that; the game runs natively in this process. Every header the runtime includes from the emulator is a one-line
// forwarder to this file, which declares just the names it uses, with the port's meaning:
//   - guest memory is this process's own memory, read through the port's fault-free reader (duo_port.h);
//   - there is one "process", the game, which never changes; guest-thread suspension and the BRK guest bridge do not
//     exist (the runtime already treats that as its NCE configuration);
//   - files are a real directory (the package) or the port's RomFS reader;
//   - input injection (virtual gamepad, pad gate) is accepted and dropped: the BotW package presses no buttons.
// Nothing here is Eden code. What can stay Eden's own (common_types.h, swap.h, vfs_types.h, ...) is vendored instead.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/swap.h"
#include "core/file_sys/vfs/vfs_types.h"

// ---- logging: LOG_*(Class, "fmt {}", args) with std::format, into the port's log --------------------------------
// Eden formats enums as their underlying integer in log lines.
template <typename T>
struct std::formatter<T, std::enable_if_t<std::is_enum_v<T>, char>> : formatter<std::underlying_type_t<T>> {
    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const -> decltype(ctx.out()) {
        return std::formatter<std::underlying_type_t<T>>::format(static_cast<std::underlying_type_t<T>>(value), ctx);
    }
};

namespace Common::Log {
enum class Level : u8 { Trace, Debug, Info, Warning, Error, Critical, Count };
/// One line to the port's log sink (duo_port.h `log`); Debug and below are dropped unless DUO_DEBUG is set.
void Write(Level level, std::string_view text);
extern std::atomic<bool> verbose;
template <typename... Args>
void Fmt(Level level, std::format_string<Args...> format, const Args&... args) {
    if (level < Level::Info && !verbose.load(std::memory_order_relaxed)) {
        return;
    }
    Write(level, std::vformat(format.get(), std::make_format_args(args...)));
}
} // namespace Common::Log

#define LOG_TRACE(log_class, ...) (void(0))
#define LOG_DEBUG(log_class, ...) ::Common::Log::Fmt(::Common::Log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(log_class, ...) ::Common::Log::Fmt(::Common::Log::Level::Info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) ::Common::Log::Fmt(::Common::Log::Level::Warning, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) ::Common::Log::Fmt(::Common::Log::Level::Error, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) ::Common::Log::Fmt(::Common::Log::Level::Critical, __VA_ARGS__)
#define ASSERT(cond) (void(0))
#define ASSERT_MSG(cond, ...) (void(0))
#define UNREACHABLE() __builtin_unreachable()

// ---- Common ----------------------------------------------------------------------------------------------------
namespace Common {

/// Upper- or lower-case hex of a byte array (Eden's common/hex_util.h).
template <typename Container>
[[nodiscard]] std::string HexToString(const Container& data, bool upper = true) {
    static constexpr char lo[] = "0123456789abcdef", up[] = "0123456789ABCDEF";
    std::string out;
    for (const u8 c : data) {
        out.push_back((upper ? up : lo)[c >> 4]);
        out.push_back((upper ? up : lo)[c & 15]);
    }
    return out;
}

enum class ThreadPriority : u32 { Low = 0, Normal = 1, High = 2, VeryHigh = 3, Critical = 4 };
void SetCurrentThreadName(const char* name);
void SetCurrentThreadPriority(ThreadPriority priority);

/// Runs queued jobs on the calling thread. The only user is the ASTC decoder, which queues row stripes and waits;
/// the port decodes few ASTC textures and has no reason to keep a pool for them.
class ThreadWorker {
public:
    template <typename F>
    void QueueWork(F&& work) {
        std::forward<F>(work)();
    }
    void WaitForRequests() {}
};

/// The title module is linked into this library (port_module.cpp), so there is never a library to load.
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    [[nodiscard]] bool IsOpen() const {
        return false;
    }
    [[nodiscard]] bool Open(const char*) {
        return false;
    }
    void Close() {}
    template <typename T>
    [[nodiscard]] bool GetSymbol(const char*, T* out) const {
        *out = nullptr;
        return false;
    }
};

struct UUID {
    std::array<u8, 16> uuid{};
    [[nodiscard]] u128 AsU128() const {
        u128 out{};
        std::memcpy(out.data(), uuid.data(), 16);
        return out;
    }
    [[nodiscard]] std::string FormattedString() const {
        return HexToString(uuid, false);
    }
    [[nodiscard]] std::string RawString() const {
        return HexToString(uuid, false);
    }
    [[nodiscard]] bool IsValid() const {
        return uuid != std::array<u8, 16>{};
    }
};

namespace FS {
enum class EdenPath { EdenDir, CacheDir, ConfigDir, LoadDir, LogDir, NANDDir, SDMCDir };
/// Every Eden path is a folder under the port's writable Duo folder (duo_port.h `data_dir`).
[[nodiscard]] const std::filesystem::path& GetEdenPath(EdenPath path);
[[nodiscard]] inline std::string PathToUTF8String(const std::filesystem::path& path) {
    return path.string();
}
[[nodiscard]] inline std::filesystem::path ToU8String(std::string_view text) {
    return std::filesystem::path{text};
}
} // namespace FS

} // namespace Common

// ---- Settings --------------------------------------------------------------------------------------------------
namespace Settings {
enum class Language : u32 {
    Japanese, EnglishAmerican, French, German, Italian, Spanish, Chinese, Korean, Dutch, Portuguese, Russian,
    Taiwanese, EnglishBritish, FrenchCanadian, SpanishLatin, ChineseSimplified, ChineseTraditional,
    PortugueseBrazilian, Polish, Thai,
};
template <typename T>
struct Value {
    T value{};
    [[nodiscard]] const T& GetValue() const {
        return value;
    }
};
struct Values {
    Value<Language> language_index{Language::EnglishAmerican};
    std::map<u64, std::vector<std::string>> disabled_addons;
};
extern Values values;
[[nodiscard]] std::string_view CanonicalizeEnum(Language language);
/// True: the runtime then never arms its BRK guest bridge, exactly as on Eden's NCE backend.
[[nodiscard]] inline bool IsNceEnabled() {
    return true;
}
} // namespace Settings

// ---- FileSys ---------------------------------------------------------------------------------------------------
namespace FileSys {

class VfsFile {
public:
    virtual ~VfsFile() = default;
    virtual std::string GetName() const = 0;
    virtual std::size_t GetSize() const = 0;
    virtual bool Resize(std::size_t new_size) = 0;
    virtual VirtualDir GetContainingDirectory() const = 0;
    virtual bool IsWritable() const = 0;
    virtual bool IsReadable() const = 0;
    virtual std::size_t Read(u8* data, std::size_t length, std::size_t offset = 0) const = 0;
    virtual std::size_t Write(const u8* data, std::size_t length, std::size_t offset = 0) = 0;
    virtual bool Rename(std::string_view name) = 0;
    virtual std::vector<u8> ReadBytes(std::size_t size, std::size_t offset = 0) const {
        std::vector<u8> out(size);
        out.resize(Read(out.data(), size, offset));
        return out;
    }
    virtual std::vector<u8> ReadAllBytes() const {
        return ReadBytes(GetSize());
    }
    template <typename T>
    std::size_t ReadBytes(T* data, std::size_t size, std::size_t offset = 0) const {
        return Read(reinterpret_cast<u8*>(data), size, offset);
    }
    template <typename T>
    std::size_t ReadObject(T* data, std::size_t offset = 0) const {
        return Read(reinterpret_cast<u8*>(data), sizeof(T), offset);
    }
    std::size_t WriteBytes(const std::vector<u8>& data, std::size_t offset = 0) {
        return Write(data.data(), data.size(), offset);
    }
    virtual std::string GetFullPath() const {
        return GetName();
    }
    std::string GetExtension() const {
        return std::filesystem::path{GetName()}.extension().string();
    }
};

class VfsDirectory {
public:
    virtual ~VfsDirectory() = default;
    /// A file by path below this directory ('/'-separated), or null.
    virtual VirtualFile GetFileRelative(std::string_view path) const = 0;
    virtual VirtualDir GetDirectoryRelative(std::string_view path) const = 0;
    virtual std::vector<VirtualFile> GetFiles() const = 0;
    virtual std::vector<VirtualDir> GetSubdirectories() const = 0;
    virtual std::string GetName() const = 0;
    virtual std::string GetFullPath() const {
        return GetName();
    }
    virtual VirtualDir GetParentDirectory() const {
        return nullptr;
    }
    virtual VirtualFile GetFile(std::string_view name) const {
        return GetFileRelative(name);
    }
    virtual VirtualDir GetSubdirectory(std::string_view name) const {
        return GetDirectoryRelative(name);
    }
    /// Writable directories only (the persisted-flags folder); read-only ones refuse.
    virtual VirtualFile CreateFile(std::string_view) {
        return nullptr;
    }
    virtual VirtualDir CreateSubdirectory(std::string_view) {
        return nullptr;
    }
    virtual bool DeleteFile(std::string_view) {
        return false;
    }
    virtual bool IsWritable() const {
        return false;
    }
    virtual bool IsReadable() const {
        return true;
    }
};

/// A file held in memory (decoded archive members).
class VectorVfsFile final : public VfsFile {
public:
    explicit VectorVfsFile(std::vector<u8> data_ = {}, std::string name_ = {}, VirtualDir = nullptr)
        : data{std::move(data_)}, name{std::move(name_)} {}
    std::string GetName() const override {
        return name;
    }
    std::size_t GetSize() const override {
        return data.size();
    }
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
    std::size_t Read(u8* out, std::size_t length, std::size_t offset) const override {
        if (offset >= data.size()) {
            return 0;
        }
        const std::size_t n = std::min(length, data.size() - offset);
        std::memcpy(out, data.data() + offset, n);
        return n;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

private:
    std::vector<u8> data;
    std::string name;
};

/// A real folder, read-only unless `writable`. Names are matched exactly; ".." and absolute paths are refused.
[[nodiscard]] VirtualDir OpenRealDirectory(const std::filesystem::path& path, bool writable = false);
/// The game's RomFS ("romfs:") and its add-on content ("aoc:") through the port's readers; null when the port has
/// not mounted them.
[[nodiscard]] VirtualDir OpenPortRomFS();
[[nodiscard]] VirtualDir OpenPortAocRomFS();

enum class ContentRecordType : u8 { Program, Data };
enum class SaveDataSpaceId : u8 { User = 1 };
enum class SaveDataType : u8 { Account = 1 };
inline constexpr u64 InvalidSystemSaveDataId = 0;
struct SaveDataAttribute {
    static SaveDataAttribute Make(u64, SaveDataType, u128, u64) {
        return {};
    }
};
/// The application control data: only the supported-language mask is asked for (0 = "any").
struct NACP {
    [[nodiscard]] u32 GetSupportedLanguages() const {
        return 0;
    }
};
} // namespace FileSys

namespace Core {
class System;
}

namespace FileSys {
struct PatchManager {
    static std::pair<std::unique_ptr<NACP>, VirtualFile> GetMetadataFromBaseOrUpdate(Core::System&, u64) {
        return {};
    }
};
} // namespace FileSys

/// A service result code: 0 is success.
struct Result {
    u32 raw{};
    [[nodiscard]] constexpr bool IsSuccess() const {
        return raw == 0;
    }
    [[nodiscard]] constexpr bool IsError() const {
        return raw != 0;
    }
};
[[nodiscard]] constexpr bool R_SUCCEEDED(Result result) {
    return result.IsSuccess();
}
[[nodiscard]] constexpr bool R_FAILED(Result result) {
    return result.IsError();
}

// ---- Service ---------------------------------------------------------------------------------------------------
namespace Service::Set {
enum class LanguageCode : u64 {};
/// Indexed by Settings::Language; only the size and the element are passed on to Service::NS below.
inline constexpr std::array<LanguageCode, 20> available_language_codes{
    LanguageCode{0},  LanguageCode{1},  LanguageCode{2},  LanguageCode{3},  LanguageCode{4},
    LanguageCode{5},  LanguageCode{6},  LanguageCode{7},  LanguageCode{8},  LanguageCode{9},
    LanguageCode{10}, LanguageCode{11}, LanguageCode{12}, LanguageCode{13}, LanguageCode{14},
    LanguageCode{15}, LanguageCode{16}, LanguageCode{17}, LanguageCode{18}, LanguageCode{19}};
} // namespace Service::Set

namespace Service::NS {
enum class ApplicationLanguage : u8 {
    AmericanEnglish = 0, BritishEnglish, Japanese, French, German, LatinAmericanSpanish, Spanish, Italian, Dutch,
    CanadianFrench, Portuguese, Russian, Korean, TraditionalChinese, SimplifiedChinese, BrazilianPortuguese, Polish,
    Thai, Count,
};
/// The settings language as the application language the game is given (no priority fallback chain: the port's
/// game runs in the one language its setup installed).
[[nodiscard]] std::optional<ApplicationLanguage> ConvertToApplicationLanguage(Set::LanguageCode code);
[[nodiscard]] const std::array<ApplicationLanguage, 1>* GetApplicationLanguagePriorityList(ApplicationLanguage lang);
[[nodiscard]] inline u32 GetSupportedLanguageFlag(ApplicationLanguage lang) {
    return 1u << static_cast<u32>(lang);
}
} // namespace Service::NS

namespace Service::FileSystem {
/// Save data is never opened in the port (the BotW module exports no save extension).
struct SaveDataController {
    Result OpenSaveData(FileSys::VirtualDir* out, FileSys::SaveDataSpaceId, const FileSys::SaveDataAttribute&) {
        *out = nullptr;
        return Result{1};
    }
};
struct FileSystemController {
    /// Where companion packages are looked for: <root>/<package>/dualscreen/manifest.json.
    [[nodiscard]] FileSys::VirtualDir GetModificationLoadRoot(u64 title_id) const;
    [[nodiscard]] std::shared_ptr<SaveDataController> OpenSaveDataController() const {
        return nullptr;
    }
};
} // namespace Service::FileSystem

namespace Service::Account {
struct ProfileManager {
    [[nodiscard]] Common::UUID GetLastOpenedUser() const {
        return {};
    }
};
} // namespace Service::Account

// ---- Loader ----------------------------------------------------------------------------------------------------
namespace Loader {
enum class ResultStatus : u16 { Success, ErrorNotImplemented };
struct AppLoader {
    /// The title's icon (the idle page): the port has none to give.
    ResultStatus ReadIcon(std::vector<u8>&) {
        return ResultStatus::ErrorNotImplemented;
    }
};
} // namespace Loader

// ---- Kernel ----------------------------------------------------------------------------------------------------
namespace Core::Memory {
/// The game's memory is this process's memory. Reads go through the port's reader, which reports an unmapped
/// address instead of faulting; a failed read gives zeroes, as Eden's unmapped reads do.
class Memory {
public:
    bool ReadBlock(VAddr address, void* out, std::size_t size);
    bool WriteBlock(VAddr address, const void* in, std::size_t size);
    [[nodiscard]] bool IsValidVirtualAddressRange(VAddr address, u64 size) const;
    [[nodiscard]] bool IsValidVirtualAddress(VAddr address) const {
        return IsValidVirtualAddressRange(address, 1);
    }
    /// A direct pointer when one byte at `address` is readable. As in Eden the caller may read further than it
    /// checked; the runtime's own callers bound themselves with IsValidVirtualAddressRange first.
    [[nodiscard]] u8* GetPointerSilent(VAddr address);
    [[nodiscard]] u8* GetPointer(VAddr address) {
        return GetPointerSilent(address);
    }
    template <typename T>
    [[nodiscard]] T* GetPointer(VAddr address) {
        return reinterpret_cast<T*>(GetPointerSilent(address));
    }
    [[nodiscard]] u8 Read8(VAddr address) {
        return ReadValue<u8>(address);
    }
    [[nodiscard]] u16 Read16(VAddr address) {
        return ReadValue<u16>(address);
    }
    [[nodiscard]] u32 Read32(VAddr address) {
        return ReadValue<u32>(address);
    }
    [[nodiscard]] u64 Read64(VAddr address) {
        return ReadValue<u64>(address);
    }
    /// Guest writes are for the load plan's mailbox (milestone 3); until then the runtime has nothing to write.
    void Write8(VAddr address, u8 value) {
        WriteBlock(address, &value, sizeof value);
    }
    void Write16(VAddr address, u16 value) {
        WriteBlock(address, &value, sizeof value);
    }
    void Write32(VAddr address, u32 value) {
        WriteBlock(address, &value, sizeof value);
    }
    void Write64(VAddr address, u64 value) {
        WriteBlock(address, &value, sizeof value);
    }
    [[nodiscard]] std::string ReadCString(VAddr address, std::size_t max_length);

private:
    template <typename T>
    T ReadValue(VAddr address) {
        T value{};
        ReadBlock(address, &value, sizeof value);
        return value;
    }
};
} // namespace Core::Memory

namespace Kernel {

namespace Svc {
/// Saved registers of a borrowed guest thread (the BRK guest bridge, which the port never arms).
struct ThreadContext {
    std::array<u64, 29> r{};
    u64 fp{}, lr{}, sp{}, pc{};
    u32 pstate{};
    u32 padding{};
    std::array<u128, 32> v{};
    u32 fpcr{}, fpsr{};
    u64 tpidr{};
};
enum class ThreadState : u32 { Initialized, Waiting, Runnable, Terminated };
} // namespace Svc

class KernelCore;

/// A typed guest address, so that GetInteger is found the way Eden's is (argument-dependent lookup).
struct KProcessAddress {
    u64 value{};
};
[[nodiscard]] constexpr u64 GetInteger(KProcessAddress address) {
    return address.value;
}

/// Heap extents feed the dev tools' heap scans only; the port reports none.
struct KPageTable {
    [[nodiscard]] KProcessAddress GetHeapRegionStart() const {
        return {};
    }
    [[nodiscard]] u64 GetHeapRegionSize() const {
        return 0;
    }
    [[nodiscard]] u64 GetNormalMemorySize() const {
        return 0;
    }
};
class KProcess;

class KThread {
public:
    [[nodiscard]] KProcess* GetOwnerProcess() const {
        return nullptr;
    }
    [[nodiscard]] Svc::ThreadContext& GetContext() {
        return context;
    }
    [[nodiscard]] const Svc::ThreadContext& GetContext() const {
        return context;
    }
    [[nodiscard]] u64 GetThreadId() const {
        return 0;
    }
    [[nodiscard]] s32 GetActiveCore() const {
        return 0;
    }
    void Open() {}
    void Close() {}

private:
    Svc::ThreadContext context{};
};

class KProcess {
public:
    [[nodiscard]] Core::Memory::Memory& GetMemory() {
        return memory;
    }
    [[nodiscard]] KPageTable& GetPageTable() {
        return page_table;
    }
    [[nodiscard]] u64 GetProcessId() const {
        return 1;
    }
    [[nodiscard]] std::vector<KThread>& GetThreadList() {
        return threads;
    }
    [[nodiscard]] bool Is64Bit() const {
        return true;
    }
    void Open() {}
    void Close() {}

private:
    Core::Memory::Memory memory;
    KPageTable page_table;
    std::vector<KThread> threads;
};

/// Eden's reference-counting holder. The port's one process lives as long as the library, so this only carries
/// the pointer.
template <typename T>
class KScopedAutoObject {
public:
    KScopedAutoObject() = default;
    KScopedAutoObject(T* object_) : object{object_} {}
    explicit KScopedAutoObject(KernelCore&) {}
    [[nodiscard]] T* GetPointerUnsafe() const {
        return object;
    }
    [[nodiscard]] bool IsNull() const {
        return object == nullptr;
    }
    [[nodiscard]] bool IsNotNull() const {
        return object != nullptr;
    }
    T* operator->() const {
        return object;
    }
    T& operator*() const {
        return *object;
    }
    void Reset(T* object_ = nullptr) {
        object = object_;
    }
    void SetObject(T* object_) {
        object = object_;
    }

private:
    T* object{};
};

class KernelCore {
public:
    /// The game process and the "epoch" that identifies it; neither ever changes in the port.
    [[nodiscard]] KScopedAutoObject<KProcess> RetainApplicationProcess(u64* epoch) {
        *epoch = 1;
        return KScopedAutoObject<KProcess>{&process};
    }
    [[nodiscard]] bool IsApplicationProcess(const KProcess* candidate, u64 epoch) const {
        return candidate == &process && epoch == 1;
    }
    [[nodiscard]] KProcess* ApplicationProcess() {
        return &process;
    }

private:
    KProcess process;
};

} // namespace Kernel

// ---- HID / input -----------------------------------------------------------------------------------------------
namespace Core::HID {
enum class NpadIdType : u32 { Player1 = 0, Handheld = 0x20 };
enum class NpadButton : u64 {
    None = 0, A = 1ull << 0, B = 1ull << 1, X = 1ull << 2, Y = 1ull << 3, StickL = 1ull << 4, StickR = 1ull << 5,
    L = 1ull << 6, R = 1ull << 7, ZL = 1ull << 8, ZR = 1ull << 9, Plus = 1ull << 10, Minus = 1ull << 11,
    Left = 1ull << 12, Up = 1ull << 13, Right = 1ull << 14, Down = 1ull << 15,
    StickLLeft = 1ull << 16, StickLUp = 1ull << 17, StickLRight = 1ull << 18, StickLDown = 1ull << 19,
    StickRLeft = 1ull << 20, StickRUp = 1ull << 21, StickRRight = 1ull << 22, StickRDown = 1ull << 23,
    All = 0xFFFFFFFFFFFFFFFFull,
};
struct NpadButtonState {
    NpadButton raw{};
};
struct AnalogStickState {
    s32 x{}, y{};
};
struct AnalogSticks {
    AnalogStickState left{}, right{};
};
/// The game's controller as the emulator would report it. The port does not expose its pad here yet, so the
/// runtime's controller-focus mode ("nav") sees no input.
class EmulatedController {
public:
    [[nodiscard]] NpadButtonState GetNpadButtons() const {
        return {};
    }
    [[nodiscard]] AnalogSticks GetSticks() const {
        return {};
    }
    [[nodiscard]] bool IsConnected(bool = false) const {
        return false;
    }
};
class HIDCore {
public:
    [[nodiscard]] EmulatedController* GetEmulatedController(NpadIdType) {
        return &controller;
    }

private:
    EmulatedController controller;
};

/// Eden's gate that hides the real pad from the game while the companion owns it. Recorded, never applied.
namespace DSModPadGate {
inline void BlockForContextChange(bool) {}
inline void Set(bool, u64 = 0, bool = false, bool = false) {}
inline void Latch(std::size_t, u64) {}
inline void Reset() {}
} // namespace DSModPadGate
} // namespace Core::HID

namespace Service::HID {
[[nodiscard]] constexpr std::size_t NpadIdTypeToIndex(Core::HID::NpadIdType id) {
    return id == Core::HID::NpadIdType::Handheld ? 8 : 0;
}
} // namespace Service::HID

namespace InputCommon {
/// Synthetic button presses into the game. Dropped: no action in the BotW package presses a button (they are all
/// module, page, view_reset or flag steps).
class VirtualGamepad {
public:
    enum class VirtualButton {
        ButtonA, ButtonB, ButtonX, ButtonY, StickL, StickR, TriggerL, TriggerR, TriggerZL, TriggerZR, ButtonPlus,
        ButtonMinus, ButtonLeft, ButtonUp, ButtonRight, ButtonDown, ButtonSL, ButtonSR, ButtonHome, ButtonCapture,
    };
    enum class VirtualStick { Left = 0, Right = 1 };
    void SetButtonState(std::size_t, int, bool) {}
    void SetButtonState(std::size_t, VirtualButton, bool) {}
    void SetStickPosition(std::size_t, int, float, float) {}
    void SetStickPosition(std::size_t, VirtualStick, float, float) {}
    void ResetControllers() {}
};
class InputSubsystem {
public:
    [[nodiscard]] VirtualGamepad* GetVirtualGamepad() {
        return &pad;
    }

private:
    VirtualGamepad pad;
};
} // namespace InputCommon

// ---- timing, GPU, System ---------------------------------------------------------------------------------------
namespace Core::Timing {
using TimedCallback = std::function<std::optional<std::chrono::nanoseconds>(s64, std::chrono::nanoseconds)>;
struct EventType {
    TimedCallback callback;
    std::string name;
};
[[nodiscard]] std::shared_ptr<EventType> CreateEvent(std::string name, TimedCallback&& callback);
enum class UnscheduleEventType { Wait, NoWait };
/// The runtime schedules exactly one looping event, its 60 Hz Tick. The port runs it on its own "duo-tick" thread,
/// woken once per frame the game presents (duo_host.cpp), so "scheduling" only records the callback.
class CoreTiming {
public:
    void ScheduleLoopingEvent(std::chrono::nanoseconds start, std::chrono::nanoseconds period,
                              const std::shared_ptr<EventType>& event);
    void UnscheduleEvent(const std::shared_ptr<EventType>& event,
                         UnscheduleEventType type = UnscheduleEventType::Wait);
    [[nodiscard]] std::chrono::nanoseconds GetGlobalTimeNs() const;
    [[nodiscard]] std::chrono::microseconds GetGlobalTimeUs() const;
    /// The event to run, or null (taken by the tick thread each frame).
    [[nodiscard]] std::shared_ptr<EventType> Looping() const;

private:
    mutable std::mutex mutex;
    std::shared_ptr<EventType> looping;
};
} // namespace Core::Timing

namespace VideoCore::DSMod {
struct AuxRouting;
struct ReadbackRequest;
struct ReadbackResult;
} // namespace VideoCore::DSMod

namespace Tegra {
class GPU {
public:
    GPU();
    ~GPU();
    /// The hand-off between the runtime (PublishUi) and whoever shows the page (duo_host.cpp's presenter).
    [[nodiscard]] VideoCore::DSMod::AuxRouting& DSModAux() {
        return *aux;
    }
    [[nodiscard]] const VideoCore::DSMod::AuxRouting& DSModAux() const {
        return *aux;
    }
    /// Reading back the game's own textures needs the emulator's texture cache.
    [[nodiscard]] bool DSModTextureReadbackAvailable() const {
        return false;
    }
    bool DSModRequestTextureReadback(const VideoCore::DSMod::ReadbackRequest&,
                                     std::function<void(VideoCore::DSMod::ReadbackResult&&)>) {
        return false;
    }

private:
    std::unique_ptr<VideoCore::DSMod::AuxRouting> aux;
};
} // namespace Tegra

namespace Core {

struct PerfStatsTotals {
    u64 game_frames{}, system_frames{};
    std::chrono::microseconds frametime{};
};
struct PerfStats {
    [[nodiscard]] PerfStatsTotals GetTotals() const;
};

/// A guest CPU core, as the BRK guest bridge is handed one. Never constructed in the port.
class ArmInterface {
public:
    void SetContext(const Kernel::Svc::ThreadContext&) {}
    void GetContext(Kernel::Svc::ThreadContext&) const {}
};
/// Instruction-cache invalidation after a guest patch: patches are milestone 3 and are applied by the port's
/// loader, not through here.
inline void InvalidateInstructionCacheRange(const Kernel::KProcess*, u64, u64) {}

class System {
public:
    [[nodiscard]] Timing::CoreTiming& CoreTiming() {
        return core_timing;
    }
    [[nodiscard]] Kernel::KernelCore& Kernel() {
        return kernel;
    }
    [[nodiscard]] Tegra::GPU& GPU() {
        return gpu;
    }
    [[nodiscard]] HID::HIDCore& HIDCore() {
        return hid_core;
    }
    [[nodiscard]] InputCommon::InputSubsystem* GetInputSubsystem() {
        return &input;
    }
    [[nodiscard]] Service::FileSystem::FileSystemController& GetFileSystemController() {
        return fs_controller;
    }
    [[nodiscard]] Service::Account::ProfileManager& GetProfileManager() {
        return profiles;
    }
    [[nodiscard]] Loader::AppLoader& GetAppLoader() {
        return app_loader;
    }
    [[nodiscard]] PerfStats& GetPerfStats() {
        return perf_stats;
    }
    [[nodiscard]] bool IsPoweredOn() const {
        return true;
    }
    /// True: the runtime then never asks for the guest's threads to be suspended around a read.
    [[nodiscard]] bool IsMulticore() const {
        return true;
    }
    template <typename F>
    bool RunWithGuestThreadsSuspended(F&& work) {
        std::forward<F>(work)();
        return true;
    }
    /// The load plan's mailbox (milestone 3). {0, 0}: no plan is installed, so the module's action bridge
    /// reports itself unavailable and the package runs read-only.
    [[nodiscard]] std::pair<u64, u64> GetDualScreenGuestMailbox() const {
        return {0, 0};
    }
    [[nodiscard]] u32 GetDualScreenGuestMailboxEpoch() const {
        return 0;
    }

private:
    Timing::CoreTiming core_timing;
    Kernel::KernelCore kernel;
    Tegra::GPU gpu;
    HID::HIDCore hid_core;
    InputCommon::InputSubsystem input;
    Service::FileSystem::FileSystemController fs_controller;
    Service::Account::ProfileManager profiles;
    Loader::AppLoader app_loader;
    PerfStats perf_stats;
};

} // namespace Core
