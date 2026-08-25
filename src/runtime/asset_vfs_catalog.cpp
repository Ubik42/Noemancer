#include "asset_vfs_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

AssetVfsCatalog failure(const std::uint64_t revision, const AssetVfsCatalogLimits& limits,
                        std::string code, std::string detail) {
    return {.success = false,
            .code = std::move(code),
            .detail = std::move(detail),
            .registry_revision = revision,
            .observation_byte_limit = limits.max_observation_bytes};
}

bool valid_identity(const std::string_view value, const std::size_t max_bytes) {
    if (value.empty() || value.size() > max_bytes) return false;
    return std::ranges::all_of(value, [](const unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
    });
}

bool bounded_string(const std::string_view value, const std::size_t max_bytes) {
    return value.size() <= max_bytes && value.find('\0') == std::string_view::npos;
}

std::filesystem::path normalized_root(const std::filesystem::path& root) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(root, error);
    if (error) return {};
    return absolute.lexically_normal();
}

bool safe_relative_path(const std::string_view value, std::filesystem::path& normalized) {
    if (value.empty() || value.find('\0') != std::string_view::npos) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || character == ':' || character == '?' || character == '#' || character == '%') return false;
    }
    const std::filesystem::path candidate{value};
    if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory()) return false;
    for (const auto& component : candidate) {
        if (component == "..") return false;
    }
    normalized = candidate.lexically_normal();
    if (normalized.empty() || normalized == "." || normalized.is_absolute() ||
        normalized.has_root_name() || normalized.has_root_directory()) return false;
    for (const auto& component : normalized) {
        if (component == "..") return false;
    }
    return true;
}

Json mount_json(const VfsMountSpec& mount) {
    return {{"id", mount.id},
            {"kind", vfs_mount_kind_name(mount.kind)},
            {"priority", mount.priority},
            {"readOnly", mount.read_only},
            {"sourceRoot", mount.source_root.generic_string()},
            {"virtualRoot", mount.virtual_root}};
}

Json asset_json(const AssetVfsRecord& asset) {
    return {{"assetId", asset.asset_id},
            {"bytes", asset.bytes},
            {"contentHash", asset.content_hash},
            {"kind", asset.kind},
            {"mountId", asset.mount_id},
            {"uri", asset.uri}};
}

} // namespace

const AssetVfsRecord* AssetVfsCatalog::find(const std::string_view asset_id) const noexcept {
    const auto found = std::ranges::lower_bound(assets, asset_id, {}, &AssetVfsRecord::asset_id);
    return found != assets.end() && found->asset_id == asset_id ? &*found : nullptr;
}

std::string AssetVfsCatalog::observation_json(const std::size_t byte_budget) const {
    const auto configured = std::max<std::size_t>(2U, observation_byte_limit);
    const auto budget = byte_budget == 0U ? configured : std::min(configured, byte_budget);
    if (budget < 2U) return {};

    const auto make = [&](const std::size_t mount_count, const std::size_t asset_count) {
        Json observed_mounts = Json::array();
        for (std::size_t index = 0; index < mount_count; ++index) observed_mounts.push_back(mount_json(mounts[index]));
        Json observed_assets = Json::array();
        for (std::size_t index = 0; index < asset_count; ++index) observed_assets.push_back(asset_json(assets[index]));
        return Json{{"schema", "noemancer.asset-vfs-catalog/0.1"},
                    {"success", success},
                    {"code", code},
                    {"detail", detail},
                    {"registryRevision", registry_revision},
                    {"mounts", std::move(observed_mounts)},
                    {"assets", std::move(observed_assets)},
                    {"mountCount", mounts.size()},
                    {"assetCount", assets.size()},
                    {"unavailableAssetCount", unavailable_assets},
                    {"observedMountCount", mount_count},
                    {"observedAssetCount", asset_count},
                    {"omittedMountCount", mounts.size() - mount_count},
                    {"omittedAssetCount", assets.size() - asset_count},
                    {"truncated", mount_count != mounts.size() || asset_count != assets.size()}};
    };

    auto fits = [&](const std::size_t mount_count, const std::size_t asset_count, std::string* encoded) {
        auto candidate = make(mount_count, asset_count).dump();
        const auto accepted = candidate.size() <= budget;
        if (accepted && encoded != nullptr) *encoded = std::move(candidate);
        return accepted;
    };

    std::string encoded;
    if (fits(mounts.size(), assets.size(), &encoded)) return encoded;

    std::size_t mount_count = mounts.size();
    while (mount_count > 0U && !fits(mount_count, 0U, nullptr)) --mount_count;
    if (!fits(mount_count, 0U, nullptr)) {
        const auto minimal = Json{{"schema", "noemancer.asset-vfs-catalog/0.1"}, {"truncated", true}}.dump();
        return minimal.size() <= budget ? minimal : std::string{"{}"};
    }

    std::size_t low = 0U;
    std::size_t high = assets.size();
    while (low < high) {
        const auto middle = low + (high - low + 1U) / 2U;
        if (fits(mount_count, middle, nullptr)) low = middle;
        else high = middle - 1U;
    }
    static_cast<void>(fits(mount_count, low, &encoded));
    return encoded;
}

AssetVfsCatalog build_asset_vfs_catalog(const AssetRegistry& registry, AssetVfsCatalogOptions options) {
    return build_asset_vfs_catalog(registry.asset_roots(), registry.records(), registry.revision(), std::move(options));
}

