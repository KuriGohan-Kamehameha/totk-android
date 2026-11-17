// SPDX-License-Identifier: GPL-3.0-or-later
// The companion host: what the emulator's System does for Eden Duo's runtime, done by the native port.
//
//   duo_host_start (called by libtotk once the game's modules are loaded) copies the port's service table and starts
//   the "duo-tick" thread. Everything else happens there:
//     1. the facts first, each on its own log line: title, build id, where main is and how large, whether main reads;
//     2. the package: <package_root>/<name>/dualscreen/manifest.json (the app unpacks it from the APK and writes
//        <package_root>/.ready last). Found: Eden Duo's ModRuntime is built on it, the real page. Not found, or
//        TOTK_DUO=module: the game's title module (the one this library was built with: Breath of the Wild's or Tears
//        of the Kingdom's, CMakeLists.txt DUO_TITLE) alone on a minimal host, its outputs in the log;
//     3. the tick loop. The game posts one wake per presented frame (duo_host_frame); the loop also wakes 16 ms
//        after its last tick, so the page keeps its 60 Hz clock while the game renders 30 frames a second and
//        keeps animating through a loading stall. Nothing of this runs on a game thread.
//   The page reaches the screen through AuxRouting, as in the emulator: the runtime publishes ARGB pixels, the
//   "duo-present" thread copies each new frame into the second screen's Surface (DuoSurfaceView hands it over by
//   JNI). No GPU is involved; the game's Vulkan device never sees the companion.
//
// Read-only: the shim refuses guest writes and reports no mailbox, so the module's action bridge stays unavailable.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/mod_runtime.h"
#include "duo_host.h"
#include "duo_state.h"
#include "video_core/dsmod/aux_routing.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#endif

#define DUO_EXPORT extern "C" __attribute__((visibility("default")))

namespace Duo {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

DuoPort g_port{};
std::string g_package_root, g_data_dir;
std::atomic<bool> g_started{false};
std::counting_semaphore<> g_frames{0};
// The shim's System, made when the library is loaded so that the surface (JNI, any time) and the game (duo_host_start)
// may arrive in either order. Never destroyed: the process ends with _exit.
Core::System* const g_system = new Core::System;
std::atomic<u64> g_ticks{0};
std::atomic<bool> g_stop{false};
std::thread g_tick_thread;

// The second screen's surface; guarded by g_surface_mutex, which the presenter holds across a post (a post can wait
// for the display), so the tick thread never takes it: it wakes the presenter through g_wake_mutex alone.
std::mutex g_surface_mutex;
std::mutex g_wake_mutex;
std::condition_variable g_present_wake;
u64 g_present_generation = 0; // guarded by g_wake_mutex; bumped on every tick and surface change
#ifdef __ANDROID__
ANativeWindow* g_window;
#endif
std::atomic<u64> g_posted{0};

// ---- the common game state (duo_host.h LedTap, duo_state.h) --------------------------------------------------------
const DuoStateTable* g_led_table = nullptr;                // the running title's names (set by duo_host_start)
std::array<int64_t, DUO_STATE_MAX_VALUES> g_led_pending{}; // tick thread only

bool EnvIs(const char* name, const char* value) {
    const char* const set = std::getenv(name);
    return set != nullptr && std::strcmp(set, value) == 0;
}

template <typename... Args>
void Say(std::format_string<Args...> format, const Args&... args) {
    Common::Log::Write(Common::Log::Level::Info, std::vformat(format.get(), std::make_format_args(args...)));
}

std::string Hex(const u8* bytes, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        out += std::format("{:02X}", bytes[i]);
    }
    return out;
}

