// SPDX-License-Identifier: GPL-3.0-or-later
// The title module, linked into this library. Stands in for Eden Duo's mod_module.cpp, which verifies a checksum,
// stages a .so from the package into a code cache and dlopens it: here the Breath of the Wild module
// (vendor/companions/native/modules/01007EF00011E000.cpp) is compiled in, so "loading" is the same handshake
// without a file. The package's manifest still decides: its title id, module ABI and build-id list are checked
// exactly as Eden checks them, and a package for another build gets the same diagnostic.

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/mod_module.h"
#include "core/mods/mod_package_io.h"
#include "duo_host.h"

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
    };
    g_module_api->sample(instance, &tapped);
    Duo::LedCommit();
}

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
        // The Breath of the Wild module exports no font, save, write, data or texture extension.
        LOG_INFO(Core, "DSMod module: loaded {} for title {} build {} (built in)",
                 api->name ? api->name : "unnamed", title, build);
        g_module_api = api;
        g_tapped_api = *api;
        g_tapped_api.sample = TappedSample;
        loaded->api = &g_tapped_api;
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
