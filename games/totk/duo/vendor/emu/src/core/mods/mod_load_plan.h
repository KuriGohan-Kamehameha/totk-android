// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core {
class System;
}

namespace Core::Mods {

/// Format 2 needs runtime 19. Require an explicit package declaration so older runtimes gate
/// the entire package instead of rendering a companion without its required guest helper.
inline constexpr u32 GuestHelperMinimumRuntime = 19;
[[nodiscard]] constexpr bool IsLoadPlanRuntimeCompatible(u32 format, u32 declared_runtime,
                                                        u32 host_runtime) {
    return (format == 1 || format == 2) && declared_runtime <= host_runtime &&
           (format != 2 || (declared_runtime >= GuestHelperMinimumRuntime &&
                            host_runtime >= GuestHelperMinimumRuntime));
}

struct LoadPlanSegment {
    u32 location{};
    u32 size{};
};

struct LoadPlanLayout {
    std::array<LoadPlanSegment, 3> segments{};
    u32 bss_size{};
};

class ModLoadPlan {
public:
    enum class RelocationKind {
        AArch64Adrp,
        AArch64AddLo12,
        AArch64Branch26,
        AArch64Ldr32Lo12,
        AArch64Ldr64Lo12,
    };

    enum class RelocationTarget { Mailbox, Main, Code };

    struct Relocation {
        u32 offset{};
        RelocationKind kind{};
        s64 addend{};
        RelocationTarget target{RelocationTarget::Mailbox};
    };

    struct GuestCode {
        std::vector<u8> bytes;
        std::vector<Relocation> relocations;
    };

    struct Write {
        u64 offset{};
        std::vector<u8> expected;
        std::vector<u8> replacement;
        std::vector<Relocation> relocations;
    };

    ModLoadPlan() = default;
    ModLoadPlan(u64 mailbox_size_, std::vector<Write> writes_,
                std::optional<u32> epoch_offset_ = std::nullopt,
                std::optional<GuestCode> guest_code_ = std::nullopt, u64 main_size_ = 0)
        : mailbox_size{mailbox_size_}, writes{std::move(writes_)}, epoch_offset{epoch_offset_},
          guest_code{std::move(guest_code_)}, main_size{main_size_} {}

    [[nodiscard]] u64 MailboxSize() const {
        return mailbox_size;
    }

    [[nodiscard]] std::optional<u32> MailboxEpochOffset() const {
        return epoch_offset;
    }

    /// Separate RX allocation. Format 1 continues to use no extra code pages.
    [[nodiscard]] u64 GuestCodeSize() const;

    /// Validates and applies all writes atomically. Offsets are relative to the NSO image, not
    /// an NCE pre-text area. mailbox_address is the final guest virtual address of the mailbox.
    [[nodiscard]] bool Apply(std::span<u8> module_image, VAddr load_base, VAddr mailbox_address,
                             VAddr code_address = 0) const;

private:
    u64 mailbox_size{};
    std::vector<Write> writes;
    std::optional<u32> epoch_offset;
    std::optional<GuestCode> guest_code;
    u64 main_size{};
};

struct LoadPlanDiscovery {
    enum class Status {
        None,        ///< no selected package, or it carries no load plan for this module
        Found,       ///< a plan for this executable
        Unsupported, ///< the package has no plan for this build, or its plan names another build
        Invalid,     ///< the plan for this build is malformed
    };

    Status status{Status::None};
    std::optional<ModLoadPlan> plan;
    std::string error;
    std::string package; ///< the selected package's folder name, for log messages
    std::string build;   ///< the module's build ID (hex), for log messages
};

/// Selects the same first enabled, lexically sorted dual-screen package as ModRuntime::Discover.
/// A manifest without load_plan is treated as no plan. A plan that is missing for this build,
/// names another executable or is malformed is reported as Unsupported or Invalid and never
/// returned. Neither stops the game: the loader starts it without the plan (no guest helper and
/// no mailbox), so the companion runs without guest actions or shows its not-supported page.
[[nodiscard]] LoadPlanDiscovery DiscoverModLoadPlan(System& system, u64 title_id,
                                                    const std::array<u8, 0x20>& build_id,
                                                    std::string_view module_name,
                                                    const LoadPlanLayout& layout);

/// DiscoverModLoadPlan for an already selected package: assets is its dualscreen folder,
/// manifest its parsed manifest.json and required_runtime its declared minimum runtime.
[[nodiscard]] LoadPlanDiscovery ReadModLoadPlan(const FileSys::VirtualDir& assets,
                                                const nlohmann::json& manifest,
                                                u32 required_runtime, u64 title_id,
                                                const std::array<u8, 0x20>& build_id,
                                                std::string_view module_name,
                                                const LoadPlanLayout& layout);

/// The module the selected package's load plan targets ("main" unless the manifest sets
/// load_plan_module). nullopt when the selected package has no load plan or needs a newer runtime.
/// The loader reserves the mailbox after the last module when this is not the last one, so mods
/// that branch between modules at fixed offsets keep their layout.
[[nodiscard]] std::optional<std::string> LoadPlanTargetModule(System& system, u64 title_id);

} // namespace Core::Mods
