#include "engine/content_hash.hpp"
#include "runtime/asset_vfs_catalog.hpp"
#include "runtime/vfs_asset_reader.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

class TemporaryTree final {
public:
    TemporaryTree() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("noemancer-vfs-asset-reader-" + std::to_string(nonce));
        std::filesystem::create_directories(root);
    }
    ~TemporaryTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

int fail(const int code, const std::string& detail) {
    std::cerr << detail << '\n';
    return code;
}

std::vector<std::byte> fixture_bytes(const std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()),
        reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

void write_bytes(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

noemancer::AssetRecord asset(
    std::string id, std::string relative_path, const std::filesystem::path& root,
    const std::span<const std::byte> bytes, const bool available = true) {
    const auto hash = noemancer::sha256_bytes(bytes);
    return {.id = std::move(id),
            .display_name = "VFS reader fixture",
            .kind = "CookedPayload",
            .uri = "asset://fixture",
            .source_root = std::filesystem::absolute(root).lexically_normal().generic_string(),
            .relative_path = std::move(relative_path),
            .extension = ".bin",
            .content_hash = hash.value,
            .hash_provenance = "computed",
            .license = "CC0",
            .redistribution = "allowed",
            .import_state = available ? "ready" : "missing",
            .source_bytes = available ? bytes.size() : 0U,
            .available = available};
}

noemancer::AssetVfsCatalog catalog_for(
    const std::vector<std::filesystem::path>& roots,
    const std::vector<noemancer::AssetRecord>& assets,
    const std::string& identity,
    const noemancer::VfsMountKind kind) {
    return noemancer::build_asset_vfs_catalog(roots, assets, 42U,
        {.mount_identity = identity, .mount_kind = kind});
}

bool mount_catalog(noemancer::VirtualFileSystem& vfs, const noemancer::AssetVfsCatalog& catalog) {
    return std::ranges::all_of(catalog.mounts, [&](const noemancer::VfsMountSpec& mount) {
        return vfs.mount(mount).success;
    });
}

} // namespace

