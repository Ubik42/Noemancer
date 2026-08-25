#include "runtime/miniaudio_vfs_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

struct OpenFile final {
    std::mutex mutex;
    std::string uri;
    std::string mount_id;
    std::uint64_t mount_revision{};
    std::uint64_t size{};
    std::uint64_t cursor{};
    bool resident{};
    bool closed{};
    std::vector<std::byte> bytes;
};

[[nodiscard]] ma_result result_for_status(const MiniaudioVfsStatusCode code) noexcept {
    switch (code) {
    case MiniaudioVfsStatusCode::ok: return MA_SUCCESS;
    case MiniaudioVfsStatusCode::at_end: return MA_AT_END;
    case MiniaudioVfsStatusCode::invalid_argument: return MA_INVALID_ARGS;
    case MiniaudioVfsStatusCode::invalid_uri: return MA_INVALID_FILE;
    case MiniaudioVfsStatusCode::read_only: return MA_ACCESS_DENIED;
    case MiniaudioVfsStatusCode::not_found: return MA_DOES_NOT_EXIST;
    case MiniaudioVfsStatusCode::handle_budget_exceeded: return MA_TOO_MANY_OPEN_FILES;
    case MiniaudioVfsStatusCode::closed_handle: return MA_INVALID_OPERATION;
    case MiniaudioVfsStatusCode::stale_file: return MA_INVALID_OPERATION;
    case MiniaudioVfsStatusCode::io_error: return MA_IO_ERROR;
    case MiniaudioVfsStatusCode::unsupported: return MA_NOT_IMPLEMENTED;
    }
    return MA_ERROR;
}

[[nodiscard]] MiniaudioVfsStatusCode status_for_vfs_code(const std::string_view code) noexcept {
    if (code == "vfs.uri-invalid" || code == "vfs.path-escape" || code == "vfs.virtual-root-invalid") {
        return MiniaudioVfsStatusCode::invalid_uri;
    }
    if (code == "vfs.not-found" || code == "vfs.mount-not-found") {
        return MiniaudioVfsStatusCode::not_found;
    }
    if (code == "vfs.file-changed") {
        return MiniaudioVfsStatusCode::stale_file;
    }
    if (code == "vfs.read-budget-exceeded" || code == "vfs.range-invalid") {
        return MiniaudioVfsStatusCode::io_error;
    }
    if (code == "vfs.cancelled") {
        return MiniaudioVfsStatusCode::io_error;
    }
    return MiniaudioVfsStatusCode::io_error;
}

[[nodiscard]] bool has_asset_scheme(const std::string_view uri) noexcept {
    return uri.starts_with("asset://") && uri.size() > 8U;
}

} // namespace

struct MiniaudioVfsAdapter::Impl final {
    struct CallbackBridge final {
        // Must remain first: miniaudio treats the VFS pointer as a callback table.
        ma_vfs_callbacks callbacks{};
        Impl* owner{};
    } bridge;
    VirtualFileSystem& vfs;
    MiniaudioVfsLimits limits;
    mutable std::mutex handles_mutex;
    std::unordered_map<std::uintptr_t, std::shared_ptr<OpenFile>> handles;
    std::atomic<std::uintptr_t> next_token{1U};
    std::atomic<std::size_t> resident_count{};
    std::atomic<std::size_t> stream_count{};
    mutable std::mutex status_mutex;
    MiniaudioVfsStatus status;

    explicit Impl(VirtualFileSystem& source, MiniaudioVfsLimits configured)
        : vfs(source), limits(configured) {
        if (limits.max_open_handles == 0U) limits.max_open_handles = 1U;
        if (limits.max_read_bytes_per_call == 0U) limits.max_read_bytes_per_call = 1U;
        bridge.owner = this;
        bridge.callbacks.onOpen = &open;
        bridge.callbacks.onOpenW = &open_w;
        bridge.callbacks.onClose = &close;
        bridge.callbacks.onRead = &read;
        bridge.callbacks.onWrite = &write;
        bridge.callbacks.onSeek = &seek;
        bridge.callbacks.onTell = &tell;
        bridge.callbacks.onInfo = &info;
    }

