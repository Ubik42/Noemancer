#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// One engine-owned temporal contract is shared by TAA, SSR, SSGI and RTGI.
// The renderer may map these values to textures and fences, but those backend
// objects never cross this boundary.
inline constexpr std::string_view temporal_history_schema =
    "noemancer.temporal-history/0.1";
inline constexpr std::size_t temporal_history_consumer_count = 4U;
inline constexpr std::size_t temporal_history_max_identity_bytes = 128U;
inline constexpr std::size_t temporal_history_max_diagnostics = 64U;
inline constexpr std::size_t temporal_history_max_diagnostic_bytes = 512U;
inline constexpr std::uint32_t temporal_history_max_extent = 16'384U;

enum class TemporalHistoryConsumer : std::uint8_t {
    taa = 0U,
    ssr = 1U,
    ssgi = 2U,
    rtgi = 3U,

    Taa = taa,
    Ssr = ssr,
    Ssgi = ssgi,
    Rtgi = rtgi,
};

[[nodiscard]] std::string_view temporal_history_consumer_name(
    TemporalHistoryConsumer consumer) noexcept;
[[nodiscard]] bool temporal_history_consumer_valid(
    TemporalHistoryConsumer consumer) noexcept;
[[nodiscard]] std::optional<TemporalHistoryConsumer>
temporal_history_consumer_from_string(std::string_view value) noexcept;

// Reset reasons are a stable vocabulary, not renderer implementation details.
// A bitmask lets one transition explain all invalidating changes without
// replacing a specific reason with a generic "history invalid" flag.
enum class TemporalHistoryResetReason : std::uint32_t {
    none = 0U,
    first_frame = 1U << 0U,
    frame_discontinuity = 1U << 1U,
    extent_changed = 1U << 2U,
    format_changed = 1U << 3U,
    camera_changed = 1U << 4U,
    projection_changed = 1U << 5U,
    profile_changed = 1U << 6U,
    quality_changed = 1U << 7U,
    manual = 1U << 8U,
    policy_disabled = 1U << 9U,
    output_invalid = 1U << 10U,

    None = none,
    FirstFrame = first_frame,
    FrameDiscontinuity = frame_discontinuity,
    ExtentChanged = extent_changed,
    FormatChanged = format_changed,
    CameraChanged = camera_changed,
    ProjectionChanged = projection_changed,
    ProfileChanged = profile_changed,
    QualityChanged = quality_changed,
    Manual = manual,
    PolicyDisabled = policy_disabled,
    OutputInvalid = output_invalid,
};

using TemporalHistoryResetMask = std::uint32_t;

[[nodiscard]] constexpr TemporalHistoryResetMask temporal_history_reset_mask(
    const TemporalHistoryResetReason reason) noexcept {
    return static_cast<TemporalHistoryResetMask>(reason);
}

[[nodiscard]] constexpr TemporalHistoryResetMask operator|(
    const TemporalHistoryResetReason left,
    const TemporalHistoryResetReason right) noexcept {
    return temporal_history_reset_mask(left) | temporal_history_reset_mask(right);
}

[[nodiscard]] constexpr TemporalHistoryResetMask operator|(
    const TemporalHistoryResetMask left,
    const TemporalHistoryResetReason right) noexcept {
    return left | temporal_history_reset_mask(right);
}

[[nodiscard]] constexpr TemporalHistoryResetMask operator&(
    const TemporalHistoryResetMask left,
    const TemporalHistoryResetReason right) noexcept {
    return left & temporal_history_reset_mask(right);
}

[[nodiscard]] std::vector<std::string> temporal_history_reset_reason_names(
    TemporalHistoryResetMask reasons);
[[nodiscard]] std::string temporal_history_reset_reason_text(
    TemporalHistoryResetMask reasons);

struct TemporalHistoryExtent final {
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] friend constexpr bool operator==(
        const TemporalHistoryExtent&, const TemporalHistoryExtent&) noexcept = default;
};

// Identity fields are intentionally semantic strings.  For example, a
// runtime can use "camera.main" and "perspective:fov=60" without exposing a
// pointer, SDL handle, matrix ABI or RHI enum to the Engine/Agent boundary.
struct TemporalHistoryIdentity final {
    TemporalHistoryExtent extent{};
    std::string format{"rgba16f"};
    std::uint64_t frame_index{};
    std::string camera_identity{"default"};
    std::string projection_identity{"default"};
    std::string profile_identity{"default"};
    std::string quality_identity{"default"};

    [[nodiscard]] friend bool operator==(
        const TemporalHistoryIdentity&, const TemporalHistoryIdentity&) noexcept = default;
};

struct TemporalHistoryDiagnostic final {
    std::string code;
    std::string detail;
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::uint64_t revision{};
};

