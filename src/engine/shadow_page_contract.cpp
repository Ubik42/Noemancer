#include "engine/shadow_page_contract.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, shadow_page_contract_max_text_bytes));
}

void add_diagnostic(std::vector<ShadowPageContractDiagnostic>& diagnostics,
                    std::string code, std::string path, std::string message) {
    if (diagnostics.size() >= shadow_page_contract_max_diagnostics) return;
    diagnostics.push_back(ShadowPageContractDiagnostic{
        .code = bounded_text(code),
        .path = bounded_text(path),
        .message = bounded_text(message),
    });
}

bool map_kind_valid(const ShadowPageMapKind kind) noexcept {
    return kind == ShadowPageMapKind::directional || kind == ShadowPageMapKind::local;
}

bool invalidation_reason_valid(const ShadowPageInvalidationReason reason) noexcept {
    switch (reason) {
    case ShadowPageInvalidationReason::light_transform:
    case ShadowPageInvalidationReason::camera_clipmap_shift:
    case ShadowPageInvalidationReason::caster_revision:
    case ShadowPageInvalidationReason::profile_resource_resize:
    case ShadowPageInvalidationReason::backend_device_loss:
    case ShadowPageInvalidationReason::manual_debug:
        return true;
    }
    return false;
}

std::uint32_t fallback_priority(const ShadowPageFallbackReason reason) noexcept {
    switch (reason) {
    case ShadowPageFallbackReason::none: return 0U;
    case ShadowPageFallbackReason::not_requested: return 1U;
    case ShadowPageFallbackReason::invalid_coordinates: return 2U;
    case ShadowPageFallbackReason::non_finite_projection: return 3U;
    case ShadowPageFallbackReason::stale_epoch: return 4U;
    case ShadowPageFallbackReason::feedback_overflow: return 5U;
    case ShadowPageFallbackReason::pool_exhausted: return 6U;
    case ShadowPageFallbackReason::unsupported_backend: return 7U;
    }
    return 7U;
}

bool key_less(const ShadowPageKey& left, const ShadowPageKey& right) noexcept {
    if (left.light_id != right.light_id) return left.light_id < right.light_id;
    if (left.map_kind != right.map_kind)
        return static_cast<std::uint8_t>(left.map_kind) <
            static_cast<std::uint8_t>(right.map_kind);
    if (left.level != right.level) return left.level < right.level;
    if (left.page_x != right.page_x) return left.page_x < right.page_x;
    if (left.page_y != right.page_y) return left.page_y < right.page_y;
    return left.epoch < right.epoch;
}

bool projection_finite(const ShadowPageProjection& projection) noexcept {
    if (!projection.valid) return false;
    return std::all_of(projection.clip_from_world.begin(), projection.clip_from_world.end(),
                       [](const float value) { return std::isfinite(value); });
}

bool limits_valid(const ShadowPagePlanLimits& limits,
                  std::vector<ShadowPageContractDiagnostic>& diagnostics) {
    bool valid = true;
    const auto check = [&](const std::uint32_t value, const std::uint32_t maximum,
                           const std::string_view path) {
        if (value == 0U || value > maximum) {
            add_diagnostic(diagnostics, "shadow-page.limits-range", std::string(path),
                           "The plan limit must be non-zero and within the bounded contract.");
            valid = false;
        }
    };
    check(limits.pool_capacity, shadow_page_contract_max_pool_capacity,
          "/limits/poolCapacity");
    check(limits.max_requests, shadow_page_contract_max_requests,
          "/limits/maxRequests");
    check(limits.max_resident, shadow_page_contract_max_resident,
          "/limits/maxResident");
    check(limits.max_dirty, shadow_page_contract_max_dirty, "/limits/maxDirty");
    check(limits.max_evictions, shadow_page_contract_max_evictions,
          "/limits/maxEvictions");
    check(limits.max_invalidations, shadow_page_contract_max_invalidations,
          "/limits/maxInvalidations");
    check(limits.max_fallback_samples, static_cast<std::uint32_t>(shadow_page_contract_max_samples),
          "/limits/maxFallbackSamples");
    check(limits.max_pages_rendered, shadow_page_contract_max_pages_rendered,
          "/limits/maxPagesRendered");
    if (limits.max_resident < limits.pool_capacity) {
        add_diagnostic(diagnostics, "shadow-page.limits-capacity", "/limits/maxResident",
                       "maxResident must cover the physical pool capacity.");
        valid = false;
    }
    return valid;
}