int main() {
    TemporaryTree tree;
    const auto source_a = tree.root / "source" / "a";
    const auto source_b = tree.root / "source" / "b";
    const auto package_a = tree.root / "package" / "a";
    const auto package_b = tree.root / "package" / "b";
    const auto mesh = fixture_bytes("mesh-payload-0123456789");
    const auto animation = fixture_bytes("animation-payload-abcdefgh");
    for (const auto& root : {source_a, package_a}) write_bytes(root / "hero.meshbin", mesh);
    for (const auto& root : {source_b, package_b}) write_bytes(root / "hero.animbin", animation);

    const std::vector<std::filesystem::path> source_roots{source_a, source_b};
    const std::vector<std::filesystem::path> package_roots{package_a, package_b};
    std::vector<noemancer::AssetRecord> source_records{
        asset("asset.mesh.hero", "hero.meshbin", source_a, mesh),
        asset("asset.animation.hero", "hero.animbin", source_b, animation),
        asset("asset.unavailable", "missing.ktx2", source_a, {}, false)};
    std::vector<noemancer::AssetRecord> package_records{
        asset("asset.mesh.hero", "hero.meshbin", package_a, mesh),
        asset("asset.animation.hero", "hero.animbin", package_b, animation),
        asset("asset.unavailable", "missing.ktx2", package_a, {}, false)};
    auto source_catalog = catalog_for(source_roots, source_records, "reader.source", noemancer::VfsMountKind::directory);
    auto package_catalog = catalog_for(package_roots, package_records, "reader.package", noemancer::VfsMountKind::package_directory);
    noemancer::VirtualFileSystem source_vfs;
    noemancer::VirtualFileSystem package_vfs;
    if (!source_catalog.success || !package_catalog.success ||
        !mount_catalog(source_vfs, source_catalog) || !mount_catalog(package_vfs, package_catalog)) {
        return fail(1, "Source/package fixture catalogs could not be mounted.");
    }

    const auto source_mesh = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.mesh.hero");
    const auto package_mesh = noemancer::read_vfs_asset(package_vfs, package_catalog, "asset.mesh.hero");
    if (!source_mesh.success || !package_mesh.success || !source_mesh.content_hash_verified ||
        !package_mesh.content_hash_verified || source_mesh.bytes != mesh || package_mesh.bytes != mesh ||
        source_mesh.bytes != package_mesh.bytes || source_mesh.uri != package_mesh.uri ||
        source_mesh.mount_id == package_mesh.mount_id || source_mesh.catalog_revision != 42U) {
        return fail(2, "Full source/package reads did not preserve identity, bytes and verified integrity.");
    }
    const auto source_animation = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.animation.hero");
    if (!source_animation.success || source_animation.bytes != animation ||
        source_animation.uri.find("roots/1/") == std::string::npos) {
        return fail(3, "The second asset root was not resolved through its canonical URI.");
    }

    const auto unknown = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.unknown");
    const auto unavailable = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.unavailable");
    if (unknown.success || unavailable.success || unknown.code != "asset-read.not-found" ||
        unavailable.code != "asset-read.not-found" || !unknown.uri.empty() || !unavailable.uri.empty()) {
        return fail(4, "Unknown or unavailable assets did not fail closed without leaking a path.");
    }

    const auto rejected_range = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.mesh.hero",
        {.offset = 5U, .length = 7U, .byte_budget = 7U});
    if (rejected_range.success || rejected_range.code != "asset-read.integrity-requires-full-read") {
        return fail(5, "A range incorrectly claimed whole-asset integrity verification.");
    }
    const auto range = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.mesh.hero",
        {.offset = 5U, .length = 7U, .byte_budget = 7U, .verify_content_hash = false});
    if (!range.success || range.content_hash_verified || range.bytes != fixture_bytes("payload") ||
        range.offset != 5U || range.eof || range.observed_content_hash.empty()) {
        return fail(6, "The explicit bounded range read did not return an honest plain-data receipt.");
    }
    const auto budget = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.mesh.hero",
        {.byte_budget = mesh.size() - 1U});
    if (budget.success || budget.code != "asset-read.vfs-failed" ||
        budget.detail.find("vfs.read-budget-exceeded") == std::string::npos || !budget.bytes.empty()) {
        return fail(7, "The byte budget did not reject the full read without publishing bytes.");
    }

    int cancellation_checks{};
    const auto cancelled = noemancer::read_vfs_asset(source_vfs, source_catalog, "asset.mesh.hero",
        {.cancelled = [&] { return ++cancellation_checks >= 2; }});
    if (cancelled.success || cancelled.code != "asset-read.vfs-failed" ||
        cancelled.detail.find("vfs.cancelled") == std::string::npos || !cancelled.bytes.empty()) {
        return fail(8, "Cancellation did not propagate without publishing partial bytes.");
    }

    auto mismatch_records = source_records;
    mismatch_records.front().content_hash =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto mismatch_catalog = catalog_for(source_roots, mismatch_records, "reader.mismatch", noemancer::VfsMountKind::directory);
    noemancer::VirtualFileSystem mismatch_vfs;
    if (!mismatch_catalog.success || !mount_catalog(mismatch_vfs, mismatch_catalog)) {
        return fail(9, "The mismatch fixture could not be mounted.");
    }
    const auto mismatch = noemancer::read_vfs_asset(mismatch_vfs, mismatch_catalog, "asset.mesh.hero");
    if (mismatch.success || mismatch.code != "asset-read.hash-mismatch" || mismatch.content_hash_verified ||
        mismatch.observed_content_hash.empty() || !mismatch.bytes.empty()) {
        return fail(10, "A catalog hash mismatch was not rejected atomically.");
    }

    auto bare_records = source_records;
    bare_records.front().content_hash.erase(0U, 7U);
    std::ranges::transform(bare_records.front().content_hash, bare_records.front().content_hash.begin(),
        [](const unsigned char value) { return static_cast<char>(std::toupper(value)); });
    const auto bare_catalog = catalog_for(source_roots, bare_records, "reader.bare", noemancer::VfsMountKind::directory);
    noemancer::VirtualFileSystem bare_vfs;
    if (!bare_catalog.success || !mount_catalog(bare_vfs, bare_catalog) ||
        !noemancer::read_vfs_asset(bare_vfs, bare_catalog, "asset.mesh.hero").success) {
        return fail(11, "The repository-compatible bare SHA-256 spelling was not normalized for verification.");
    }

    auto invalid_records = source_records;
    invalid_records.front().content_hash = "sha256:not-a-real-content-hash";
    const auto invalid_catalog = catalog_for(source_roots, invalid_records, "reader.invalid", noemancer::VfsMountKind::directory);
    noemancer::VirtualFileSystem invalid_vfs;
    if (!invalid_catalog.success || !mount_catalog(invalid_vfs, invalid_catalog)) return fail(12, "Invalid-hash fixture setup failed.");
    const auto unverifiable = noemancer::read_vfs_asset(invalid_vfs, invalid_catalog, "asset.mesh.hero");
    if (unverifiable.success || unverifiable.code != "asset-read.hash-unverifiable") {
        return fail(13, "A placeholder hash was falsely treated as verified content identity.");
    }

    return 0;
}
