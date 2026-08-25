#include "runtime/asset_vfs_catalog.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

class TemporaryTree final {
public:
    TemporaryTree() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("noemancer-asset-vfs-catalog-" + std::to_string(nonce));
        std::filesystem::create_directories(root);
    }
    ~TemporaryTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

void write_fixture(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "textures");
    std::filesystem::create_directories(root / "audio");
    { std::ofstream output(root / "textures" / "hero.bin", std::ios::binary); output << "hero"; }
    { std::ofstream output(root / "audio" / "theme.bin", std::ios::binary); output << "theme!"; }
    std::ofstream registry(root / "registry.json", std::ios::binary);
    registry << R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"asset.texture.hero","displayName":"Hero","kind":"Texture","uri":"asset://textures/hero.bin","path":"textures/hero.bin","contentHash":"sha256:hero"},
      {"id":"asset.audio.theme","displayName":"Theme","kind":"Audio","uri":"asset://audio/theme.bin","path":"audio/theme.bin","contentHash":"sha256:theme"},
      {"id":"asset.optional.missing","displayName":"Optional","kind":"Texture","uri":"asset://missing.bin","path":"missing.bin","contentHash":"sha256:missing","optional":true},
      {"id":"asset.builtin","displayName":"Builtin","kind":"Texture","uri":"builtin://texture/white"}
    ]})";
}

noemancer::AssetRecord record(std::string id, std::string relative_path, std::string source_root,
                              const bool available = true) {
    return {.id = std::move(id),
            .display_name = "Fixture",
            .kind = "Texture",
            .uri = "asset://fixture",
            .source_root = std::move(source_root),
            .relative_path = std::move(relative_path),
            .extension = ".bin",
            .content_hash = "sha256:fixture",
            .hash_provenance = "manifest",
            .license = "CC0",
            .redistribution = "allowed",
            .import_state = available ? "ready" : "missing",
            .source_bytes = available ? 7U : 0U,
            .available = available};
}

int fail(const int code, const std::string& detail) {
    std::cerr << detail << '\n';
    return code;
}

} // namespace

int main() {
    TemporaryTree tree;
    const auto source_root = tree.root / "source";
    const auto package_root = tree.root / "package" / "content" / "assets";
    write_fixture(source_root);
    write_fixture(package_root);

    const noemancer::AssetRegistry source_registry(source_root);
    const noemancer::AssetRegistry package_registry(package_root);
    const auto source = noemancer::build_asset_vfs_catalog(source_registry, {
        .mount_identity = "project.source-assets",
        .mount_kind = noemancer::VfsMountKind::directory});
    const auto package = noemancer::build_asset_vfs_catalog(package_registry, {
        .mount_identity = "package.content-assets",
        .mount_kind = noemancer::VfsMountKind::package_directory});
    if (!source.success || !package.success || source.assets.size() != 2U || package.assets.size() != 2U ||
        source.unavailable_assets != 2U || package.unavailable_assets != 2U) {
        return fail(1, "Source/package registries did not produce equivalent usable asset catalogs.");
    }
    for (std::size_t index = 0U; index < source.assets.size(); ++index) {
        const auto& left = source.assets[index];
        const auto& right = package.assets[index];
        if (left.asset_id != right.asset_id || left.uri != right.uri ||
            left.content_hash != right.content_hash || left.bytes != right.bytes || left.kind != right.kind ||
            left.mount_id == right.mount_id) {
            return fail(2, "Source/package asset identity diverged beyond the explicit mount identity.");
        }
    }
    if (source.mounts.front().kind != noemancer::VfsMountKind::directory ||
        package.mounts.front().kind != noemancer::VfsMountKind::package_directory ||
        source.find("asset.audio.theme") == nullptr || source.find("asset.optional.missing") != nullptr) {
        return fail(3, "Catalog lookup or mount kind did not preserve the plain-data contract.");
    }
    noemancer::VirtualFileSystem source_vfs;
    noemancer::VirtualFileSystem package_vfs;
    for (const auto& mount : source.mounts) {
        if (!source_vfs.mount(mount).success) return fail(31, "A source catalog mount spec was rejected by the VFS.");
    }
    for (const auto& mount : package.mounts) {
        if (!package_vfs.mount(mount).success) return fail(32, "A package catalog mount spec was rejected by the VFS.");
    }
    const auto* source_theme = source.find("asset.audio.theme");
    const auto* package_theme = package.find("asset.audio.theme");
    const auto source_read = source_vfs.read({.uri = source_theme->uri, .byte_budget = 64U});
    const auto package_read = package_vfs.read({.uri = package_theme->uri, .byte_budget = 64U});
    if (!source_read.success || !package_read.success || source_read.bytes != package_read.bytes ||
        source_read.bytes.size() != 6U) {
        return fail(33, "Source and packaged catalog mounts did not resolve identical file bytes.");
    }

    const std::vector<std::filesystem::path> roots{source_root};
    const auto normalized_root = std::filesystem::absolute(source_root).lexically_normal().generic_string();
    for (const auto& unsafe : std::vector<std::string>{"../secret.bin", "textures/../../secret.bin", "C:/secret.bin", "/secret.bin"}) {
        const std::vector<noemancer::AssetRecord> records{record("asset.unsafe", unsafe, normalized_root)};
        const auto rejected = noemancer::build_asset_vfs_catalog(roots, records, 7U,
            {.mount_identity = "fixture.assets"});
        if (rejected.success || rejected.code != "asset-vfs.unsafe-relative-path") {
            return fail(4, "An absolute or parent-traversing Registry path was accepted: " + unsafe);
        }
    }

    const std::vector<noemancer::AssetRecord> duplicates{
        record("asset.duplicate", "one.bin", normalized_root),
        record("asset.duplicate", "two.bin", normalized_root)};
    const auto duplicate = noemancer::build_asset_vfs_catalog(roots, duplicates, 8U,
        {.mount_identity = "fixture.assets"});
    if (duplicate.success || duplicate.code != "asset-vfs.duplicate-asset-id") {
        return fail(5, "Duplicate Asset IDs did not fail the catalog atomically.");
    }

    std::vector<noemancer::AssetRecord> unordered{
        record("asset.z", "z.bin", normalized_root),
        record("asset.unavailable", "missing.bin", normalized_root, false),
        record("asset.a", "a.bin", normalized_root)};
    const auto ordered = noemancer::build_asset_vfs_catalog(roots, unordered, 9U,
        {.mount_identity = "fixture.assets", .limits = {.max_observation_bytes = 1024U}});
    if (!ordered.success || ordered.assets.size() != 2U || ordered.assets[0].asset_id != "asset.a" ||
        ordered.assets[1].asset_id != "asset.z" || ordered.unavailable_assets != 1U ||
        ordered.observation_json() != ordered.observation_json()) {
        return fail(6, "Catalog ordering, unavailable filtering, or JSON stability regressed.");
    }
    const auto bounded_text = ordered.observation_json(512U);
    const auto bounded = nlohmann::json::parse(bounded_text, nullptr, false);
    if (bounded.is_discarded() || bounded_text.size() > 512U || !bounded.value("truncated", false) ||
        bounded.value("assetCount", 0U) != 2U) {
        return fail(7, "Catalog observation was not valid, bounded, deterministic JSON.");
    }
    const auto full = nlohmann::json::parse(ordered.observation_json(1024U));
    if (full.at("schema") != "noemancer.asset-vfs-catalog/0.1" ||
        full.at("assets").front().at("assetId") != "asset.a") {
        return fail(8, "Catalog JSON did not preserve its schema or stable asset ordering.");
    }

    return 0;
}
