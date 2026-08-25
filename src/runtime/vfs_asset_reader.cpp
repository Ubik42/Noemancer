#include "runtime/vfs_asset_reader.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace noemancer {
namespace {

[[nodiscard]] VfsAssetReadReceipt failure(
    std::string code, std::string detail, const AssetVfsCatalog& catalog,
    const std::string_view asset_id, const AssetVfsRecord* record = nullptr) {
    VfsAssetReadReceipt receipt;
    receipt.code = std::move(code);
    receipt.detail = std::move(detail);
    receipt.asset_id = std::string(asset_id);
    receipt.catalog_revision = catalog.registry_revision;
    if (record != nullptr) {
        receipt.uri = record->uri;
        receipt.kind = record->kind;
        receipt.mount_id = record->mount_id;
        receipt.expected_content_hash = record->content_hash;
        receipt.total_bytes = static_cast<std::uint64_t>(record->bytes);
    }
    return receipt;
}

[[nodiscard]] bool normalize_sha256(const std::string_view value, std::string& normalized) {
    auto digits = value;
    if (digits.starts_with("sha256:")) digits.remove_prefix(7U);
    if (digits.size() != 64U || !std::ranges::all_of(digits, [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        })) return false;
    normalized = "sha256:";
    normalized.reserve(71U);
    for (const auto character : digits) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return true;
}

} // namespace

VfsAssetReadReceipt read_vfs_asset(
    const VirtualFileSystem& vfs,
    const AssetVfsCatalog& catalog,
    const std::string_view asset_id,
    VfsAssetReadRequest request) {
    if (!catalog.success) {
        return failure("asset-read.catalog-unavailable",
            "The Asset VFS Catalog is not usable: " + catalog.code, catalog, asset_id);
    }
    if (asset_id.empty()) {
        return failure("asset-read.asset-id-empty", "A stable Asset ID is required.", catalog, asset_id);
    }
    if (request.cancelled && request.cancelled()) {
        return failure("asset-read.cancelled", "The asset read was cancelled before lookup.", catalog, asset_id);
    }
    const auto* record = catalog.find(asset_id);
    if (record == nullptr) {
        return failure("asset-read.not-found",
            "The Asset ID is unknown or unavailable in this catalog.", catalog, asset_id);
    }
    if (record->bytes > std::numeric_limits<std::uint64_t>::max()) {
        return failure("asset-read.size-unsupported", "The catalog byte count is not representable.",
            catalog, asset_id, record);
    }

    const auto full_read = request.offset == 0U && request.length == 0U;
    if (request.verify_content_hash && !full_read) {
        return failure("asset-read.integrity-requires-full-read",
            "A byte range cannot prove the whole-asset content hash; request a full read or explicitly disable verification.",
            catalog, asset_id, record);
    }
    std::string expected_hash;
    if (request.verify_content_hash && !normalize_sha256(record->content_hash, expected_hash)) {
        return failure("asset-read.hash-unverifiable",
            "The catalog content hash is not a SHA-256 identity and cannot be verified.",
            catalog, asset_id, record);
    }

    auto read = vfs.read({.uri = record->uri,
                          .offset = request.offset,
                          .length = request.length,
                          .byte_budget = request.byte_budget,
                          .cancelled = std::move(request.cancelled)});
    if (!read.success) {
        auto receipt = failure("asset-read.vfs-failed", read.code + ": " + read.detail,
            catalog, asset_id, record);
        receipt.offset = request.offset;
        receipt.mount_revision = read.file.mount_revision;
        receipt.total_bytes = read.file.total_bytes;
        return receipt;
    }
    if (record->bytes != 0U && read.file.total_bytes != static_cast<std::uint64_t>(record->bytes)) {
        auto receipt = failure("asset-read.size-mismatch",
            "The mounted file size does not match the Asset Catalog metadata.", catalog, asset_id, record);
        receipt.mount_revision = read.file.mount_revision;
        receipt.total_bytes = read.file.total_bytes;
        receipt.offset = read.offset;
        receipt.observed_content_hash = read.sha256;
        return receipt;
    }

    std::string observed_hash;
    if (request.verify_content_hash &&
        (!normalize_sha256(read.sha256, observed_hash) || observed_hash != expected_hash)) {
        auto receipt = failure("asset-read.hash-mismatch",
            "The mounted bytes do not match the Asset Catalog content hash.", catalog, asset_id, record);
        receipt.mount_revision = read.file.mount_revision;
        receipt.total_bytes = read.file.total_bytes;
        receipt.offset = read.offset;
        receipt.eof = read.eof;
        receipt.observed_content_hash = read.sha256;
        return receipt;
    }

    return {.success = true,
            .code = "ok",
            .detail = request.verify_content_hash
                ? "The complete asset was read and its catalog content hash was verified."
                : "The requested asset byte range was read; whole-asset hash verification was not requested.",
            .asset_id = record->asset_id,
            .uri = record->uri,
            .kind = record->kind,
            .mount_id = read.file.mount_id,
            .expected_content_hash = record->content_hash,
            .observed_content_hash = read.sha256,
            .content_hash_verified = request.verify_content_hash,
            .catalog_revision = catalog.registry_revision,
            .mount_revision = read.file.mount_revision,
            .total_bytes = read.file.total_bytes,
            .offset = read.offset,
            .eof = read.eof,
            .bytes = std::move(read.bytes)};
}

} // namespace noemancer
