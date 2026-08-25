#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <miniaudio.h>

#include "engine/virtual_file_system.hpp"

namespace noemancer {

// Runtime-private limits for the miniaudio bridge. Files at or below the
// resident threshold are snapshotted on open; larger files use bounded VFS
// range reads so packaged audio does not need to be copied to a disk path.
struct MiniaudioVfsLimits final {
    std::size_t max_open_handles{256U};
    std::size_t max_resident_file_bytes{2U * 1024U * 1024U};
    std::size_t max_read_bytes_per_call{4U * 1024U * 1024U};
};

enum class MiniaudioVfsStatusCode : std::uint8_t {
    ok,
    at_end,
    invalid_argument,
    invalid_uri,
    read_only,
    not_found,
    handle_budget_exceeded,
    closed_handle,
    stale_file,
    io_error,
    unsupported
};

struct MiniaudioVfsStatus final {
    MiniaudioVfsStatusCode code{MiniaudioVfsStatusCode::ok};
    ma_result result{MA_SUCCESS};
    std::string vfs_code;
    std::string detail;
};

// Adapts Noemancer's URI-based read authority to miniaudio's callback ABI.
// The adapter must outlive every decoder/resource manager that receives
// native_vfs(); those consumers must be destroyed before the adapter or VFS.
class MiniaudioVfsAdapter final {
public:
    explicit MiniaudioVfsAdapter(VirtualFileSystem& vfs, MiniaudioVfsLimits limits = {});
    ~MiniaudioVfsAdapter();
    MiniaudioVfsAdapter(const MiniaudioVfsAdapter&) = delete;
    MiniaudioVfsAdapter& operator=(const MiniaudioVfsAdapter&) = delete;
    MiniaudioVfsAdapter(MiniaudioVfsAdapter&&) = delete;
    MiniaudioVfsAdapter& operator=(MiniaudioVfsAdapter&&) = delete;

    // Assign directly to ma_resource_manager_config::pVFS or a decoder
    // configuration. No ownership is transferred.
    [[nodiscard]] ma_vfs* native_vfs() noexcept;
    [[nodiscard]] MiniaudioVfsStatus last_status() const;
    [[nodiscard]] std::size_t active_handles() const noexcept;
    [[nodiscard]] std::size_t resident_handles() const noexcept;
    [[nodiscard]] std::size_t stream_handles() const noexcept;
    [[nodiscard]] const MiniaudioVfsLimits& limits() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view miniaudio_vfs_status_name(MiniaudioVfsStatusCode code) noexcept;

} // namespace noemancer