/// The facts a wrong build would get wrong, before anything depends on them.
bool ReportGame() {
    Say("duo: host up, title {:016X} build {} main_base={:#x} main_size={:#x}", g_port.title_id,
        Hex(g_port.build_id, 16), g_port.main_base, g_port.main_size);
    std::array<u32, 4> head{};
    if (!g_port.safe_read(g_port.main_base, head.data(), sizeof head)) {
        Say("duo: main is not readable at {:#x}; companion off", g_port.main_base);
        return false;
    }
    Say("duo: main+0 reads {:08x} {:08x} {:08x} {:08x}", head[0], head[1], head[2], head[3]);
    // Per title: the call sites its package's load plan would patch (milestone 3), read only: do they survive the
    // port's rewriter as the original `bl` words? And the size of the original main (text + rodata + data), below
    // which the module's readers refuse the image. Breath of the Wild 1.6.0: main+0x1852248 da00e797,
    // main+0xF4109C b5030194 (bytes as stored), 0x2D66000. Tears of the Kingdom 1.4.3: no load plan, 0x3A25D60.
    struct Site {
        u64 offset;
        u32 word;
    };
    static constexpr Site BotwSites[] = {{0x1852248, 0x97e700da}, {0xF4109C, 0x940103b5}};
    struct Title {
        u64 title_id;
        std::span<const Site> sites;
        u64 original_main;
    };
    static constexpr Title Titles[] = {{0x01007EF00011E000, BotwSites, 0x2D66000}, {0x0100F2C0115B6000, {}, 0x3A25D60}};
    const auto* const title = std::ranges::find(Titles, g_port.title_id, &Title::title_id);
    if (title == std::end(Titles)) {
        Say("duo: title {:016X} is not one this host knows; companion off", g_port.title_id);
        return false;
    }
    for (const auto& site : title->sites) {
        u32 found{};
        const bool read = g_port.safe_read(g_port.main_base + site.offset, &found, sizeof found) != 0;
        Say("duo: load-plan site main+{:#x}: {} (original {:08x}, here {:08x}); not patched in this build",
            site.offset, !read ? "unreadable" : found == site.word ? "original" : "differs", site.word, found);
    }
    if (g_port.main_size < title->original_main) {
        Say("duo: main is {:#x} bytes, smaller than the original {:#x}; the module will read nothing",
            g_port.main_size, title->original_main);
    }
    return true;
}

// ---- milestone 0: the module alone ------------------------------------------------------------------------------
/// The title module on the smallest host it accepts: memory reads, the log, and its published values kept in a
/// table whose changes go to the log. No package, no page, no RomFS (read_romfs answers "missing").
class ModuleOnly {
public:
    bool Start() {
        api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
        const std::string build = Hex(g_port.build_id, 32);
        if (api == nullptr || api->title_id != g_port.title_id || !api->supports_build(build.c_str())) {
            Say("duo: module-only: the built-in module does not accept title {:016X} build {}", g_port.title_id,
                build);
            return false;
        }
        host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
        host.struct_size = sizeof host;
        host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
        host.capabilities = EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN;
        host.userdata = this;
        host.title_id = g_port.title_id;
        std::memcpy(host.build_id, g_port.build_id, sizeof host.build_id);
        host.main_base = g_port.main_base;
        host.main_size = g_port.main_size;
        host.get_tick = [](void* p) { return static_cast<ModuleOnly*>(p)->ticks; };
        host.get_heap_begin = [](void*) -> uint64_t { return 0; };
        host.get_heap_end = [](void*) -> uint64_t { return 0; };
        host.is_mapped = [](void*, uint64_t address, uint64_t size) -> EdenDsmodBool {
            return size <= (64u << 20) && Memory().IsValidVirtualAddressRange(address, size);
        };
        host.read_memory = [](void*, uint64_t address, void* output, size_t size) -> EdenDsmodBool {
            return output != nullptr && size <= (64u << 20) && Memory().IsValidVirtualAddressRange(address, size) &&
                   Memory().ReadBlock(address, output, size);
        };
        host.get_read_pointer = [](void*, uint64_t, size_t) -> const uint8_t* { return nullptr; };
        host.write_memory = [](void*, uint64_t, const void*, size_t) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
        host.log = [](void*, uint32_t, const char* message) { Say("duo: module: {}", message ? message : ""); };
        host.begin_output = [](void*) {};
        host.end_output = [](void*) {};
        host.publish_i64 = [](void* p, const char* name, int64_t value) {
            Self(p).Put(name, Value{value});
            LedTap(name, value);
        };
        host.publish_f64 = [](void* p, const char* name, double value) { Self(p).Put(name, Value{value}); };
        host.publish_text = [](void* p, const char* name, const char* value) {
            Self(p).Put(name, Value{std::string{value ? value : ""}});
        };
        host.publish_address = [](void* p, const char* name, uint64_t value) { Self(p).Put(name, Value{value}); };
        host.publish_map = [](void*, const EdenDsmodMapFrame*) {};
        host.read_romfs = [](void*, const char*, uint64_t, void*, size_t) -> size_t { return 0; };
        host.get_i64 = [](void*, const char*, int64_t fallback) { return fallback; };
        host.get_f64 = [](void*, const char*, double fallback) { return fallback; };
        host.get_text = [](void*, const char*) -> const char* { return nullptr; };
        instance = api->create(&host, "{}");
        if (instance == nullptr) {
            Say("duo: module-only: the module could not create its state");
            return false;
        }
        if (const auto* const ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
            ext != nullptr && ext->configure != nullptr) {
            // No mailbox: the module's action bridge stays unavailable, which is what read-only means.
            host_ext.version = EDEN_DSMOD_EXT_VERSION;
            host_ext.struct_size = sizeof host_ext;
            host_ext.abi_hash = EDEN_DSMOD_EXT_HASH;
            host_ext.userdata = this;
            host_ext.load_u32 = [](void*, uint32_t, uint32_t*) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
            host_ext.load_u64 = [](void*, uint32_t, uint64_t*) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
            host_ext.store_u32 = [](void*, uint32_t, uint32_t) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
            host_ext.store_u64 = [](void*, uint32_t, uint64_t) -> EdenDsmodBool { return EDEN_DSMOD_FALSE; };
            ext->configure(instance, &host_ext);
        }
        Say("duo: module-only: {} created (ABI {}), ticking once per presented frame", api->name ? api->name : "module",
            api->abi_version);
        return true;
    }

