#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// The descriptor is intentionally a plain-data discovery record.  It carries
// no bearer token or secret; credential_file is only a bounded, root-relative
// reference to a separately managed local credential sidecar.
struct LiveEditorSessionDescriptor final {
    std::string schema_version{"noemancer.live-editor-session/0.1"};
    std::uint32_t version{1U};
    std::string session_id;
    std::uint64_t process_id{};
    std::string process_identity;
    std::string project_id;
    std::string project_name;
    std::string project_root;
    std::string endpoint;
    std::string credential_file;
    std::vector<std::string> capabilities;
    std::uint64_t created_unix_milliseconds{};
    std::uint64_t heartbeat_unix_milliseconds{};
    std::uint64_t revision{};
};

struct LiveEditorSessionDiagnostic final {
    std::string code;
    std::string detail;
    std::string path;
};

struct LiveEditorSessionWriteReceipt final {
    bool success{};
    bool atomic{};
    bool descriptor_removed{};
    bool credential_removed{};
    std::string code;
    std::string detail;
    std::string descriptor_path;
    std::uint64_t revision{};
};

struct LiveEditorSessionDiscovery final {
    bool success{};
    std::string code;
    std::string detail;
    std::vector<LiveEditorSessionDescriptor> sessions;
    std::vector<LiveEditorSessionDiagnostic> diagnostics;
    std::size_t stale_removed{};
};

struct LiveEditorSessionStoreOptions final {
    // Empty selects %LOCALAPPDATA%/Noemancer/Sessions on Windows.  Tests and
    // embedded hosts should set an isolated absolute directory here.
    std::filesystem::path root_override;
    std::uint64_t now_unix_milliseconds{};
    std::uint64_t stale_after_milliseconds{120'000U};
    std::size_t max_discovered_sessions{128U};
    std::size_t max_scan_entries{256U};
};

class LiveEditorSessionStore final {
public:
    explicit LiveEditorSessionStore(LiveEditorSessionStoreOptions options = {});

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // Publish refuses to replace an existing session file.  The descriptor is
    // normalized only for omitted revision/timestamps; its serialized bytes
    // are published through one bounded atomic replacement.
    [[nodiscard]] LiveEditorSessionWriteReceipt publish(
        LiveEditorSessionDescriptor descriptor);

    // Refresh is an owner- and revision-checked heartbeat update.  It keeps
    // all identity/endpoint/capability fields from the published descriptor,
    // advances revision by one, and writes a fresh heartbeat.
    [[nodiscard]] LiveEditorSessionWriteReceipt refresh(
        std::string_view session_id,
        std::string_view process_identity,
        std::uint64_t expected_revision);

    // Revoke requires the same process identity and revision check.  A
    // credential sidecar, when referenced, is removed with the descriptor;
    // the descriptor is always removed even if sidecar cleanup reports an
    // error, so a stale discovery record cannot retain an active endpoint.
    [[nodiscard]] LiveEditorSessionWriteReceipt revoke(
        std::string_view session_id,
        std::string_view process_identity,
        std::uint64_t expected_revision);

    // Discovery is bounded and deterministic.  Valid stale descriptors are
    // removed (including their credential sidecar); malformed or oversized
    // records are ignored with bounded diagnostics.
    [[nodiscard]] LiveEditorSessionDiscovery discover();

    [[nodiscard]] static std::string descriptor_json(
        const LiveEditorSessionDescriptor& descriptor);

    [[nodiscard]] static std::uint64_t current_process_id() noexcept;
    [[nodiscard]] static std::string current_process_identity();

    static constexpr std::size_t max_descriptor_bytes() noexcept { return 64U * 1024U; }
    static constexpr std::size_t max_capabilities() noexcept { return 64U; }
    static constexpr std::size_t max_capability_bytes() noexcept { return 96U; }
    static constexpr std::size_t max_diagnostics() noexcept { return 128U; }
    static constexpr std::size_t max_scan_entries() noexcept { return 512U; }

private:
    LiveEditorSessionStoreOptions options_;
    std::filesystem::path root_;
};

} // namespace noemancer
