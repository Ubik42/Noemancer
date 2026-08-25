#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

enum class VfsMountKind : std::uint8_t {
    directory,
    package_directory
};

struct VfsLimits final {
    std::size_t max_mounts{64U};
    std::size_t max_uri_bytes{1024U};
    std::size_t max_read_bytes{64U * 1024U * 1024U};
    std::size_t max_observation_bytes{64U * 1024U};
};

struct VfsMountSpec final {
    std::string id;
    std::string virtual_root;
    std::filesystem::path source_root;
    VfsMountKind kind{VfsMountKind::directory};
    std::int32_t priority{};
    bool read_only{true};
};

struct VfsMountReceipt final {
    bool success{};
    bool changed{};
    std::string code;
    std::string detail;
    std::uint64_t revision_before{};
    std::uint64_t revision_after{};
    std::string mount_id;
};

struct VfsFileHandle final {
    std::string uri;
    std::string mount_id;
    std::string relative_path;
    std::uint64_t mount_revision{};
    std::uint64_t total_bytes{};
};

struct VfsStatResult final {
    bool success{};
    std::string code;
    std::string detail;
    VfsFileHandle file;
};

struct VfsReadRequest final {
    std::string uri;
    std::uint64_t offset{};
    // Zero means read to EOF, still bounded by byte_budget and global limits.
    std::size_t length{};
    std::size_t byte_budget{64U * 1024U * 1024U};
    // Called before opening and between bounded read chunks. Returning true
    // cancels without publishing partial bytes.
    std::function<bool()> cancelled;
};

struct VfsReadResult final {
    bool success{};
    std::string code;
    std::string detail;
    VfsFileHandle file;
    std::uint64_t offset{};
    bool eof{};
    std::string sha256;
    std::vector<std::byte> bytes;
};

// Thread-safe, read-oriented mount authority. Public identities and requests
// are plain data; filesystem handles and platform-specific state stay private.
// Directory and current package-directory mounts intentionally share this
// contract so a future archive adapter does not leak into Scene/RPC formats.
class VirtualFileSystem final {
public:
    explicit VirtualFileSystem(VfsLimits limits = {});
    ~VirtualFileSystem();
    VirtualFileSystem(const VirtualFileSystem&) = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;
    VirtualFileSystem(VirtualFileSystem&&) noexcept;
    VirtualFileSystem& operator=(VirtualFileSystem&&) noexcept;

    [[nodiscard]] VfsMountReceipt mount(VfsMountSpec spec);
    [[nodiscard]] VfsMountReceipt unmount(std::string_view mount_id);
    [[nodiscard]] VfsStatResult stat(std::string_view uri) const;
    [[nodiscard]] VfsReadResult read(const VfsReadRequest& request) const;
    [[nodiscard]] std::string observation_json() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const VfsLimits& limits() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view vfs_mount_kind_name(VfsMountKind kind) noexcept;

} // namespace noemancer