    void Tick() {
        ++ticks;
        changed = 0;
        try {
            api->sample(instance, &host); // reads the game and publishes; tick only settles the action bridge
            LedCommit();
            api->tick(instance, &host);
        } catch (...) {
            Say("duo: module-only: the module threw at tick {}", ticks);
        }
        // The first complete set, then a line every ten seconds: enough to see live values move in the log.
        if (ticks == 1 || ticks == 120 || ticks % 600 == 0) {
            Report();
        }
    }

private:
    using Value = std::variant<int64_t, double, uint64_t, std::string>;
    static ModuleOnly& Self(void* p) {
        return *static_cast<ModuleOnly*>(p);
    }
    static Core::Memory::Memory& Memory() {
        return g_system->Kernel().ApplicationProcess()->GetMemory();
    }
    void Put(const char* name, Value value) {
        if (name == nullptr) {
            return;
        }
        auto [it, added] = values.try_emplace(name, value);
        if (added || it->second != value) {
            it->second = std::move(value);
            ++changed;
        }
    }
    static std::string Show(const Value& value) {
        return std::visit(
            [](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return '"' + v.substr(0, 40) + '"';
                } else if constexpr (std::is_same_v<T, uint64_t>) {
                    return std::format("{:#x}", v);
                } else {
                    return std::format("{}", v);
                }
            },
            value);
    }
    void Report() {
        Say("duo: module-only: tick {}: {} values published, {} changed this tick", ticks, values.size(), changed);
        // The values a player can check against the game's own screen, a few per line. The module publishes
        // thousands (every page widget's text); the rest would only bury these.
        // Each title module's names (only the running one's appear).
        static constexpr std::string_view watch[] = {
            "botw.state.", "botw.pouch_valid", "botw.pouch_count", "botw.inventory.count", "botw.quest.count",
            "botw.ready.rupees", "botw.ready.heal.life", "botw.ready.heal.max", "botw.ready.stamina.percent",
            "botw.ready.clock.text", "botw.ready.temperature.text", "botw.ready.area", "botw.ready.weapon",
            "botw.ready.bow", "botw.ready.shield", "botw.ready.arrow.count", "botw.ready.shrine_count",
            "botw.ready.spirit_orbs", "botw.bridge.available",
            "totk.status", "totk.state.", "totk.rupees", "totk.hearts.text", "totk.layer_text", "totk.gear."};
        std::string line;
        for (const auto& [name, value] : values) {
            const bool watched = std::ranges::any_of(watch, [&](std::string_view w) {
                return w.ends_with('.') ? std::string_view{name}.starts_with(w) : name == w;
            });
            if (!watched) {
                continue;
            }
            line += std::format("{}={} ", name, Show(value));
            if (line.size() > 300) {
                Say("duo: module-only:   {}", line);
                line.clear();
            }
        }
        if (!line.empty()) {
            Say("duo: module-only:   {}", line);
        }
    }

    const EdenDsmodModuleApi* api{};
    EdenDsmodHostApi host{};
    EdenDsmodHostExtensions host_ext{};
    void* instance{};
    u64 ticks{};
    size_t changed{};
    std::map<std::string, Value> values;
};