Json finite_matrix_json(const ShadowPageProjection& projection) {
    return Json{
        {"valid", projection.valid},
        {"finite", projection_finite(projection)},
    };
}

Json key_json(const ShadowPageKey& key) {
    return Json{
        {"lightId", bounded_text(key.light_id)},
        {"mapKind", shadow_page_map_kind_name(key.map_kind)},
        {"level", key.level},
        {"pageX", key.page_x},
        {"pageY", key.page_y},
        {"epoch", key.epoch},
    };
}

Json state_json(const ShadowPageState& state) {
    return Json{
        {"requested", state.requested},
        {"resident", state.resident},
        {"renderableAtLevel", state.renderable_at_level},
        {"fallbackLevel", state.fallback_level},
        {"dirty", state.dirty},
        {"uncached", state.uncached},
        {"lastRequestedFrame", state.last_requested_frame},
        {"lastRenderedFrame", state.last_rendered_frame},
    };
}

Json record_json(const ShadowPageStateRecord& record) {
    return Json{{"key", key_json(record.key)}, {"state", state_json(record.state)}};
}

Json request_json(const ShadowPageRequest& request) {
    return Json{
        {"key", key_json(request.key)},
        {"projection", finite_matrix_json(request.projection)},
        {"priority", request.priority},
        {"primary", request.primary},
    };
}

Json invalidation_json(const ShadowPageInvalidation& invalidation) {
    return Json{
        {"key", key_json(invalidation.key)},
        {"reason", shadow_page_invalidation_reason_name(invalidation.reason)},
    };
}

Json limits_json(const ShadowPagePlanLimits& limits) {
    return Json{
        {"poolCapacity", limits.pool_capacity},
        {"maxRequests", limits.max_requests},
        {"maxResident", limits.max_resident},
        {"maxDirty", limits.max_dirty},
        {"maxEvictions", limits.max_evictions},
        {"maxInvalidations", limits.max_invalidations},
        {"maxFallbackSamples", limits.max_fallback_samples},
        {"maxPagesRendered", limits.max_pages_rendered},
    };
}

Json statistics_json(const ShadowPageStatistics& statistics) {
    return Json{
        {"poolCapacity", statistics.pool_capacity},
        {"residentPages", statistics.resident_pages},
        {"requestedPages", statistics.requested_pages},
        {"newPages", statistics.new_pages},
        {"evictedPages", statistics.evicted_pages},
        {"invalidatedPages", statistics.invalidated_pages},
        {"missingSamples", statistics.missing_samples},
        {"fallbackSamples", statistics.fallback_samples},
        {"poolExhaustions", statistics.pool_exhaustions},
        {"feedbackOverflow", statistics.feedback_overflow},
        {"maxPagesRenderedThisFrame", statistics.max_pages_rendered_this_frame},
        {"duplicateRequests", statistics.duplicate_requests},
    };
}

Json fallback_json(const ShadowPageFallbackSample& fallback) {
    return Json{{"key", key_json(fallback.key)},
                {"reason", shadow_page_fallback_reason_name(fallback.reason)}};
}

Json diagnostic_json(const ShadowPageContractDiagnostic& diagnostic) {
    return Json{{"code", bounded_text(diagnostic.code)},
                {"path", bounded_text(diagnostic.path)},
                {"message", bounded_text(diagnostic.message)}};
}

template <typename T>
void sort_unique_keys(std::vector<T>& values) {
    std::sort(values.begin(), values.end(), [](const T& left, const T& right) {
        return key_less(left.key, right.key);
    });
    values.erase(std::unique(values.begin(), values.end(), [](const T& left, const T& right) {
        return left.key == right.key;
    }), values.end());
}

