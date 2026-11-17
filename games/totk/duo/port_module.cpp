// SPDX-License-Identifier: GPL-3.0-or-later
// The title module, linked into this library. Stands in for Eden Duo's mod_module.cpp, which verifies a checksum,
// stages a .so from the package into a code cache and dlopens it: here the game's module (one per library: the Breath
// of the Wild module vendor/companions/native/modules/01007EF00011E000.cpp or the Tears of the Kingdom module
// 0100F2C0115B6000.cpp, CMakeLists.txt DUO_TITLE) is compiled in, so "loading" is the same handshake without a file.
// The package's manifest still decides: its title id, module ABI and build-id list are checked exactly as Eden checks
// them, and a package for another build gets the same diagnostic.
//
// Item actions (DUO_MODULE_ACTIONS, Tears of the Kingdom): the module's port-only table (vendor/.../
// totk_native_actions.h) is bound at configure time to the runtime's route, the DuoPort ABI 3 functions (duo_port.h,
// MIT; runtime/companion_actions.c behind them). This file only forwards: the rules, the gate, the one request at a
// time and every game call stay the runtime's, on the game's frame-end thread. Without them (a runtime that offers
// none) the module is bound to nothing and shows its actions unavailable, as in Eden Duo.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/mod_module.h"
#include "core/mods/mod_package_io.h"
#include "duo_host.h"
#if DUO_MODULE_ACTIONS
#include "totk_native_actions.h"
#endif