    ~Impl() {
        std::lock_guard lock(handles_mutex);
        for (auto& [token, file] : handles) {
            (void)token;
            std::lock_guard file_lock(file->mutex);
            file->closed = true;
        }
        handles.clear();
    }

    [[nodiscard]] static Impl& self(ma_vfs* value) noexcept {
        return *reinterpret_cast<CallbackBridge*>(value)->owner;
    }

    void publish(MiniaudioVfsStatusCode code, std::string vfs_code = {}, std::string detail = {}) {
        std::lock_guard lock(status_mutex);
        status = {.code = code, .result = result_for_status(code),
                  .vfs_code = std::move(vfs_code), .detail = std::move(detail)};
    }

    [[nodiscard]] std::shared_ptr<OpenFile> find(const ma_vfs_file opaque) {
        const auto token = reinterpret_cast<std::uintptr_t>(opaque);
        std::lock_guard lock(handles_mutex);
        const auto it = handles.find(token);
        return it == handles.end() ? nullptr : it->second;
    }

    [[nodiscard]] static ma_result open(ma_vfs* opaque, const char* path,
                                        ma_uint32 mode, ma_vfs_file* out_file) {
        auto& adapter = self(opaque);
        if (out_file == nullptr || path == nullptr || mode == 0U) {
            adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "open requires a path and output handle");
            return MA_INVALID_ARGS;
        }
        *out_file = nullptr;
        if ((mode & MA_OPEN_MODE_WRITE) != 0U || (mode & MA_OPEN_MODE_READ) == 0U) {
            adapter.publish(MiniaudioVfsStatusCode::read_only, {}, "Noemancer package VFS is read-only");
            return MA_ACCESS_DENIED;
        }

        const std::string uri(path);
        if (!has_asset_scheme(uri)) {
            adapter.publish(MiniaudioVfsStatusCode::invalid_uri, "vfs.uri.invalid", "audio source must use asset://");
            return MA_INVALID_FILE;
        }

        {
            std::lock_guard lock(adapter.handles_mutex);
            if (adapter.handles.size() >= adapter.limits.max_open_handles) {
                adapter.publish(MiniaudioVfsStatusCode::handle_budget_exceeded, {}, "open audio handle budget exhausted");
                return MA_TOO_MANY_OPEN_FILES;
            }
        }

        const auto stat = adapter.vfs.stat(uri);
        if (!stat.success) {
            const auto code = status_for_vfs_code(stat.code);
            adapter.publish(code, stat.code, stat.detail);
            return result_for_status(code);
        }

        auto file = std::make_shared<OpenFile>();
        file->uri = uri;
        file->mount_id = stat.file.mount_id;
        file->mount_revision = stat.file.mount_revision;
        file->size = stat.file.total_bytes;
        file->resident = stat.file.total_bytes <= adapter.limits.max_resident_file_bytes &&
            stat.file.total_bytes <= adapter.vfs.limits().max_read_bytes;

        if (file->resident && file->size > 0U) {
            if (file->size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                adapter.publish(MiniaudioVfsStatusCode::io_error, "vfs.read.too-large", "resident audio exceeds addressable memory");
                return MA_IO_ERROR;
            }
            const auto length = static_cast<std::size_t>(file->size);
            const auto loaded = adapter.vfs.read(VfsReadRequest{
                .uri = uri, .offset = 0U, .length = length, .byte_budget = length});
            if (!loaded.success || loaded.bytes.size() != length ||
                loaded.file.mount_revision != file->mount_revision || loaded.file.mount_id != file->mount_id) {
                const auto code = loaded.success ? MiniaudioVfsStatusCode::stale_file : status_for_vfs_code(loaded.code);
                adapter.publish(code, loaded.code,
                                loaded.success ? "audio source changed while opening" : loaded.detail);
                return result_for_status(code);
            }
            file->bytes = loaded.bytes;
        }