void sort_unique_key_values(std::vector<ShadowPageKey>& values) {
    std::sort(values.begin(), values.end(), key_less);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool contains_key(const std::vector<ShadowPageStateRecord>& records,
                 const ShadowPageKey& key) {
    return std::any_of(records.begin(), records.end(),
                       [&](const auto& record) { return record.key == key; });
}

bool contains_key(const std::vector<ShadowPageKey>& keys, const ShadowPageKey& key) {
    return std::any_of(keys.begin(), keys.end(),
                       [&](const auto& candidate) { return candidate == key; });
}

bool contains_key(const std::vector<ShadowPageRequest>& requests,
                  const ShadowPageKey& key) {
    return std::any_of(requests.begin(), requests.end(),
                       [&](const auto& request) { return request.key == key; });
}

void set_global_fallback(ShadowPagePlan& plan,
                         const ShadowPageFallbackReason reason) {
    plan.fallback_active = true;
    if (fallback_priority(reason) > fallback_priority(plan.fallback_reason))
        plan.fallback_reason = reason;
}

void append_fallback(ShadowPagePlan& plan, const ShadowPageKey& key,
                     const ShadowPageFallbackReason reason,
                     const bool missing_sample) {
    set_global_fallback(plan, reason);
    ++plan.statistics.fallback_samples;
    if (missing_sample) ++plan.statistics.missing_samples;
    if (plan.fallback_samples.size() < plan.limits.max_fallback_samples)
        plan.fallback_samples.push_back(ShadowPageFallbackSample{key, reason});
}

bool valid_request_key(const ShadowPageRequest& request,
                       ShadowPagePlan& plan) {
    const auto diagnostics = validate_shadow_page_key(request.key);
    if (!diagnostics.empty()) {
        for (const auto& diagnostic : diagnostics)
            add_diagnostic(plan.diagnostics, diagnostic.code, diagnostic.path, diagnostic.message);
        append_fallback(plan, request.key, ShadowPageFallbackReason::invalid_coordinates, true);
        return false;
    }
    if (request.key.epoch != plan.epoch) {
        append_fallback(plan, request.key, ShadowPageFallbackReason::stale_epoch, true);
        return false;
    }
    if (!projection_finite(request.projection)) {
        add_diagnostic(plan.diagnostics, "shadow-page.projection-non-finite",
                       "/requests/projection",
                       "A page request with a non-finite or invalid projection is rejected.");
        append_fallback(plan, request.key,
                        ShadowPageFallbackReason::non_finite_projection, true);
        return false;
    }
    return true;
}

bool valid_current_key(const ShadowPageKey& key, ShadowPagePlan& plan,
                       const ShadowPageFallbackReason invalid_reason =
                           ShadowPageFallbackReason::invalid_coordinates) {
    const auto diagnostics = validate_shadow_page_key(key);
    if (!diagnostics.empty()) {
        for (const auto& diagnostic : diagnostics)
            add_diagnostic(plan.diagnostics, diagnostic.code, diagnostic.path, diagnostic.message);
        append_fallback(plan, key, invalid_reason, true);
        return false;
    }
    if (key.epoch != plan.epoch) {
        append_fallback(plan, key, ShadowPageFallbackReason::stale_epoch, true);
        return false;
    }
    return true;
}

bool invalidation_less(const ShadowPageInvalidation& left,
                       const ShadowPageInvalidation& right) noexcept {
    if (key_less(left.key, right.key)) return true;
    if (key_less(right.key, left.key)) return false;
    return static_cast<std::uint8_t>(left.reason) <
        static_cast<std::uint8_t>(right.reason);
}

bool invalidation_equal(const ShadowPageInvalidation& left,
                        const ShadowPageInvalidation& right) noexcept {
    return left.key == right.key && left.reason == right.reason;
}

} // namespace

std::string_view shadow_page_map_kind_name(const ShadowPageMapKind kind) noexcept {
    switch (kind) {
    case ShadowPageMapKind::directional: return "directional";
    case ShadowPageMapKind::local: return "local";
    }
    return "unknown";
}

std::string_view shadow_page_invalidation_reason_name(
    const ShadowPageInvalidationReason reason) noexcept {
    switch (reason) {
    case ShadowPageInvalidationReason::light_transform: return "light-transform";
    case ShadowPageInvalidationReason::camera_clipmap_shift: return "camera-clipmap-shift";
    case ShadowPageInvalidationReason::caster_revision: return "caster-revision";
    case ShadowPageInvalidationReason::profile_resource_resize: return "profile-resource-resize";
    case ShadowPageInvalidationReason::backend_device_loss: return "backend-device-loss";
    case ShadowPageInvalidationReason::manual_debug: return "manual-debug";
    }
    return "unknown";
}

