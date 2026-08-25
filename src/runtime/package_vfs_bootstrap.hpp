#pragma once

#include "engine/scene_document.hpp"
#include "engine/virtual_file_system.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace noemancer {

struct PackageVfsBootstrapLimits final {
    std::size_t profile_byte_budget{1U * 1024U * 1024U};
    std::size_t scene_byte_budget{32U * 1024U * 1024U};
    std::size_t hud_byte_budget{4U * 1024U * 1024U};
    std::size_t max_relative_path_bytes{1024U};
    VfsLimits vfs_limits{};
};

// Stable, serializable evidence for the package trust-root transition. It
// deliberately contains VFS identities rather than physical host paths.
struct PackageVfsBootstrapReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::string mount_id{"runtime.package"};
    std::uint64_t vfs_revision{};
    std::string profile_uri;
    std::string startup_scene_uri;
    std::string hud_document_uri;
    std::string profile_sha256;
    std::string startup_scene_sha256;
    std::string hud_document_sha256;
    std::uint64_t profile_bytes{};
    std::uint64_t startup_scene_bytes{};
    std::uint64_t hud_document_bytes{};

    [[nodiscard]] std::string json() const;
};

struct PackageVfsBootstrapResult final {
    PackageVfsBootstrapReceipt receipt;
    std::shared_ptr<VirtualFileSystem> vfs;
    nlohmann::json profile;
    std::string profile_text;
    SceneDocument scene;
    std::string hud_document;
    std::string display_name;
    // Runtime-private physical root retained only for adapters which still
    // need migration. It is never included in the receipt or observations.
    std::filesystem::path package_root;

    [[nodiscard]] explicit operator bool() const noexcept { return receipt.success; }
};

// The physical profile is the sole explicit startup trust root. Once its
// canonical package root has been derived and mounted, every document is read
// through package:// with bounded VFS requests.
[[nodiscard]] PackageVfsBootstrapResult bootstrap_package_vfs(
    const std::filesystem::path& game_profile_path,
    PackageVfsBootstrapLimits limits = {});

} // namespace noemancer
