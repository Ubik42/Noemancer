#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Bounded, renderer-neutral data contract for the first shadow-page slice.
// It deliberately describes ownership, epochs and fallback evidence rather
// than a page-table texture, physical GPU tile or backend resource handle.
inline constexpr std::string_view shadow_page_contract_schema =
    "noemancer.shadow-page-contract/0.1";
inline constexpr std::size_t shadow_page_contract_max_text_bytes = 128U;
inline constexpr std::size_t shadow_page_contract_max_diagnostics = 32U;
inline constexpr std::size_t shadow_page_contract_max_samples = 256U;
inline constexpr std::uint32_t shadow_page_contract_max_level = 31U;
inline constexpr std::int32_t shadow_page_contract_max_coordinate = 1'048'575;
inline constexpr std::uint32_t shadow_page_contract_max_pool_capacity = 512U;
inline constexpr std::uint32_t shadow_page_contract_max_requests = 256U;
inline constexpr std::uint32_t shadow_page_contract_max_resident = 512U;
inline constexpr std::uint32_t shadow_page_contract_max_dirty = 256U;
inline constexpr std::uint32_t shadow_page_contract_max_evictions = 256U;
inline constexpr std::uint32_t shadow_page_contract_max_invalidations = 256U;
inline constexpr std::uint32_t shadow_page_contract_max_pages_rendered = 256U;

enum class ShadowPageMapKind : std::uint8_t {
    directional = 0U,
    local = 1U,

    Directional = directional,
    Local = local,
};

enum class ShadowPageInvalidationReason : std::uint8_t {
    light_transform = 0U,
    camera_clipmap_shift = 1U,
    caster_revision = 2U,
    profile_resource_resize = 3U,
    backend_device_loss = 4U,
    manual_debug = 5U,

    LightTransform = light_transform,
    CameraClipmapShift = camera_clipmap_shift,
    CasterRevision = caster_revision,
    ProfileResourceResize = profile_resource_resize,
    BackendDeviceLoss = backend_device_loss,
    ManualDebug = manual_debug,
};

enum class ShadowPageFallbackReason : std::uint8_t {
    none = 0U,
    not_requested = 1U,
    pool_exhausted = 2U,
    non_finite_projection = 3U,
    stale_epoch = 4U,
    feedback_overflow = 5U,
    unsupported_backend = 6U,
    invalid_coordinates = 7U,

    None = none,
    NotRequested = not_requested,
    PoolExhausted = pool_exhausted,
    NonFiniteProjection = non_finite_projection,
    StaleEpoch = stale_epoch,
    FeedbackOverflow = feedback_overflow,
    UnsupportedBackend = unsupported_backend,
    InvalidCoordinates = invalid_coordinates,
};

[[nodiscard]] std::string_view shadow_page_map_kind_name(
    ShadowPageMapKind kind) noexcept;
[[nodiscard]] std::string_view shadow_page_invalidation_reason_name(
    ShadowPageInvalidationReason reason) noexcept;
[[nodiscard]] std::string_view shadow_page_fallback_reason_name(
    ShadowPageFallbackReason reason) noexcept;

struct ShadowPageKey final {
    std::string light_id;
    ShadowPageMapKind map_kind{ShadowPageMapKind::directional};
    std::uint32_t level{};
    std::int32_t page_x{};
    std::int32_t page_y{};
    std::uint64_t epoch{};
};

[[nodiscard]] bool operator==(const ShadowPageKey& left,
                              const ShadowPageKey& right) noexcept;
[[nodiscard]] bool operator!=(const ShadowPageKey& left,
                              const ShadowPageKey& right) noexcept;

struct ShadowPageState final {
    bool requested{};
    bool resident{};
    bool renderable_at_level{};
    std::int32_t fallback_level{-1};
    bool dirty{};
    bool uncached{};
    std::uint64_t last_requested_frame{};
    std::uint64_t last_rendered_frame{};
};

struct ShadowPageStateRecord final {
    ShadowPageKey key;
    ShadowPageState state;
};

struct ShadowPageProjection final {
    bool valid{};
    std::array<float, 16> clip_from_world{};
};

struct ShadowPageRequest final {
    ShadowPageKey key;
    ShadowPageProjection projection;
    std::uint32_t priority{};
    bool primary{};
};