std::string_view shadow_page_fallback_reason_name(
    const ShadowPageFallbackReason reason) noexcept {
    switch (reason) {
    case ShadowPageFallbackReason::none: return "none";
    case ShadowPageFallbackReason::not_requested: return "not-requested";
    case ShadowPageFallbackReason::pool_exhausted: return "pool-exhausted";
    case ShadowPageFallbackReason::non_finite_projection: return "non-finite-projection";
    case ShadowPageFallbackReason::stale_epoch: return "stale-epoch";
    case ShadowPageFallbackReason::feedback_overflow: return "feedback-overflow";
    case ShadowPageFallbackReason::unsupported_backend: return "unsupported-backend";
    case ShadowPageFallbackReason::invalid_coordinates: return "invalid-coordinates";
    }
    return "unknown";
}

bool operator==(const ShadowPageKey& left, const ShadowPageKey& right) noexcept {
    return left.light_id == right.light_id && left.map_kind == right.map_kind &&
        left.level == right.level && left.page_x == right.page_x &&
        left.page_y == right.page_y && left.epoch == right.epoch;
}

bool operator!=(const ShadowPageKey& left, const ShadowPageKey& right) noexcept {
    return !(left == right);
}

std::vector<ShadowPageContractDiagnostic>
validate_shadow_page_key(const ShadowPageKey& key) {
    std::vector<ShadowPageContractDiagnostic> diagnostics;
    if (key.light_id.empty() || key.light_id.size() > shadow_page_contract_max_text_bytes)
        add_diagnostic(diagnostics, "shadow-page.light-id-invalid", "/lightId",
                       "lightId must be non-empty and bounded.");
    if (!map_kind_valid(key.map_kind))
        add_diagnostic(diagnostics, "shadow-page.map-kind-invalid", "/mapKind",
                       "mapKind must be directional or local.");
    if (key.level > shadow_page_contract_max_level)
        add_diagnostic(diagnostics, "shadow-page.level-out-of-range", "/level",
                       "The page level exceeds the bounded contract.");
    if (key.page_x < 0 || key.page_x > shadow_page_contract_max_coordinate)
        add_diagnostic(diagnostics, "shadow-page.coordinate-out-of-range", "/pageX",
                       "pageX must be a non-negative bounded page coordinate.");
    if (key.page_y < 0 || key.page_y > shadow_page_contract_max_coordinate)
        add_diagnostic(diagnostics, "shadow-page.coordinate-out-of-range", "/pageY",
                       "pageY must be a non-negative bounded page coordinate.");
    return diagnostics;
}