        std::uintptr_t token{};
        {
            std::lock_guard lock(adapter.handles_mutex);
            // Recheck after I/O so concurrent opens cannot overrun the budget.
            if (adapter.handles.size() >= adapter.limits.max_open_handles) {
                adapter.publish(MiniaudioVfsStatusCode::handle_budget_exceeded, {}, "open audio handle budget exhausted");
                return MA_TOO_MANY_OPEN_FILES;
            }
            token = adapter.next_token.fetch_add(1U, std::memory_order_relaxed);
            // Tokens are never recycled, so a stale ma_vfs_file can never
            // alias a later open. Exhaustion is practically unreachable on
            // 64-bit hosts, but fail closed instead of wrapping on 32-bit.
            if (token == 0U || adapter.handles.contains(token)) {
                adapter.publish(MiniaudioVfsStatusCode::handle_budget_exceeded, {},
                                "audio handle identity space is exhausted");
                return MA_TOO_MANY_OPEN_FILES;
            }
            adapter.handles.emplace(token, file);
        }
        if (file->resident) adapter.resident_count.fetch_add(1U, std::memory_order_relaxed);
        else adapter.stream_count.fetch_add(1U, std::memory_order_relaxed);
        *out_file = reinterpret_cast<ma_vfs_file>(token);
        adapter.publish(MiniaudioVfsStatusCode::ok);
        return MA_SUCCESS;
    }

    [[nodiscard]] static ma_result open_w(ma_vfs* opaque, const wchar_t*, ma_uint32, ma_vfs_file* out_file) {
        auto& adapter = self(opaque);
        if (out_file != nullptr) *out_file = nullptr;
        adapter.publish(MiniaudioVfsStatusCode::unsupported, {}, "wide paths are not part of the asset URI contract");
        return MA_NOT_IMPLEMENTED;
    }

    [[nodiscard]] static ma_result close(ma_vfs* opaque, const ma_vfs_file handle) {
        auto& adapter = self(opaque);
        const auto token = reinterpret_cast<std::uintptr_t>(handle);
        std::shared_ptr<OpenFile> file;
        {
            std::lock_guard lock(adapter.handles_mutex);
            const auto it = adapter.handles.find(token);
            if (it == adapter.handles.end()) {
                adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle is closed or foreign");
                return MA_INVALID_OPERATION;
            }
            file = std::move(it->second);
            adapter.handles.erase(it);
        }
        {
            std::lock_guard lock(file->mutex);
            file->closed = true;
        }
        if (file->resident) adapter.resident_count.fetch_sub(1U, std::memory_order_relaxed);
        else adapter.stream_count.fetch_sub(1U, std::memory_order_relaxed);
        adapter.publish(MiniaudioVfsStatusCode::ok);
        return MA_SUCCESS;
    }

    [[nodiscard]] static ma_result read(ma_vfs* opaque, const ma_vfs_file handle, void* destination,
                                        const size_t requested, size_t* bytes_read) {
        auto& adapter = self(opaque);
        if (bytes_read != nullptr) *bytes_read = 0U;
        if (destination == nullptr) {
            adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "read destination is null");
            return MA_INVALID_ARGS;
        }
        const auto file = adapter.find(handle);
        if (!file) {
            adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle is closed or foreign");
            return MA_INVALID_OPERATION;
        }
        std::lock_guard file_lock(file->mutex);
        if (file->closed) {
            adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle was closed concurrently");
            return MA_INVALID_OPERATION;
        }
        if (requested == 0U) {
            adapter.publish(MiniaudioVfsStatusCode::ok);
            return MA_SUCCESS;
        }
        if (file->cursor >= file->size) {
            adapter.publish(MiniaudioVfsStatusCode::at_end);
            return MA_AT_END;
        }

        const auto available64 = file->size - file->cursor;
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(
            available64, std::numeric_limits<std::size_t>::max()));
        const auto count = std::min({requested, available, adapter.limits.max_read_bytes_per_call});
        if (file->resident) {
            std::memcpy(destination, file->bytes.data() + static_cast<std::size_t>(file->cursor), count);
        } else {
            const auto loaded = adapter.vfs.read(VfsReadRequest{
                .uri = file->uri, .offset = file->cursor, .length = count, .byte_budget = count});
            if (!loaded.success) {
                const auto code = status_for_vfs_code(loaded.code);
                adapter.publish(code, loaded.code, loaded.detail);
                return result_for_status(code);
            }
            if (loaded.file.mount_revision != file->mount_revision || loaded.file.mount_id != file->mount_id) {
                adapter.publish(MiniaudioVfsStatusCode::stale_file, {}, "streaming source mount changed after open");
                return MA_INVALID_OPERATION;
            }
            if (loaded.bytes.empty()) {
                adapter.publish(MiniaudioVfsStatusCode::at_end);
                return MA_AT_END;
            }
            std::memcpy(destination, loaded.bytes.data(), loaded.bytes.size());
            if (bytes_read != nullptr) *bytes_read = loaded.bytes.size();
            file->cursor += loaded.bytes.size();
            adapter.publish(MiniaudioVfsStatusCode::ok);
            return MA_SUCCESS;
        }
        file->cursor += count;
        if (bytes_read != nullptr) *bytes_read = count;
        adapter.publish(MiniaudioVfsStatusCode::ok);
        return MA_SUCCESS;
    }

    [[nodiscard]] static ma_result write(ma_vfs* opaque, ma_vfs_file, const void*, size_t, size_t* bytes_written) {
        auto& adapter = self(opaque);
        if (bytes_written != nullptr) *bytes_written = 0U;
        adapter.publish(MiniaudioVfsStatusCode::read_only, {}, "Noemancer package VFS is read-only");
        return MA_ACCESS_DENIED;
    }

    [[nodiscard]] static ma_result seek(ma_vfs* opaque, const ma_vfs_file handle,
                                        const ma_int64 offset, const ma_seek_origin origin) {
        auto& adapter = self(opaque);
        const auto file = adapter.find(handle);
        if (!file) {
            adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle is closed or foreign");
            return MA_INVALID_OPERATION;
        }
        std::lock_guard file_lock(file->mutex);
        if (file->closed) {
            adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle was closed concurrently");
            return MA_INVALID_OPERATION;
        }

        std::uint64_t base{};
        switch (origin) {
        case ma_seek_origin_start: base = 0U; break;
        case ma_seek_origin_current: base = file->cursor; break;
        case ma_seek_origin_end: base = file->size; break;
        default:
            adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "unknown seek origin");
            return MA_INVALID_ARGS;
        }
        std::uint64_t next{};
        if (offset >= 0) {
            const auto distance = static_cast<std::uint64_t>(offset);
            if (distance > file->size - std::min(base, file->size)) {
                adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "seek exceeds audio source bounds");
                return MA_INVALID_ARGS;
            }
            next = base + distance;
        } else {
            const auto distance = static_cast<std::uint64_t>(-(offset + 1)) + 1U;
            if (distance > base) {
                adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "seek precedes audio source start");
                return MA_INVALID_ARGS;
            }
            next = base - distance;
        }
        if (next > file->size) {
            adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "seek exceeds audio source bounds");
            return MA_INVALID_ARGS;
        }
        file->cursor = next;
        adapter.publish(MiniaudioVfsStatusCode::ok);
        return MA_SUCCESS;
    }

    [[nodiscard]] static ma_result tell(ma_vfs* opaque, const ma_vfs_file handle, ma_int64* cursor) {
        auto& adapter = self(opaque);
        if (cursor == nullptr) {
            adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "tell output is null");
            return MA_INVALID_ARGS;
        }
        const auto file = adapter.find(handle);
        if (!file) {
            adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle is closed or foreign");
            return MA_INVALID_OPERATION;
        }
        std::lock_guard file_lock(file->mutex);
        if (file->closed || file->cursor > static_cast<std::uint64_t>(std::numeric_limits<ma_int64>::max())) {
            adapter.publish(file->closed ? MiniaudioVfsStatusCode::closed_handle : MiniaudioVfsStatusCode::io_error,
                            {}, "audio cursor is unavailable");
            return file->closed ? MA_INVALID_OPERATION : MA_IO_ERROR;
        }
        *cursor = static_cast<ma_int64>(file->cursor);
        adapter.publish(MiniaudioVfsStatusCode::ok);
        return MA_SUCCESS;
    }

    [[nodiscard]] static ma_result info(ma_vfs* opaque, const ma_vfs_file handle, ma_file_info* out_info) {
        auto& adapter = self(opaque);
        if (out_info == nullptr) {
            adapter.publish(MiniaudioVfsStatusCode::invalid_argument, {}, "file info output is null");
            return MA_INVALID_ARGS;
        }
        const auto file = adapter.find(handle);
        if (!file) {
            adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle is closed or foreign");
            return MA_INVALID_OPERATION;
        }
        std::lock_guard file_lock(file->mutex);
        if (file->closed) {
            adapter.publish(MiniaudioVfsStatusCode::closed_handle, {}, "audio handle was closed concurrently");
            return MA_INVALID_OPERATION;
        }
        out_info->sizeInBytes = file->size;
        adapter.publish(MiniaudioVfsStatusCode::ok);
        return MA_SUCCESS;
    }
};