// ---- milestone 1: the runtime -----------------------------------------------------------------------------------
std::optional<Core::Mods::ModRuntime> g_runtime;

/// The app unpacks the package from the APK when it starts and writes ".ready" last; the game needs far longer to
/// reach its first frame, so this normally returns at once. Half a minute covers a first start on a slow device.
bool WaitForPackage() {
    const std::filesystem::path ready = std::filesystem::path{g_package_root} / ".ready";
    for (int tries = 0; tries < 60 && !g_stop.load(); ++tries) {
        std::error_code error;
        if (std::filesystem::exists(ready, error)) {
            return true;
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

bool StartRuntime() {
    if (g_package_root.empty() || ((g_port.flags & DUO_PORT_WAIT_PACKAGE) != 0 && !WaitForPackage())) {
        Say("duo: no companion package under '{}'", g_package_root);
        return false;
    }
    try {
        std::array<u8, 0x20> build{};
        std::memcpy(build.data(), g_port.build_id, build.size());
        auto manifest = Core::Mods::ModRuntime::Discover(*g_system, g_port.title_id, build);
        if (!manifest) {
            Say("duo: no usable companion package for title {:016X} under '{}'", g_port.title_id, g_package_root);
            return false;
        }
        Say("duo: package '{}' ({}), {} pages, canvas {}x{}", manifest->name, manifest->mod_dir_name,
            manifest->pages.size(), manifest->canvas_w, manifest->canvas_h);
        g_runtime.emplace(*g_system, std::move(*manifest));
        g_runtime->SetMainMemoryParameters(g_port.main_base, g_port.main_size);
        g_runtime->Initialize();
        return true;
    } catch (const std::exception& error) {
        Say("duo: the companion runtime failed to start: {}", error.what());
    } catch (...) {
        Say("duo: the companion runtime failed to start");
    }
    g_runtime.reset();
    return false;
}

void WakePresenter() {
    {
        std::scoped_lock lock{g_wake_mutex};
        ++g_present_generation;
    }
    g_present_wake.notify_one();
}

/// Name the calling thread and let the port place it (duo_port.h place_thread) before it starts anything.
void EnterThread(const char* name) {
    Common::SetCurrentThreadName(name);
    if (g_port.place_thread != nullptr) {
        g_port.place_thread(name);
    }
}

void TickLoop() {
    EnterThread("duo-tick");
    if (!ReportGame()) {
        return;
    }
    const bool module_only_wanted = EnvIs("TOTK_DUO", "module");
    const bool runtime_up = !module_only_wanted && StartRuntime();
    ModuleOnly module_only;
    const bool module_up = !runtime_up && module_only.Start();
    if (!runtime_up && !module_up) {
        Say("duo: nothing to run; companion off");
        return;
    }
    Say("duo: mode {}", runtime_up ? "runtime (the package's page on the second screen)" : "module-only (log)");

    constexpr auto Period = std::chrono::nanoseconds{1'000'000'000 / 60};
    auto last_tick = Clock::now() - Period;
    auto last_frame = Clock::now();
    double spent_ms = 0, worst_ms = 0;
    u64 measured = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        // One wake per presented frame, or the 60 Hz clock, whichever comes first. With no frame for two seconds
        // (the game is paused or in the background) the clock drops to 4 Hz.
        const bool idle = Clock::now() - last_frame > 2s;
        const auto deadline = last_tick + (idle ? std::chrono::nanoseconds{250ms} : Period);
        if (g_frames.try_acquire_until(deadline)) {
            while (g_frames.try_acquire()) {
            }
            last_frame = Clock::now();
            if (last_frame - last_tick < Period / 2) {
                continue; // a frame right after a clock tick: the next clock tick carries it
            }
        }
        const auto begin = Clock::now();
        last_tick = begin;
        Duo::BeginTick();
        try {
            if (runtime_up) {
                if (const auto event = g_system->CoreTiming().Looping()) {
                    event->callback(0, std::chrono::nanoseconds{0});
                }
            } else {
                module_only.Tick();
            }
        } catch (const std::exception& error) {
            Say("duo: tick {} threw: {}; companion stopped", g_ticks.load(), error.what());
            return;
        } catch (...) {
            Say("duo: tick {} threw; companion stopped", g_ticks.load());
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        spent_ms += ms;
        worst_ms = std::max(worst_ms, ms);
        ++measured;
        const u64 ticks = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
        WakePresenter();
        // A heartbeat: early (one second in), then every thirty seconds. Tick cost matters: every guest read is a
        // system call or two in the port.
        if (ticks == 60 || ticks % 1800 == 0) {
            const auto& aux = g_system->GPU().DSModAux();
            Say("duo: tick {}: {:.2f} ms average, {:.2f} ms worst over {} ticks; game frames {}; surface {}x{} {}; "
                "page frames posted {}",
                ticks, spent_ms / static_cast<double>(measured), worst_ms, measured,
                g_port.frames != nullptr ? g_port.frames() : 0, aux.width.load(), aux.height.load(),
                aux.present.load() ? "shown" : "hidden", g_posted.load());
            spent_ms = worst_ms = 0;
            measured = 0;
        }
    }
}

} // namespace

void LedTap(const char* name, int64_t value) {
    // Hundreds of values pass here each tick: only the running title's state names (duo_state.h) are kept.
    if (const int i = duo_state_index(g_led_table, name); i >= 0) {
        g_led_pending[static_cast<size_t>(i)] = value;
    }
}

void LedCommit() {
    if (g_port.state == nullptr || g_led_table == nullptr) {
        return;
    }
    CompanionState state;
    g_led_table->fill(g_led_pending.data(), &state);
    g_port.state(&state);
}

// ---- test hooks (duo/test/host_test.cpp); the Android presenter below uses the same entry points -----------------
void SetSurfaceSize(u32 width, u32 height, bool present) {
    auto& aux = g_system->GPU().DSModAux();
    aux.width.store(width);
    aux.height.store(height);
    aux.present.store(present);
}

void BlitFrame(u32* out, u32 out_w, u32 out_h, u32 stride, const std::vector<u32>& pixels, u32 w, u32 h) {
    const auto rect = VideoCore::DSMod::CompanionTouchRect(out_w, out_h, w, h, false);
    const auto swizzle = [](u32 p) { return 0xFF000000u | (p & 0x0000FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu); };
    for (u32 y = 0; y < out_h; ++y) {
        u32* const row = out + static_cast<size_t>(y) * stride;
        if (y < rect.y || y >= rect.y + rect.h) {
            std::fill_n(row, out_w, 0xFF000000u);
            continue;
        }
        const u32 sy = rect.h == h ? y - rect.y : static_cast<u32>(static_cast<u64>(y - rect.y) * h / rect.h);
        const u32* const src = pixels.data() + static_cast<size_t>(sy) * w;
        std::fill_n(row, rect.x, 0xFF000000u);
        if (rect.w == w) {
            for (u32 x = 0; x < w; ++x) {
                row[rect.x + x] = swizzle(src[x]);
            }
        } else {
            for (u32 x = 0; x < rect.w; ++x) {
                row[rect.x + x] = swizzle(src[static_cast<u64>(x) * w / rect.w]);
            }
        }
        std::fill_n(row + rect.x + rect.w, out_w - rect.x - rect.w, 0xFF000000u);
    }
}

void TestTouch(u32 x, u32 y, bool down) {
    using VideoCore::DSMod::AuxTouchPoint;
    if (!down) {
        g_system->GPU().DSModAux().SetTouch({});
        return;
    }
    const AuxTouchPoint point{.finger_id = 1, .x = x, .y = y, .attributes = 1u, .delta_ns = 0};
    g_system->GPU().DSModAux().SetTouch(std::span{&point, 1});
}

bool TakeFrame(std::vector<u32>& pixels, u32& width, u32& height, u64& serial) {
    return g_system->GPU().DSModAux().TakeUi(pixels, width, height, serial);
}

u64 Ticks() {
    return g_ticks.load(std::memory_order_relaxed);
}

void Stop() {
    g_stop.store(true);
    g_frames.release();
    if (g_tick_thread.joinable()) {
        g_tick_thread.join();
    }
    g_runtime.reset();
}

} // namespace Duo

// ---- the second screen (Android) --------------------------------------------------------------------------------
#ifdef __ANDROID__
namespace Duo {
namespace {

void PresentLoop() {
    // Started by the first surface, which can precede duo_host_start: the port's table may not be there yet.
    Common::SetCurrentThreadName("duo-present");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    std::vector<u32> pixels;
    u32 w = 0, h = 0;
    u64 serial = 0, seen_generation = 0;
    ANativeWindow* shown_on = nullptr;
    for (;;) {
        {
            std::unique_lock wake{g_wake_mutex};
            g_present_wake.wait_for(wake, 100ms, [&] { return g_present_generation != seen_generation; });
            seen_generation = g_present_generation;
        }
        std::scoped_lock lock{g_surface_mutex};
        if (g_window == nullptr) {
            shown_on = nullptr;
            continue;
        }
        // A new surface starts black and gets the current page again, changed or not.
        const bool fresh = shown_on != g_window;
        if (fresh) {
            serial = 0;
        }
        const bool have = TakeFrame(pixels, w, h, serial);
        if (!have && !fresh) {
            continue;
        }
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(g_window, &buffer, nullptr) != 0) {
            continue; // the surface is going away; surfaceDestroyed follows
        }
        if (have) {
            BlitFrame(static_cast<u32*>(buffer.bits), static_cast<u32>(buffer.width), static_cast<u32>(buffer.height),
                      static_cast<u32>(buffer.stride), pixels, w, h);
        } else {
            for (s32 y = 0; y < buffer.height; ++y) {
                std::fill_n(static_cast<u32*>(buffer.bits) + static_cast<size_t>(y) * buffer.stride, buffer.width,
                            0xFF000000u);
            }
        }
        ANativeWindow_unlockAndPost(g_window);
        shown_on = g_window;
        if (have && g_posted.fetch_add(1) == 0) {
            Say("duo: first page frame on the second screen: {}x{} into a {}x{} surface", w, h, buffer.width,
                buffer.height);
        }
    }
}

std::once_flag g_presenter_once;

} // namespace
} // namespace Duo

/// DuoSurfaceView.nativeSurface: the second screen's Surface, or null when it is gone. Returns only after the
/// presenter has let go of the previous one (it posts under the same mutex), as surfaceDestroyed requires.
DUO_EXPORT JNIEXPORT void JNICALL Java_com_aure_totk_companion_DuoSurfaceView_nativeSurface(JNIEnv* env, jclass,
                                                                                           jobject surface,
                                                                                           jint width, jint height) {
    using namespace Duo;
    ANativeWindow* const next = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (next != nullptr) {
        ANativeWindow_setBuffersGeometry(next, width, height, WINDOW_FORMAT_RGBA_8888);
    }
    {
        std::scoped_lock lock{g_surface_mutex};
        if (g_window != nullptr) {
            ANativeWindow_release(g_window);
        }
        g_window = next;
        // The runtime draws only while a surface exists (AuxRouting.present), at the size touches arrive in.
        SetSurfaceSize(next != nullptr ? static_cast<u32>(width) : 0, next != nullptr ? static_cast<u32>(height) : 0,
                       next != nullptr);
    }
    std::call_once(g_presenter_once, [] { std::thread{PresentLoop}.detach(); });
    WakePresenter();
    Say("duo: second-screen surface {}", next != nullptr ? std::format("{}x{}", width, height) : std::string{"gone"});
}

/// DuoSurfaceView.nativeTouch: the pointers now down, in surface pixels (count 0: all lifted). Off unless
/// TOTK_DUO_TOUCH=1: touch is the next milestone and has not run on a device.
DUO_EXPORT JNIEXPORT void JNICALL Java_com_aure_totk_companion_DuoSurfaceView_nativeTouch(JNIEnv* env, jclass,
                                                                                         jintArray ids,
                                                                                         jfloatArray xs,
                                                                                         jfloatArray ys, jint count) {
    using namespace Duo;
    static const bool enabled = EnvIs("TOTK_DUO_TOUCH", "1");
    if (!enabled) {
        return;
    }
    using VideoCore::DSMod::AuxRouting;
    using VideoCore::DSMod::AuxTouchPoint;
    static std::mutex mutex;
    static std::array<u32, AuxRouting::MaxTouch> down{}; // finger ids (pointer id + 1) of the previous event
    static size_t down_count = 0;
    std::scoped_lock lock{mutex};
    std::array<AuxTouchPoint, AuxRouting::MaxTouch> points{};
    const size_t n = std::min<size_t>(count > 0 ? static_cast<size_t>(count) : 0, points.size());
    std::array<jint, AuxRouting::MaxTouch> id{};
    std::array<jfloat, AuxRouting::MaxTouch> x{}, y{};
    if (n != 0) {
        env->GetIntArrayRegion(ids, 0, static_cast<jsize>(n), id.data());
        env->GetFloatArrayRegion(xs, 0, static_cast<jsize>(n), x.data());
        env->GetFloatArrayRegion(ys, 0, static_cast<jsize>(n), y.data());
    }
    for (size_t i = 0; i < n; ++i) {
        const u32 finger = static_cast<u32>(id[i]) + 1;
        const bool was_down = std::find(down.begin(), down.begin() + down_count, finger) != down.begin() + down_count;
        points[i] = AuxTouchPoint{.finger_id = finger,
                                  .x = static_cast<u32>(std::max(0.0f, x[i])),
                                  .y = static_cast<u32>(std::max(0.0f, y[i])),
                                  .attributes = was_down ? 0u : 1u,
                                  .delta_ns = 0};
    }
    for (size_t i = 0; i < n; ++i) {
        down[i] = points[i].finger_id;
    }
    down_count = n;
    g_system->GPU().DSModAux().SetTouch(std::span{points}.first(n)); // a finger no longer listed ends there
}
#endif // __ANDROID__

// ---- the port's two entry points --------------------------------------------------------------------------------
DUO_EXPORT int duo_host_start(const DuoPort* port) {
    using namespace Duo;
    if (port == nullptr || port->abi != DUO_PORT_ABI || port->size != sizeof(DuoPort) || port->safe_read == nullptr ||
        port->log == nullptr) {
        std::fprintf(stderr, "[duo] refused: the port's service table is from another build\n");
        return 0;
    }
    if (g_started.exchange(true)) {
        return 1;
    }
    g_port = *port;
    g_package_root = port->package_root != nullptr ? port->package_root : "";
    g_data_dir = port->data_dir != nullptr ? port->data_dir : "";
    g_port.package_root = g_package_root.c_str();
    g_port.data_dir = g_data_dir.c_str();
    g_led_table = duo_state_table(port->title_id);
    SetPort(&g_port);
    Common::Log::verbose.store(EnvIs("TOTK_DUO_DEBUG", "1"));
    g_tick_thread = std::thread{TickLoop};
#ifdef __ANDROID__
    g_tick_thread.detach();
#endif
    return 1;
}

DUO_EXPORT void duo_host_frame(void) {
    Duo::g_frames.release();
}