ShadowPagePlan plan_shadow_pages(const ShadowPageFrameInput& input) {
    ShadowPagePlan plan;
    plan.schema = std::string(shadow_page_contract_schema);
    plan.epoch = input.epoch;
    plan.frame = input.frame;
    plan.limits = input.limits;
    plan.statistics.pool_capacity = input.limits.pool_capacity;
    if (input.schema != shadow_page_contract_schema ||
        input.schema.size() > shadow_page_contract_max_text_bytes) {
        add_diagnostic(plan.diagnostics, "shadow-page.schema-invalid", "/schema",
                       "Expected noemancer.shadow-page-contract/0.1.");
    }
    if (!limits_valid(input.limits, plan.diagnostics)) {
        plan.fallback_active = true;
        plan.fallback_reason = ShadowPageFallbackReason::unsupported_backend;
        return plan;
    }
    plan.valid = true;
    if (!input.backend_supported) {
        set_global_fallback(plan, ShadowPageFallbackReason::unsupported_backend);
        add_diagnostic(plan.diagnostics, "shadow-page.backend-unsupported", "/backendSupported",
                       "The backend cannot execute this page contract; use CSM or fully-lit fallback.");
    }

    const auto mark_overflow = [&](const std::size_t count, const std::string_view path) {
        if (count <= shadow_page_contract_max_samples) return;
        const auto overflow = static_cast<std::uint32_t>(std::min<std::size_t>(
            count - shadow_page_contract_max_samples, std::numeric_limits<std::uint32_t>::max()));
        plan.statistics.feedback_overflow += overflow;
        set_global_fallback(plan, ShadowPageFallbackReason::feedback_overflow);
        add_diagnostic(plan.diagnostics, "shadow-page.input-overflow", std::string(path),
                       "Input samples beyond the hard observation bound were not admitted.");
    };
    mark_overflow(input.requests.size(), "/requests");
    mark_overflow(input.resident_pages.size(), "/residentPages");
    mark_overflow(input.dirty_pages.size(), "/dirtyPages");
    mark_overflow(input.eviction_candidates.size(), "/evictionCandidates");
    mark_overflow(input.invalidations.size(), "/invalidations");
    if (input.feedback_overflow) {
        ++plan.statistics.feedback_overflow;
        set_global_fallback(plan, ShadowPageFallbackReason::feedback_overflow);
    }

    std::vector<ShadowPageRequest> requests;
    requests.reserve(std::min<std::size_t>(input.requests.size(), shadow_page_contract_max_samples));
    const auto request_count = std::min<std::size_t>(input.requests.size(), shadow_page_contract_max_samples);
    for (std::size_t index = 0U; index < request_count; ++index) {
        const auto& request = input.requests[index];
        if (input.backend_supported && valid_request_key(request, plan)) requests.push_back(request);
        else if (!input.backend_supported)
            append_fallback(plan, request.key, ShadowPageFallbackReason::unsupported_backend, true);
    }
    std::sort(requests.begin(), requests.end(), [](const auto& left, const auto& right) {
        if (key_less(left.key, right.key)) return true;
        if (key_less(right.key, left.key)) return false;
        if (left.priority != right.priority) return left.priority > right.priority;
        if (left.primary != right.primary) return left.primary > right.primary;
        return left.projection.clip_from_world < right.projection.clip_from_world;
    });
    for (const auto& request : requests) {
        if (!plan.requested_pages.empty() && plan.requested_pages.back().key == request.key) {
            ++plan.statistics.duplicate_requests;
            continue;
        }
        if (plan.requested_pages.size() >= input.limits.max_requests) {
            append_fallback(plan, request.key, ShadowPageFallbackReason::feedback_overflow, true);
            ++plan.statistics.feedback_overflow;
            continue;
        }
        plan.requested_pages.push_back(request);
    }
    sort_unique_keys(plan.requested_pages);

    const auto resident_count = std::min<std::size_t>(input.resident_pages.size(), shadow_page_contract_max_samples);
    for (std::size_t index = 0U; index < resident_count; ++index) {
        const auto& record = input.resident_pages[index];
        if (!input.backend_supported || !record.state.resident) continue;
        if (valid_current_key(record.key, plan)) plan.resident_pages.push_back(record);
    }
    sort_unique_keys(plan.resident_pages);
    while (plan.resident_pages.size() > input.limits.max_resident) plan.resident_pages.pop_back();
    while (plan.resident_pages.size() > input.limits.pool_capacity) {
        plan.evicted_pages.push_back(plan.resident_pages.back().key);
        plan.resident_pages.pop_back();
    }

    std::vector<ShadowPageKey> eviction_candidates;
    const auto eviction_count = std::min<std::size_t>(input.eviction_candidates.size(), shadow_page_contract_max_samples);
    for (std::size_t index = 0U; index < eviction_count; ++index) {
        const auto& key = input.eviction_candidates[index];
        if (input.backend_supported && valid_current_key(key, plan)) eviction_candidates.push_back(key);
    }
    sort_unique_key_values(eviction_candidates);
    if (eviction_candidates.size() > input.limits.max_evictions)
        eviction_candidates.resize(input.limits.max_evictions);

    if (input.backend_supported) {
        for (const auto& request : plan.requested_pages) {
            auto resident = std::find_if(plan.resident_pages.begin(), plan.resident_pages.end(),
                                         [&](const auto& record) { return record.key == request.key; });
            if (resident != plan.resident_pages.end()) {
                resident->state.requested = true;
                resident->state.last_requested_frame = input.frame;
                continue;
            }
            if (plan.resident_pages.size() >= input.limits.pool_capacity) {
                auto candidate = std::find_if(eviction_candidates.begin(), eviction_candidates.end(),
                    [&](const auto& key) { return !contains_key(plan.requested_pages, key) &&
                        contains_key(plan.resident_pages, key) && !contains_key(plan.evicted_pages, key); });
                if (candidate != eviction_candidates.end() &&
                    plan.evicted_pages.size() < input.limits.max_evictions) {
                    const auto key = *candidate;
                    plan.evicted_pages.push_back(key);
                    plan.resident_pages.erase(std::remove_if(plan.resident_pages.begin(), plan.resident_pages.end(),
                        [&](const auto& record) { return record.key == key; }), plan.resident_pages.end());
                }
            }
            if (plan.resident_pages.size() >= input.limits.pool_capacity) {
                append_fallback(plan, request.key, ShadowPageFallbackReason::pool_exhausted, true);
                ++plan.statistics.pool_exhaustions;
                continue;
            }
            ShadowPageState state;
            state.requested = true;
            state.resident = true;
            state.renderable_at_level = false;
            state.dirty = true;
            state.uncached = true;
            state.last_requested_frame = input.frame;
            const ShadowPageStateRecord new_page{request.key, state};
            plan.new_pages.push_back(new_page);
            plan.resident_pages.push_back(new_page);
        }
    }

    const auto dirty_count = std::min<std::size_t>(input.dirty_pages.size(), shadow_page_contract_max_samples);
    for (std::size_t index = 0U; index < dirty_count; ++index) {
        const auto& record = input.dirty_pages[index];
        if (!input.backend_supported || !record.state.resident || !valid_current_key(record.key, plan)) continue;
        if (contains_key(plan.dirty_pages, record.key)) continue;
        if (plan.dirty_pages.size() < input.limits.max_dirty) {
            auto dirty = record;
            dirty.state.dirty = true;
            plan.dirty_pages.push_back(dirty);
        } else {
            ++plan.statistics.feedback_overflow;
            set_global_fallback(plan, ShadowPageFallbackReason::feedback_overflow);
        }
    }

    const auto invalidation_count = std::min<std::size_t>(input.invalidations.size(), shadow_page_contract_max_samples);
    std::vector<ShadowPageInvalidation> invalidations;
    for (std::size_t index = 0U; index < invalidation_count; ++index) {
        const auto& invalidation = input.invalidations[index];
        if (!invalidation_reason_valid(invalidation.reason)) {
            add_diagnostic(plan.diagnostics, "shadow-page.invalidation-reason-invalid", "/invalidations/reason",
                           "Unknown invalidation reason was rejected.");
            continue;
        }
        if (input.backend_supported && valid_current_key(invalidation.key, plan))
            invalidations.push_back(invalidation);
    }
    std::sort(invalidations.begin(), invalidations.end(), invalidation_less);
    invalidations.erase(std::unique(invalidations.begin(), invalidations.end(), invalidation_equal), invalidations.end());
    if (invalidations.size() > input.limits.max_invalidations) {
        const auto discarded = invalidations.size() - input.limits.max_invalidations;
        plan.statistics.feedback_overflow += static_cast<std::uint32_t>(std::min<std::size_t>(
            discarded, std::numeric_limits<std::uint32_t>::max()));
        invalidations.resize(input.limits.max_invalidations);
        set_global_fallback(plan, ShadowPageFallbackReason::feedback_overflow);
    }
    plan.invalidated_pages = invalidations;
    for (const auto& invalidation : plan.invalidated_pages) {
        if (contains_key(plan.dirty_pages, invalidation.key)) continue;
        auto resident = std::find_if(plan.resident_pages.begin(), plan.resident_pages.end(),
                                     [&](const auto& record) { return record.key == invalidation.key; });
        if (resident != plan.resident_pages.end() && plan.dirty_pages.size() < input.limits.max_dirty) {
            auto dirty = *resident;
            dirty.state.dirty = true;
            plan.dirty_pages.push_back(dirty);
        }
    }
    sort_unique_keys(plan.dirty_pages);
    std::sort(plan.resident_pages.begin(), plan.resident_pages.end(),
              [](const auto& left, const auto& right) { return key_less(left.key, right.key); });
    std::sort(plan.new_pages.begin(), plan.new_pages.end(),
              [](const auto& left, const auto& right) { return key_less(left.key, right.key); });
    sort_unique_key_values(plan.evicted_pages);

    plan.statistics.resident_pages = static_cast<std::uint32_t>(plan.resident_pages.size());
    plan.statistics.requested_pages = static_cast<std::uint32_t>(plan.requested_pages.size());
    plan.statistics.new_pages = static_cast<std::uint32_t>(plan.new_pages.size());
    plan.statistics.evicted_pages = static_cast<std::uint32_t>(plan.evicted_pages.size());
    plan.statistics.invalidated_pages = static_cast<std::uint32_t>(plan.invalidated_pages.size());
    plan.statistics.max_pages_rendered_this_frame = std::min(
        input.pages_rendered_this_frame, input.limits.max_pages_rendered);
    return plan;
}

