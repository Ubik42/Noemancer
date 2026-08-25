#include "engine/virtual_file_system.hpp"

#include "engine/content_hash.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

#include <nlohmann/json.hpp>

namespace noemancer {
namespace {

using Json = nlohmann::json;

struct ParsedUri final {
    std::string canonical;
    std::string scheme_root;
};

[[nodiscard]] bool valid_scheme(const std::string_view scheme) noexcept {
    if (scheme.empty() || scheme.size() > 32U || scheme.front() < 'a' || scheme.front() > 'z') return false;
    return std::ranges::all_of(scheme.substr(1U), [](const char value) {
        return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
            value == '+' || value == '-' || value == '.';
    });
}

[[nodiscard]] std::optional<ParsedUri> parse_uri(
    const std::string_view value, const std::size_t max_bytes, const bool allow_root) {
    if (value.empty() || value.size() > max_bytes) return std::nullopt;
    if (!allow_root && value.ends_with('/')) return std::nullopt;
    const auto separator = value.find("://");
    if (separator == std::string_view::npos || !valid_scheme(value.substr(0U, separator))) return std::nullopt;
    if (value.find("://", separator + 3U) != std::string_view::npos) return std::nullopt;

    auto path = value.substr(separator + 3U);
    if (!path.empty() && path.back() == '/') path.remove_suffix(1U);
    if ((!allow_root && path.empty()) || (!path.empty() && (path.front() == '/' || path.back() == '/')))
        return std::nullopt;

    std::size_t begin{};
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto component = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if (component.empty() || component == "." || component == "..") return std::nullopt;
        for (const auto character : component) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20U || character == '\\' || character == ':' || character == '?' ||
                character == '#' || character == '%') return std::nullopt;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }

    ParsedUri parsed;
    parsed.scheme_root.assign(value.substr(0U, separator + 3U));
    parsed.canonical = parsed.scheme_root;
    parsed.canonical.append(path);
    return parsed;
}

[[nodiscard]] bool path_within(
    const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept {
    auto root_iterator = root.begin();
    auto candidate_iterator = candidate.begin();
    for (; root_iterator != root.end(); ++root_iterator, ++candidate_iterator) {
        if (candidate_iterator == candidate.end() || *root_iterator != *candidate_iterator) return false;
    }
    return true;
}

[[nodiscard]] bool valid_mount_id(const std::string_view id) noexcept {
    if (id.empty() || id.size() > 128U) return false;
    return std::ranges::all_of(id, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '.' || character == '_' || character == '-';
    });
}

[[nodiscard]] VfsMountReceipt mount_failure(
    std::string code, std::string detail, const std::uint64_t revision, std::string mount_id = {}) {
    return VfsMountReceipt{
        false, false, std::move(code), std::move(detail), revision, revision, std::move(mount_id)};
}

[[nodiscard]] VfsStatResult stat_failure(std::string code, std::string detail) {
    return VfsStatResult{false, std::move(code), std::move(detail), {}};
}

[[nodiscard]] VfsReadResult read_failure(
    std::string code, std::string detail, VfsFileHandle file = {}, const std::uint64_t offset = 0U) {
    return VfsReadResult{false, std::move(code), std::move(detail), std::move(file), offset, false, {}, {}};
}

} // namespace

struct VirtualFileSystem::Impl final {
    struct Mount final {
        VfsMountSpec spec;
        std::filesystem::path canonical_root;
        std::uint64_t mounted_revision{};
        std::uint64_t ordinal{};
    };

    struct Resolved final {
        Mount mount;
        std::filesystem::path path;
        std::string uri;
        std::string relative_path;
        std::uint64_t size{};
    };

    explicit Impl(VfsLimits value) : configured_limits(std::move(value)) {}

