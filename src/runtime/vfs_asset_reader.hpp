#pragma once

#include "runtime/asset_vfs_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct VfsAssetReadRequest final {
    std::uint64_t offset{};
    // Zero means read to EOF. A full read is offset == 0 and length == 0.
    std::size_t length{};
    std::size_t byte_budget{64U * 1024U * 1024U};
    // Full reads validate the Asset Registry content identity by default.
    // A range cannot prove a whole-file hash, so callers must explicitly opt
    // out when feeding a streaming decoder.
    bool verify_content_hash{true};
    std::function<bool()> cancelled;
};

struct VfsAssetReadReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::string asset_id;
    std::string uri;
    std::string kind;
    std::string mount_id;
    std::string expected_content_hash;
    std::string observed_content_hash;
    bool content_hash_verified{};
    std::uint64_t catalog_revision{};
    std::uint64_t mount_revision{};
    std::uint64_t total_bytes{};
    std::uint64_t offset{};
    bool eof{};
    std::vector<std::byte> bytes;
};

// Runtime-private Asset ID -> VFS byte boundary. It deliberately exposes no
// filesystem path and performs no codec-specific parsing. Cooked geometry,
// animation, KTX2 and future archive adapters can share this contract.
[[nodiscard]] VfsAssetReadReceipt read_vfs_asset(
    const VirtualFileSystem& vfs,
    const AssetVfsCatalog& catalog,
    std::string_view asset_id,
    VfsAssetReadRequest request = {});

} // namespace noemancer