ShadowPageLookupResult lookup_shadow_page(
    const ShadowPageKey& key, const std::uint64_t current_epoch,
    const std::vector<ShadowPageStateRecord>& resident_pages) {
    ShadowPageLookupResult result;
    const auto diagnostics = validate_shadow_page_key(key);
    if (!diagnostics.empty()) {
        result.fallback_reason = ShadowPageFallbackReason::invalid_coordinates;
        result.detail = "Invalid page coordinates or identity cannot be sampled.";
        return result;
    }
    if (key.epoch != current_epoch) {
        result.fallback_reason = ShadowPageFallbackReason::stale_epoch;
        result.detail = "The requested page epoch does not match the current epoch.";
        return result;
    }
    const auto record = std::find_if(resident_pages.begin(), resident_pages.end(),
        [&](const auto& candidate) { return candidate.key == key && candidate.key.epoch == current_epoch; });
    if (record == resident_pages.end() || !record->state.resident ||
        !record->state.renderable_at_level) {
        result.fallback_reason = ShadowPageFallbackReason::not_requested;
        result.detail = "No renderable resident page exists for the current epoch.";
        return result;
    }
    result.hit = true;
    result.record = *record;
    result.fallback_reason = ShadowPageFallbackReason::none;
    result.detail = "Current-epoch renderable page hit.";
    return result;
}