struct ShadowPageInvalidation final {
    ShadowPageKey key;
    ShadowPageInvalidationReason reason{ShadowPageInvalidationReason::manual_debug};
};

struct ShadowPageContractDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct ShadowPagePlanLimits final {
    std::uint32_t pool_capacity{64U};
    std::uint32_t max_requests{64U};
    std::uint32_t max_resident{64U};
    std::uint32_t max_dirty{64U};
    std::uint32_t max_evictions{64U};
    std::uint32_t max_invalidations{64U};
    std::uint32_t max_fallback_samples{64U};
    std::uint32_t max_pages_rendered{64U};
};

struct ShadowPageFrameInput final {
    std::string schema{std::string(shadow_page_contract_schema)};
    std::uint64_t epoch{};
    std::uint64_t frame{};
    ShadowPagePlanLimits limits;
    bool backend_supported{true};
    bool feedback_overflow{};
    std::uint32_t pages_rendered_this_frame{};
    std::vector<ShadowPageRequest> requests;
    std::vector<ShadowPageStateRecord> resident_pages;
    std::vector<ShadowPageStateRecord> dirty_pages;
    std::vector<ShadowPageKey> eviction_candidates;
    std::vector<ShadowPageInvalidation> invalidations;
};

struct ShadowPageStatistics final {
    std::uint32_t pool_capacity{};
    std::uint32_t resident_pages{};
    std::uint32_t requested_pages{};
    std::uint32_t new_pages{};
    std::uint32_t evicted_pages{};
    std::uint32_t invalidated_pages{};
    std::uint32_t missing_samples{};
    std::uint32_t fallback_samples{};
    std::uint32_t pool_exhaustions{};
    std::uint32_t feedback_overflow{};
    std::uint32_t max_pages_rendered_this_frame{};
    std::uint32_t duplicate_requests{};
};

struct ShadowPageFallbackSample final {
    ShadowPageKey key;
    ShadowPageFallbackReason reason{ShadowPageFallbackReason::none};
};

struct ShadowPagePlan final {
    std::string schema{std::string(shadow_page_contract_schema)};
    std::uint64_t epoch{};
    std::uint64_t frame{};
    ShadowPagePlanLimits limits;
    bool valid{};
    bool fallback_active{};
    ShadowPageFallbackReason fallback_reason{ShadowPageFallbackReason::none};
    std::vector<ShadowPageRequest> requested_pages;
    std::vector<ShadowPageStateRecord> resident_pages;
    std::vector<ShadowPageStateRecord> new_pages;
    std::vector<ShadowPageStateRecord> dirty_pages;
    std::vector<ShadowPageKey> evicted_pages;
    std::vector<ShadowPageInvalidation> invalidated_pages;
    std::vector<ShadowPageFallbackSample> fallback_samples;
    ShadowPageStatistics statistics;
    std::vector<ShadowPageContractDiagnostic> diagnostics;
};

struct ShadowPageLookupResult final {
    bool hit{};
    ShadowPageStateRecord record;
    ShadowPageFallbackReason fallback_reason{ShadowPageFallbackReason::none};
    std::string detail;
};

[[nodiscard]] std::vector<ShadowPageContractDiagnostic>
validate_shadow_page_key(const ShadowPageKey& key);

// Sorts and deduplicates a bounded frame input, never admits a page from an
// old epoch, and reports pool/feedback pressure without allocating unbounded
// request or diagnostic lists.
[[nodiscard]] ShadowPagePlan plan_shadow_pages(const ShadowPageFrameInput& input);

// A sample only hits when both the requested key and the resident record have
// the current epoch, are valid and are renderable.  Any stale/missing state is
// an explicit fallback rather than a dark shadow.
[[nodiscard]] ShadowPageLookupResult lookup_shadow_page(
    const ShadowPageKey& key, std::uint64_t current_epoch,
    const std::vector<ShadowPageStateRecord>& resident_pages);

[[nodiscard]] std::string shadow_page_contract_canonical_evidence(
    const ShadowPagePlan& plan);
[[nodiscard]] std::string shadow_page_contract_fingerprint(
    const ShadowPagePlan& plan);

} // namespace noemancer