    [[nodiscard]] std::optional<Resolved> resolve(
        const std::string_view uri, VfsStatResult& failure) const {
        const auto parsed = parse_uri(uri, configured_limits.max_uri_bytes, false);
        if (!parsed) {
            failure = stat_failure("vfs.uri-invalid", "The URI is not canonical scheme://path syntax.");
            return std::nullopt;
        }

        std::vector<Mount> snapshot;
        {
            std::shared_lock lock(mutex);
            snapshot = mounts;
        }
        std::ranges::sort(snapshot, [](const Mount& left, const Mount& right) {
            if (left.spec.virtual_root.size() != right.spec.virtual_root.size())
                return left.spec.virtual_root.size() > right.spec.virtual_root.size();
            if (left.spec.priority != right.spec.priority) return left.spec.priority > right.spec.priority;
            return left.ordinal < right.ordinal;
        });

        for (const auto& mount : snapshot) {
            const auto& root = mount.spec.virtual_root;
            if (parsed->canonical != root &&
                !(parsed->canonical.size() > root.size() && parsed->canonical.starts_with(root) &&
                  (root.ends_with("://") || parsed->canonical[root.size()] == '/'))) continue;

            auto relative = std::string_view(parsed->canonical).substr(root.size());
            if (!relative.empty() && relative.front() == '/') relative.remove_prefix(1U);
            if (relative.empty()) continue;

            std::error_code error;
            const auto candidate = std::filesystem::weakly_canonical(
                mount.canonical_root / std::filesystem::path(relative), error);
            if (error) continue;
            if (!path_within(mount.canonical_root, candidate)) {
                failure = stat_failure(
                    "vfs.path-escape", "The resolved path leaves its mounted source root.");
                return std::nullopt;
            }
            if (!std::filesystem::is_regular_file(candidate, error) || error) continue;
            const auto byte_count = std::filesystem::file_size(candidate, error);
            if (error || byte_count > std::numeric_limits<std::uint64_t>::max()) {
                failure = stat_failure("vfs.stat-failed", "The mounted file metadata could not be read.");
                return std::nullopt;
            }
            return Resolved{
                mount, candidate, parsed->canonical, std::string(relative), static_cast<std::uint64_t>(byte_count)};
        }

        failure = stat_failure("vfs.not-found", "No mounted regular file resolves the URI.");
        return std::nullopt;
    }

    VfsLimits configured_limits;
    mutable std::shared_mutex mutex;
    std::vector<Mount> mounts;
    std::uint64_t authority_revision{};
    std::uint64_t next_ordinal{};
};

VirtualFileSystem::VirtualFileSystem(VfsLimits limits) : impl_(std::make_unique<Impl>(std::move(limits))) {}
VirtualFileSystem::~VirtualFileSystem() = default;
VirtualFileSystem::VirtualFileSystem(VirtualFileSystem&&) noexcept = default;
VirtualFileSystem& VirtualFileSystem::operator=(VirtualFileSystem&&) noexcept = default;

VfsMountReceipt VirtualFileSystem::mount(VfsMountSpec spec) {
    if (!impl_) return mount_failure("vfs.moved-from", "The VFS no longer owns an implementation.", 0U);
    const auto parsed_root = parse_uri(spec.virtual_root, impl_->configured_limits.max_uri_bytes, true);
    if (!valid_mount_id(spec.id)) {
        return mount_failure("vfs.mount-id-invalid", "Mount IDs use 1-128 alphanumeric, dot, underscore or dash characters.",
            revision(), std::move(spec.id));
    }
    if (!parsed_root) {
        return mount_failure("vfs.virtual-root-invalid", "The virtual root is not canonical scheme://path syntax.",
            revision(), std::move(spec.id));
    }
    if (spec.kind != VfsMountKind::directory && spec.kind != VfsMountKind::package_directory) {
        return mount_failure("vfs.mount-kind-invalid", "The mount kind is not supported by this VFS contract.",
            revision(), std::move(spec.id));
    }
    spec.virtual_root = parsed_root->canonical;
    if (spec.source_root.empty()) {
        return mount_failure("vfs.source-root-invalid", "The source root is empty.", revision(), std::move(spec.id));
    }
    std::error_code error;
    const auto canonical_root = std::filesystem::canonical(spec.source_root, error);
    if (error || !std::filesystem::is_directory(canonical_root, error) || error) {
        return mount_failure("vfs.source-root-invalid", "The source root is not an accessible directory.",
            revision(), std::move(spec.id));
    }

    std::unique_lock lock(impl_->mutex);
    const auto before = impl_->authority_revision;
    if (impl_->mounts.size() >= impl_->configured_limits.max_mounts) {
        return mount_failure("vfs.mount-limit", "The configured mount limit has been reached.", before, std::move(spec.id));
    }
    if (std::ranges::any_of(impl_->mounts, [&](const Impl::Mount& mount) { return mount.spec.id == spec.id; })) {
        return mount_failure("vfs.mount-id-conflict", "A mount with this stable ID already exists.", before, std::move(spec.id));
    }
    ++impl_->authority_revision;
    impl_->mounts.push_back(Impl::Mount{
        std::move(spec), canonical_root, impl_->authority_revision, impl_->next_ordinal++});
    return VfsMountReceipt{
        true, true, "ok", "The source directory was mounted.", before, impl_->authority_revision,
        impl_->mounts.back().spec.id};
}