AssetVfsCatalog build_asset_vfs_catalog(
    const std::span<const std::filesystem::path> asset_roots,
    const std::span<const AssetRecord> assets,
    const std::uint64_t registry_revision,
    AssetVfsCatalogOptions options) {
    const auto& limits = options.limits;
    if (limits.max_roots == 0U || limits.max_assets == 0U || limits.max_string_bytes == 0U ||
        limits.max_observation_bytes < 2U) {
        return failure(registry_revision, limits, "asset-vfs.invalid-limits", "Catalog limits must be non-zero and permit a JSON object.");
    }
    if (!valid_identity(options.mount_identity, std::min<std::size_t>(limits.max_string_bytes, 120U))) {
        return failure(registry_revision, limits, "asset-vfs.invalid-mount-identity",
                       "Mount identity must contain only ASCII letters, digits, '.', '_' or '-'.");
    }
    if (asset_roots.empty()) {
        return failure(registry_revision, limits, "asset-vfs.root-missing", "The Registry has no asset root.");
    }
    if (asset_roots.size() > limits.max_roots) {
        return failure(registry_revision, limits, "asset-vfs.root-limit", "The Registry exceeds the bounded root count.");
    }
    if (assets.size() > limits.max_assets) {
        return failure(registry_revision, limits, "asset-vfs.asset-limit", "The Registry exceeds the bounded asset count.");
    }

    AssetVfsCatalog result{.success = false,
                           .code = "asset-vfs.building",
                           .detail = "Catalog construction is in progress.",
                           .registry_revision = registry_revision,
                           .observation_byte_limit = limits.max_observation_bytes};
    std::vector<std::filesystem::path> roots;
    roots.reserve(asset_roots.size());
    result.mounts.reserve(asset_roots.size());
    for (std::size_t index = 0U; index < asset_roots.size(); ++index) {
        const auto root = normalized_root(asset_roots[index]);
        if (root.empty() || root.generic_string().size() > limits.max_string_bytes) {
            return failure(registry_revision, limits, "asset-vfs.invalid-root", "An asset root is invalid or exceeds the string budget.");
        }
        if (std::ranges::find(roots, root) != roots.end()) {
            return failure(registry_revision, limits, "asset-vfs.duplicate-root", "The Registry contains a duplicate normalized asset root.");
        }
        roots.push_back(root);
        const auto suffix = std::to_string(index);
        result.mounts.push_back({.id = options.mount_identity + ".root." + suffix,
                                 .virtual_root = "asset://roots/" + suffix + "/",
                                 .source_root = root,
                                 .kind = options.mount_kind,
                                 .priority = static_cast<std::int32_t>(asset_roots.size() - index),
                                 .read_only = true});
    }

    std::unordered_set<std::string> ids;
    ids.reserve(assets.size());
    result.assets.reserve(assets.size());
    for (const auto& asset : assets) {
        if (!bounded_string(asset.id, limits.max_string_bytes) || asset.id.empty()) {
            return failure(registry_revision, limits, "asset-vfs.invalid-asset-id", "An Asset ID is empty or exceeds the string budget.");
        }
        if (!ids.insert(asset.id).second) {
            return failure(registry_revision, limits, "asset-vfs.duplicate-asset-id", "The Registry contains duplicate Asset ID: " + asset.id);
        }

        if (asset.relative_path.empty()) {
            if (!asset.available || asset.uri.starts_with("builtin://")) {
                ++result.unavailable_assets;
                continue;
            }
            return failure(registry_revision, limits, "asset-vfs.unsafe-relative-path",
                           "Available filesystem asset " + asset.id + " has no relative path.");
        }
        std::filesystem::path relative;
        if (!safe_relative_path(asset.relative_path, relative)) {
            return failure(registry_revision, limits, "asset-vfs.unsafe-relative-path",
                           "Asset " + asset.id + " has an absolute or parent-traversing relative path.");
        }
        if (!asset.available) {
            ++result.unavailable_assets;
            continue;
        }
        if (asset.kind.empty() || !bounded_string(asset.kind, limits.max_string_bytes) ||
            !bounded_string(asset.content_hash, limits.max_string_bytes) || asset.content_hash.empty()) {
            return failure(registry_revision, limits, "asset-vfs.invalid-metadata",
                           "Asset " + asset.id + " has missing or oversized VFS metadata.");
        }

        const auto source_root = asset.source_root.empty() ? roots.front() : normalized_root(asset.source_root);
        const auto found_root = std::ranges::find(roots, source_root);
        if (found_root == roots.end()) {
            return failure(registry_revision, limits, "asset-vfs.unknown-source-root",
                           "Asset " + asset.id + " does not belong to a mounted Registry root.");
        }
        const auto root_index = static_cast<std::size_t>(std::distance(roots.begin(), found_root));
        const auto relative_string = relative.generic_string();
        const auto uri = result.mounts[root_index].virtual_root + relative_string;
        if (uri.size() > limits.max_string_bytes) {
            return failure(registry_revision, limits, "asset-vfs.uri-limit", "An asset URI exceeds the string budget.");
        }
        result.assets.push_back({.asset_id = asset.id,
                                 .uri = uri,
                                 .mount_id = result.mounts[root_index].id,
                                 .content_hash = asset.content_hash,
                                 .kind = asset.kind,
                                 .bytes = asset.source_bytes});
    }

    std::ranges::sort(result.assets, [](const AssetVfsRecord& left, const AssetVfsRecord& right) {
        if (left.asset_id != right.asset_id) return left.asset_id < right.asset_id;
        return left.uri < right.uri;
    });
    result.success = true;
    result.code = "ok";
    result.detail = "Asset Registry projected into bounded VFS mount and asset identities.";
    return result;
}

} // namespace noemancer