struct TemporalHistoryRequest final {
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    TemporalHistoryIdentity identity{};
    // Hybrid Pixel and other deterministic profiles can opt out explicitly.
    // Disabled policy never consumes a stale previous frame.
    bool history_enabled{true};
    std::optional<std::uint64_t> expected_revision;
};

struct TemporalHistoryPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string plan_id;
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::uint64_t base_revision{};
    TemporalHistoryIdentity identity{};
    bool history_enabled{true};
    bool use_previous{};
    bool previous_valid{};
    TemporalHistoryResetMask reset_reasons{};
    std::vector<TemporalHistoryDiagnostic> diagnostics;
};

struct TemporalHistoryBeginResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string transaction_id;
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::uint64_t base_revision{};
    bool use_previous{};
    bool previous_valid{};
    TemporalHistoryResetMask reset_reasons{};
    std::vector<TemporalHistoryDiagnostic> diagnostics;
};

struct TemporalHistoryCommitRequest final {
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::string transaction_id;
    std::optional<std::uint64_t> expected_revision;
    // False means the producer did not publish a usable history surface.
    // This is distinct from a policy reset and is retained in diagnostics.
    bool produced_history{true};
    TemporalHistoryResetMask output_reset_reasons{};
};

struct TemporalHistoryCommitResult final {
    bool success{};
    std::string code;
    std::string detail;
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::uint64_t revision{};
    bool current_valid{};
    bool previous_valid{};
    TemporalHistoryResetMask reset_reasons{};
    std::vector<TemporalHistoryDiagnostic> diagnostics;
};

struct TemporalHistoryResetRequest final {
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::optional<std::uint64_t> expected_revision;
    TemporalHistoryResetMask reasons{
        temporal_history_reset_mask(TemporalHistoryResetReason::manual)};
};

struct TemporalHistoryResetResult final {
    bool success{};
    std::string code;
    std::string detail;
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::uint64_t revision{};
    TemporalHistoryResetMask reset_reasons{};
    std::vector<TemporalHistoryDiagnostic> diagnostics;
};

enum class TemporalHistoryLifecycle : std::uint8_t {
    idle,
    begun,

    Idle = idle,
    Begun = begun,
};

struct TemporalHistoryState final {
    TemporalHistoryConsumer consumer{TemporalHistoryConsumer::taa};
    std::uint64_t revision{};
    TemporalHistoryLifecycle lifecycle{TemporalHistoryLifecycle::idle};
    bool current_valid{};
    bool previous_valid{};
    TemporalHistoryIdentity current{};
    TemporalHistoryIdentity previous{};
    TemporalHistoryResetMask last_reset_reasons{};
    std::uint64_t commit_count{};
    std::uint64_t reset_count{};
    std::string active_transaction_id;
};

// Main-thread domain authority.  A separate slot is kept for every consumer;
// resetting SSR therefore cannot invalidate TAA, and a failed SSGI pass does
// not consume RTGI's history.  Renderer adapters can copy State as plain data.
class TemporalHistoryAuthority final {
public:
    TemporalHistoryAuthority();

    [[nodiscard]] TemporalHistoryPlan plan(const TemporalHistoryRequest& request) const;
    [[nodiscard]] TemporalHistoryBeginResult begin(const TemporalHistoryPlan& plan);
    [[nodiscard]] TemporalHistoryCommitResult commit(
        const TemporalHistoryCommitRequest& request);
    [[nodiscard]] TemporalHistoryResetResult reset(
        const TemporalHistoryResetRequest& request = {});

    [[nodiscard]] TemporalHistoryState state(
        TemporalHistoryConsumer consumer) const noexcept;
    [[nodiscard]] std::array<TemporalHistoryState, temporal_history_consumer_count>
    states() const;
    [[nodiscard]] const std::vector<TemporalHistoryDiagnostic>& diagnostics() const noexcept;
    void clear_diagnostics() noexcept;

    // Canonical evidence includes bounded diagnostics and lifecycle state.
    // Fingerprint intentionally excludes diagnostics so an error report cannot
    // change the identity of the actual temporal state.
    [[nodiscard]] std::string canonical_evidence() const;
    [[nodiscard]] std::string fingerprint() const;

private:
    struct Slot final {
        TemporalHistoryState state{};
        std::optional<TemporalHistoryPlan> active_plan;
    };

    void record_diagnostics(const std::vector<TemporalHistoryDiagnostic>& diagnostics);

    std::array<Slot, temporal_history_consumer_count> slots_{};
    std::vector<TemporalHistoryDiagnostic> diagnostics_;
    bool diagnostics_truncated_{};
};

[[nodiscard]] std::string temporal_history_canonical_evidence(
    const TemporalHistoryAuthority& authority);
[[nodiscard]] std::string temporal_history_fingerprint(
    const TemporalHistoryAuthority& authority);

} // namespace noemancer