VfsMountReceipt VirtualFileSystem::unmount(const std::string_view mount_id) {
    if (!impl_) return mount_failure("vfs.moved-from", "The VFS no longer owns an implementation.", 0U);
    std::unique_lock lock(impl_->mutex);
    const auto before = impl_->authority_revision;
    const auto iterator = std::ranges::find_if(
        impl_->mounts, [&](const Impl::Mount& mount) { return mount.spec.id == mount_id; });
    if (iterator == impl_->mounts.end()) {
        return mount_failure("vfs.mount-not-found", "No mount has this stable ID.", before, std::string(mount_id));
    }
    const auto id = iterator->spec.id;
    impl_->mounts.erase(iterator);
    ++impl_->authority_revision;
    return VfsMountReceipt{
        true, true, "ok", "The mount was removed.", before, impl_->authority_revision, id};
}

VfsStatResult VirtualFileSystem::stat(const std::string_view uri) const {
    if (!impl_) return stat_failure("vfs.moved-from", "The VFS no longer owns an implementation.");
    VfsStatResult failure;
    const auto resolved = impl_->resolve(uri, failure);
    if (!resolved) return failure;
    return VfsStatResult{
        true, "ok", "The mounted file was resolved.",
        VfsFileHandle{resolved->uri, resolved->mount.spec.id, resolved->relative_path,
            resolved->mount.mounted_revision, resolved->size}};
}