std::string shadow_page_contract_canonical_evidence(const ShadowPagePlan& plan) {
    Json requested = Json::array();
    for (const auto& request : plan.requested_pages) requested.push_back(request_json(request));
    Json resident = Json::array();
    for (const auto& record : plan.resident_pages) resident.push_back(record_json(record));
    Json new_pages = Json::array();
    for (const auto& record : plan.new_pages) new_pages.push_back(record_json(record));
    Json dirty = Json::array();
    for (const auto& record : plan.dirty_pages) dirty.push_back(record_json(record));
    Json evicted = Json::array();
    for (const auto& key : plan.evicted_pages) evicted.push_back(key_json(key));
    Json invalidated = Json::array();
    for (const auto& invalidation : plan.invalidated_pages)
        invalidated.push_back(invalidation_json(invalidation));
    Json fallbacks = Json::array();
    for (const auto& fallback : plan.fallback_samples) fallbacks.push_back(fallback_json(fallback));
    Json diagnostics = Json::array();
    for (const auto& diagnostic : plan.diagnostics) diagnostics.push_back(diagnostic_json(diagnostic));
    const Json output = {
        {"schema", bounded_text(plan.schema)},
        {"epoch", plan.epoch},
        {"frame", plan.frame},
        {"limits", limits_json(plan.limits)},
        {"valid", plan.valid},
        {"fallbackActive", plan.fallback_active},
        {"fallbackReason", shadow_page_fallback_reason_name(plan.fallback_reason)},
        {"requestedPages", std::move(requested)},
        {"residentPages", std::move(resident)},
        {"newPages", std::move(new_pages)},
        {"dirtyPages", std::move(dirty)},
        {"evictedPages", std::move(evicted)},
        {"invalidatedPages", std::move(invalidated)},
        {"fallbackSamples", std::move(fallbacks)},
        {"statistics", statistics_json(plan.statistics)},
        {"diagnostics", std::move(diagnostics)},
    };
    return output.dump(2) + "\n";
}

std::string shadow_page_contract_fingerprint(const ShadowPagePlan& plan) {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const auto canonical = shadow_page_contract_canonical_evidence(plan);
    for (const auto character : canonical) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= prime;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16U, '0');
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const auto shift = static_cast<unsigned int>((text.size() - 1U - index) * 4U);
        text[index] = digits[(hash >> shift) & 0x0fU];
    }
    return "fnv1a64:" + text;
}

} // namespace noemancer
