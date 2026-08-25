#pragma once

#include "engine/asset_registry.hpp"
#include "engine/virtual_file_system.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct AssetVfsCatalogLimits final {
    std::size_t max_roots{64U};
    std::size_t max_assets{65'536U};
    std::size_t max_string_bytes{1024U};
    std::size_t max_observation_bytes{64U * 1024U};
};

struct AssetVfsCatalogOptions final {
    // A caller-owned, stable identity for this mounted registry. Source and
    // packaged registries deliberately use different identities while keeping
    // the same asset:// URI namespace.
    std::string mount_identity;
    VfsMountKind mount_kind{VfsMountKind::directory};
    AssetVfsCatalogLimits limits{};
};

struct AssetVfsRecord final {
    std::string asset_id;
    std::string uri;
    std::string mount_id;
    std::string content_hash;
    std::string kind;
    std::uintmax_t bytes{};
};

// Runtime-private, plain-data projection of one Asset Registry into VFS mount
// and lookup identities. It does not own files or duplicate Registry state.
struct AssetVfsCatalog final {
    bool success{};
    std::string code;
    std::string detail;
    std::uint64_t registry_revision{};
    std::vector<VfsMountSpec> mounts;
    std::vector<AssetVfsRecord> assets;
    std::size_t unavailable_assets{};
    std::size_t observation_byte_limit{64U * 1024U};

    [[nodiscard]] const AssetVfsRecord* find(std::string_view asset_id) const noexcept;
    [[nodiscard]] std::string observation_json(std::size_t byte_budget = 0U) const;
};

[[nodiscard]] AssetVfsCatalog build_asset_vfs_catalog(
    const AssetRegistry& registry,
    AssetVfsCatalogOptions options);

// Plain-data overload used by package adapters and hostile-input tests. The
// ordering and source_root rules are identical to the AssetRegistry overload.
[[nodiscard]] AssetVfsCatalog build_asset_vfs_catalog(
    std::span<const std::filesystem::path> asset_roots,
    std::span<const AssetRecord> assets,
    std::uint64_t registry_revision,
    AssetVfsCatalogOptions options);

} // namespace noemancer