VfsReadResult VirtualFileSystem::read(const VfsReadRequest& request) const {
    if (!impl_) return read_failure("vfs.moved-from", "The VFS no longer owns an implementation.");
    if (request.cancelled && request.cancelled())
        return read_failure("vfs.cancelled", "The read was cancelled before opening the file.", {}, request.offset);

    VfsStatResult resolve_failure;
    const auto resolved = impl_->resolve(request.uri, resolve_failure);
    if (!resolved) return read_failure(resolve_failure.code, resolve_failure.detail, {}, request.offset);
    VfsFileHandle handle{resolved->uri, resolved->mount.spec.id, resolved->relative_path,
        resolved->mount.mounted_revision, resolved->size};
    if (request.offset > resolved->size)
        return read_failure("vfs.range-invalid", "The range offset is beyond EOF.", std::move(handle), request.offset);

    const auto remaining = resolved->size - request.offset;
    const auto requested = request.length == 0U ? remaining : static_cast<std::uint64_t>(request.length);
    if (requested > remaining)
        return read_failure("vfs.range-invalid", "The requested range extends beyond EOF.", std::move(handle), request.offset);
    const auto budget = std::min(request.byte_budget, impl_->configured_limits.max_read_bytes);
    if (requested > static_cast<std::uint64_t>(budget))
        return read_failure("vfs.read-budget-exceeded", "The requested range exceeds the effective byte budget.",
            std::move(handle), request.offset);
    if (requested > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return read_failure("vfs.read-budget-exceeded", "The requested range cannot fit in memory.",
            std::move(handle), request.offset);
    if (request.offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return read_failure("vfs.range-invalid", "The range offset is not representable by the file backend.",
            std::move(handle), request.offset);
    if (request.cancelled && request.cancelled())
        return read_failure("vfs.cancelled", "The read was cancelled before opening the file.",
            std::move(handle), request.offset);

    std::ifstream input(resolved->path, std::ios::binary);
    if (!input) return read_failure("vfs.open-failed", "The mounted file could not be opened.", std::move(handle), request.offset);
    input.seekg(static_cast<std::streamoff>(request.offset), std::ios::beg);
    if (!input) return read_failure("vfs.seek-failed", "The mounted file range could not be selected.", std::move(handle), request.offset);

    std::vector<std::byte> staged(static_cast<std::size_t>(requested));
    constexpr std::size_t chunk_size = 64U * 1024U;
    std::size_t completed{};
    while (completed < staged.size()) {
        if (request.cancelled && request.cancelled())
            return read_failure("vfs.cancelled", "The read was cancelled without publishing partial bytes.",
                std::move(handle), request.offset);
        const auto chunk = std::min(chunk_size, staged.size() - completed);
        input.read(reinterpret_cast<char*>(staged.data() + completed), static_cast<std::streamsize>(chunk));
        const auto count = input.gcount();
        if (count != static_cast<std::streamsize>(chunk))
            return read_failure("vfs.file-changed", "The mounted file changed or became unreadable during the read.",
                std::move(handle), request.offset);
        completed += chunk;
    }
    if (request.cancelled && request.cancelled())
        return read_failure("vfs.cancelled", "The read was cancelled without publishing partial bytes.",
            std::move(handle), request.offset);

    std::error_code error;
    const auto final_path = std::filesystem::weakly_canonical(resolved->path, error);
    const auto final_size = error ? 0U : std::filesystem::file_size(final_path, error);
    if (error || final_path != resolved->path || final_size != resolved->size)
        return read_failure("vfs.file-changed", "The mounted file identity changed during the read.",
            std::move(handle), request.offset);
    const auto hash = sha256_bytes(staged);
    if (!hash.success) return read_failure(hash.code, hash.detail, std::move(handle), request.offset);
    if (request.cancelled && request.cancelled())
        return read_failure("vfs.cancelled", "The read was cancelled without publishing partial bytes.",
            std::move(handle), request.offset);

    return VfsReadResult{
        true, "ok", "The requested bytes were read.", std::move(handle), request.offset,
        request.offset + requested == resolved->size, hash.value, std::move(staged)};
}

std::string VirtualFileSystem::observation_json() const {
    if (!impl_) return "{}";
    std::vector<Impl::Mount> snapshot;
    std::uint64_t current_revision{};
    {
        std::shared_lock lock(impl_->mutex);
        snapshot = impl_->mounts;
        current_revision = impl_->authority_revision;
    }
    std::ranges::sort(snapshot, [](const Impl::Mount& left, const Impl::Mount& right) {
        if (left.spec.priority != right.spec.priority) return left.spec.priority > right.spec.priority;
        return left.ordinal < right.ordinal;
    });

    Json observation{{"schema", "noemancer.vfs-observation/0.1"}, {"revision", current_revision},
        {"mountCount", snapshot.size()}, {"mounts", Json::array()}, {"truncated", false}};
    for (const auto& mount : snapshot) {
        observation["mounts"].push_back({
            {"id", mount.spec.id}, {"virtualRoot", mount.spec.virtual_root},
            {"kind", vfs_mount_kind_name(mount.spec.kind)}, {"priority", mount.spec.priority},
            {"readOnly", mount.spec.read_only}, {"mountedRevision", mount.mounted_revision}});
        if (observation.dump(-1, ' ', false, Json::error_handler_t::replace).size() >
            impl_->configured_limits.max_observation_bytes) {
            observation["mounts"].erase(observation["mounts"].end() - 1);
            observation["truncated"] = true;
            break;
        }
    }
    auto encoded = observation.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (encoded.size() <= impl_->configured_limits.max_observation_bytes) return encoded;
    encoded = R"({"truncated":true})";
    if (encoded.size() <= impl_->configured_limits.max_observation_bytes) return encoded;
    if (impl_->configured_limits.max_observation_bytes >= 2U) return "{}";
    return {};
}

std::uint64_t VirtualFileSystem::revision() const noexcept {
    if (!impl_) return 0U;
    std::shared_lock lock(impl_->mutex);
    return impl_->authority_revision;
}

const VfsLimits& VirtualFileSystem::limits() const noexcept {
    static const VfsLimits empty_limits{};
    return impl_ ? impl_->configured_limits : empty_limits;
}

std::string_view vfs_mount_kind_name(const VfsMountKind kind) noexcept {
    switch (kind) {
    case VfsMountKind::directory: return "directory";
    case VfsMountKind::package_directory: return "package-directory";
    }
    return "unknown";
}

} // namespace noemancer