MiniaudioVfsAdapter::MiniaudioVfsAdapter(VirtualFileSystem& vfs, MiniaudioVfsLimits limits)
    : impl_(std::make_unique<Impl>(vfs, limits)) {}

MiniaudioVfsAdapter::~MiniaudioVfsAdapter() = default;

ma_vfs* MiniaudioVfsAdapter::native_vfs() noexcept {
    return reinterpret_cast<ma_vfs*>(&impl_->bridge);
}

MiniaudioVfsStatus MiniaudioVfsAdapter::last_status() const {
    std::lock_guard lock(impl_->status_mutex);
    return impl_->status;
}

std::size_t MiniaudioVfsAdapter::active_handles() const noexcept {
    std::lock_guard lock(impl_->handles_mutex);
    return impl_->handles.size();
}

std::size_t MiniaudioVfsAdapter::resident_handles() const noexcept {
    return impl_->resident_count.load(std::memory_order_relaxed);
}

std::size_t MiniaudioVfsAdapter::stream_handles() const noexcept {
    return impl_->stream_count.load(std::memory_order_relaxed);
}

const MiniaudioVfsLimits& MiniaudioVfsAdapter::limits() const noexcept { return impl_->limits; }

std::string_view miniaudio_vfs_status_name(const MiniaudioVfsStatusCode code) noexcept {
    switch (code) {
    case MiniaudioVfsStatusCode::ok: return "ok";
    case MiniaudioVfsStatusCode::at_end: return "at-end";
    case MiniaudioVfsStatusCode::invalid_argument: return "invalid-argument";
    case MiniaudioVfsStatusCode::invalid_uri: return "invalid-uri";
    case MiniaudioVfsStatusCode::read_only: return "read-only";
    case MiniaudioVfsStatusCode::not_found: return "not-found";
    case MiniaudioVfsStatusCode::handle_budget_exceeded: return "handle-budget-exceeded";
    case MiniaudioVfsStatusCode::closed_handle: return "closed-handle";
    case MiniaudioVfsStatusCode::stale_file: return "stale-file";
    case MiniaudioVfsStatusCode::io_error: return "io-error";
    case MiniaudioVfsStatusCode::unsupported: return "unsupported";
    }
    return "unknown";
}

} // namespace noemancer