namespace Core::Mods {
namespace {

bool IsHex(std::string_view value, size_t length) {
    return value.size() == length &&
           std::ranges::all_of(value, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string Upper(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

// The app's stick-LED bridge wants a few of the numbers the module publishes (duo_host.h LedTap). The runtime keeps
// them where nothing outside it can read, so the module is given the runtime's host table with publish_i64 passing
// through LedTap first. Nothing else of the module's interface is changed, and the runtime sees every call it did.
const EdenDsmodModuleApi* g_module_api = nullptr;
EdenDsmodModuleApi g_tapped_api{};
thread_local const EdenDsmodHostApi* t_runtime_host = nullptr;

// Duo::TestWatch: the desktop test reads some of the module's values back (off unless a test asks).
std::atomic<bool> g_watching{false};
std::mutex g_watch_mutex;
std::string g_watch_prefix;
std::map<std::string, std::string, std::less<>> g_watched;
void Watch(const char* name, std::string value) {
    std::scoped_lock lock{g_watch_mutex};
    if (name != nullptr && std::string_view{name}.starts_with(g_watch_prefix)) {
        g_watched[name] = std::move(value);
    }
}

void TappedSample(void* instance, const EdenDsmodHostApi* host) {
    struct Scope {
        const EdenDsmodHostApi* previous;
        ~Scope() {
            t_runtime_host = previous;
        }
    } scope{t_runtime_host};
    t_runtime_host = host;
    EdenDsmodHostApi tapped = *host;
    tapped.publish_i64 = [](void* userdata, const char* name, int64_t value) {
        t_runtime_host->publish_i64(userdata, name, value);
        Duo::LedTap(name, value);
        if (g_watching.load(std::memory_order_relaxed)) {
            Watch(name, std::to_string(value));
        }
    };
    if (g_watching.load(std::memory_order_relaxed)) {
        tapped.publish_text = [](void* userdata, const char* name, const char* value) {
            t_runtime_host->publish_text(userdata, name, value);
            Watch(name, value != nullptr ? value : "");
        };
    }
    g_module_api->sample(instance, &tapped);
    Duo::LedCommit();
}

#if DUO_MODULE_ACTIONS
// The runtime's item actions (DuoPort ABI 3) in the module's table: plain forwarding, the port's functions are
// thread-safe and the strings they return are static.
const DuoPort* ActionPort() {
    const DuoPort* const port = Duo::Port();
    return port != nullptr && port->item_actions != nullptr && port->action_state != nullptr &&
                   port->pouch_gen != nullptr && port->request != nullptr && port->result != nullptr
               ? port
               : nullptr;
}
const TotkNativeActions g_native_actions{
    .version = TOTK_NATIVE_ACTIONS_VERSION,
    .struct_size = sizeof(TotkNativeActions),
    .userdata = nullptr,
    .item_actions = [](void*, int32_t key, int32_t slot, TotkNativeAction out[3]) -> uint32_t {
        DuoPortAction actions[3]{};
        const unsigned n = std::min(ActionPort()->item_actions(key, slot, actions), 3u);
        for (unsigned i = 0; i < n; ++i) {
            out[i] = {actions[i].id, actions[i].enabled, actions[i].reason};
        }
        return n;
    },
    .action_state = [](void*, int32_t* ok, const char** reason) {
        int on = 0;
        ActionPort()->action_state(&on, reason);
        *ok = on;
    },
    .pouch_gen = [](void*) -> int64_t { return ActionPort()->pouch_gen(); },
    .request = [](void*, const char* action, const char* category, int32_t slot, const char* actor, int32_t partition,
                  int64_t pouch_gen) -> int32_t {
        return ActionPort()->request(action, category, slot, actor, partition, pouch_gen);
    },
    .result = [](void*, int32_t seq, char* out, size_t cap) -> size_t { return ActionPort()->result(seq, out, cap); },
};
const EdenDsmodModuleExtensions* g_module_extensions = nullptr;
EdenDsmodModuleExtensions g_tapped_extensions{};
void TappedConfigure(void* instance, const EdenDsmodHostExtensions* host) {
    g_module_extensions->configure(instance, host);
    const bool offered = ActionPort() != nullptr;
    totk_native_port_bind_actions(instance, offered ? &g_native_actions : nullptr);
    LOG_INFO(Core, "DSMod module: item actions {}",
             offered ? "through the runtime (DuoPort ABI 3)" : "not offered by this runtime; shown unavailable");
}
#endif

} // namespace

void SetModuleCacheDirectory(const std::filesystem::path&) {}

GameModule::~GameModule() = default;

std::unique_ptr<GameModule> GameModule::Load(FileSys::VirtualDir assets, u64 title_id,
                                             std::string_view build_id, std::string& error) {
    error.clear();
    try {
        if (!assets) {
            return nullptr;
        }
        const auto manifest = ReadPackageJson(assets->GetFile("manifest.json"));
        if (!manifest || !manifest->is_object()) {
            throw std::runtime_error("missing or malformed package manifest");
        }
        if (!manifest->contains("module")) {
            if (manifest->value("requires_module", false)) {
                throw std::runtime_error("this dual-screen package is missing its required reader module");
            }
            return nullptr;
        }
        const auto title = std::format("{:016X}", title_id);
        const auto declared_title = manifest->at("title_id").get<std::string>();
        if (!IsHex(declared_title, 16) || Upper(declared_title) != title) {
            throw std::runtime_error("module title ID does not match the running game");
        }
        const auto& module = manifest->at("module");
        if (module.at("abi").get<u32>() != EDEN_DSMOD_MODULE_ABI_VERSION) {
            throw std::runtime_error("unsupported dual-screen module ABI; update the package");
        }
        const std::string build = Upper(std::string{build_id});
        if (!IsHex(build, build.size()) || build.size() < 16 || build.size() > 64) {
            throw std::runtime_error("invalid running game build ID");
        }
        const auto builds = module.at("build_ids").get<std::vector<std::string>>();
        if (builds.empty() || builds.size() > 256 ||
            !std::ranges::all_of(builds,
                                 [](const auto& b) { return (b.size() == 16 || b.size() == 64) && IsHex(b, b.size()); }) ||
            !std::ranges::any_of(builds, [&](const auto& b) { return build.starts_with(Upper(b)); })) {
            throw std::runtime_error("dual-screen module does not support this game build");
        }
        // The package's "libraries" entries name Eden's loadable builds of this same source; the port's copy is
        // the one linked here (vendor/README.md records the commit).
        auto loaded = std::unique_ptr<GameModule>{new GameModule};
        loaded->api = eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
        const auto* const api = loaded->api;
        if (!api || api->abi_version != EDEN_DSMOD_MODULE_ABI_VERSION ||
            api->struct_size != sizeof(EdenDsmodModuleApi) || api->abi_hash != EDEN_DSMOD_MODULE_ABI_HASH ||
            api->title_id != title_id || !api->supports_build || !api->create || !api->destroy ||
            !api->sample || !api->tick) {
            throw std::runtime_error("incompatible dual-screen module interface");
        }
        if (!api->supports_build(build.c_str())) {
            throw std::runtime_error("native module rejected this game build");
        }
        loaded->extensions = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
        if (const auto* const ext = loaded->extensions;
            !ext || ext->version != EDEN_DSMOD_EXT_VERSION ||
            ext->struct_size != sizeof(EdenDsmodModuleExtensions) || ext->abi_hash != EDEN_DSMOD_EXT_HASH ||
            !ext->configure) {
            throw std::runtime_error("incompatible dual-screen module extensions");
        }
        // The font extension, negotiated as Eden's mod_module.cpp does (NegotiateExtension, not required): absent or a
        // null answer, no font of the module's own; anything else must match this build's struct exactly. The Tears of
        // the Kingdom module decodes the game's Rodin font this way; the Breath of the Wild module defines no
        // eden_dsmod_get_font_extensions (DUO_MODULE_FONT, CMakeLists.txt, says whether the linked module does: the
        // dlsym of Eden's loader). Neither exports a save, write, data or texture extension.
#if DUO_MODULE_FONT
        {
            const auto* const font =
                eden_dsmod_get_font_extensions(EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH);
            if (font != nullptr &&
                (font->version != EDEN_DSMOD_FONT_EXT_VERSION || font->struct_size != sizeof(EdenDsmodFontExtensions) ||
                 font->abi_hash != EDEN_DSMOD_FONT_EXT_HASH || !font->decode_font)) {
                throw std::runtime_error("incompatible dual-screen font extensions");
            }
            loaded->font_extensions = font;
        }
#endif
        LOG_INFO(Core, "DSMod module: loaded {} for title {} build {} (built in)",
                 api->name ? api->name : "unnamed", title, build);
        g_module_api = api;
        g_tapped_api = *api;
        g_tapped_api.sample = TappedSample;
        loaded->api = &g_tapped_api;
#if DUO_MODULE_ACTIONS
        g_module_extensions = loaded->extensions;
        g_tapped_extensions = *loaded->extensions;
        g_tapped_extensions.configure = TappedConfigure;
        loaded->extensions = &g_tapped_extensions;
#endif
        return loaded;
    } catch (const std::exception& exception) {
        error = exception.what();
        LOG_ERROR(Core, "DSMod module: {}", error);
        return nullptr;
    } catch (...) {
        error = "dual-screen module threw while loading";
        LOG_ERROR(Core, "DSMod module: {}", error);
        return nullptr;
    }
}

ModuleActionOutcome CallModuleAction(const EdenDsmodModuleExtensions* extensions, void* instance,
                                     const std::string& name, s64 argument) {
    if (!extensions || !extensions->on_action || !instance || name.empty() ||
        name.size() > MaxModuleActionName) {
        return ModuleActionOutcome::NotRun;
    }
    try {
        if (extensions->on_action(instance, name.c_str(), argument)) {
            return ModuleActionOutcome::Accepted;
        }
        LOG_INFO(Core, "DSMod: module declined action '{}'", name);
    } catch (...) {
        LOG_ERROR(Core, "DSMod: module action '{}' threw", name);
    }
    return ModuleActionOutcome::Declined;
}

} // namespace Core::Mods

namespace Duo {
void TestWatch(const char* prefix) {
    std::scoped_lock lock{Core::Mods::g_watch_mutex};
    Core::Mods::g_watch_prefix = prefix != nullptr ? prefix : "";
    Core::Mods::g_watched.clear();
    Core::Mods::g_watching.store(prefix != nullptr && *prefix != 0);
}
std::string TestValue(const std::string& name) {
    std::scoped_lock lock{Core::Mods::g_watch_mutex};
    const auto it = Core::Mods::g_watched.find(name);
    return it != Core::Mods::g_watched.end() ? it->second : std::string{};
}
} // namespace Duo
