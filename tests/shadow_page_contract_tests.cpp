#include "engine/shadow_page_contract.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "shadow_page_contract_tests: " << message << '\n';
    return condition;
}

ShadowPageProjection projection() {
    ShadowPageProjection result;
    result.valid = true;
    result.clip_from_world.fill(0.0F);
    result.clip_from_world[0] = 1.0F;
    result.clip_from_world[5] = 1.0F;
    result.clip_from_world[10] = 1.0F;
    result.clip_from_world[15] = 1.0F;
    return result;
}

ShadowPageKey key(const std::int32_t x, const std::int32_t y,
                 const std::uint64_t epoch = 9U) {
    return ShadowPageKey{
        .light_id = "sun.primary",
        .map_kind = ShadowPageMapKind::directional,
        .level = 1U,
        .page_x = x,
        .page_y = y,
        .epoch = epoch,
    };
}

ShadowPageStateRecord resident_record(const ShadowPageKey& page,
                                      const bool renderable = true) {
    return ShadowPageStateRecord{
        .key = page,
        .state = ShadowPageState{
            .requested = true,
            .resident = true,
            .renderable_at_level = renderable,
            .fallback_level = -1,
            .dirty = false,
            .uncached = false,
            .last_requested_frame = 8U,
            .last_rendered_frame = 8U,
        },
    };
}

ShadowPageFrameInput base_input() {
    ShadowPageFrameInput input;
    input.epoch = 9U;
    input.frame = 10U;
    input.limits = ShadowPagePlanLimits{
        .pool_capacity = 4U,
        .max_requests = 4U,
        .max_resident = 4U,
        .max_dirty = 4U,
        .max_evictions = 4U,
        .max_invalidations = 4U,
        .max_fallback_samples = 4U,
        .max_pages_rendered = 4U,
    };
    return input;
}

bool test_vocabulary_and_key_bounds() {
    if (!check(shadow_page_contract_schema ==
                   "noemancer.shadow-page-contract/0.1",
               "schema drifted")) return false;
    if (!check(shadow_page_map_kind_name(ShadowPageMapKind::directional) == "directional" &&
                   shadow_page_invalidation_reason_name(
                       ShadowPageInvalidationReason::camera_clipmap_shift) ==
                       "camera-clipmap-shift" &&
                   shadow_page_fallback_reason_name(
                       ShadowPageFallbackReason::pool_exhausted) == "pool-exhausted",
               "page vocabulary drifted")) return false;
    if (!check(validate_shadow_page_key(key(0, 0)).empty(),
               "valid page key was rejected")) return false;

    auto negative = key(-1, 0);
    auto too_large = key(shadow_page_contract_max_coordinate + 1, 0);
    auto long_id = key(0, 0);
    long_id.light_id.assign(shadow_page_contract_max_text_bytes + 1U, 'x');
    return check(!validate_shadow_page_key(negative).empty() &&
                     !validate_shadow_page_key(too_large).empty() &&
                     !validate_shadow_page_key(long_id).empty(),
                 "negative, overlarge and oversized identity coordinates were accepted");
}

bool test_duplicate_requests_and_determinism() {
    auto input = base_input();
    const auto first_key = key(0, 0);
    const auto second_key = key(1, 0);
    input.requests.push_back(ShadowPageRequest{first_key, projection(), 2U, true});
    input.requests.push_back(ShadowPageRequest{first_key, projection(), 1U, false});
    input.requests.push_back(ShadowPageRequest{second_key, projection(), 1U, true});
    input.resident_pages.push_back(resident_record(first_key));
    input.invalidations.push_back(ShadowPageInvalidation{
        .key = second_key,
        .reason = ShadowPageInvalidationReason::caster_revision,
    });

    const auto first = plan_shadow_pages(input);
    const auto second = plan_shadow_pages(input);
    if (!check(first.valid && first.statistics.requested_pages == 2U &&
                   first.statistics.duplicate_requests == 1U &&
                   first.statistics.new_pages == 1U &&
                   first.statistics.resident_pages == 2U &&
                   first.statistics.invalidated_pages == 1U &&
                   first.fallback_samples.empty(),
               "duplicate request planning did not produce a bounded deterministic set")) return false;
    return check(shadow_page_contract_canonical_evidence(first) ==
                     shadow_page_contract_canonical_evidence(second) &&
                     shadow_page_contract_fingerprint(first) ==
                         shadow_page_contract_fingerprint(second),
                 "equivalent page input produced unstable evidence");
}

bool test_stale_epoch_and_invalid_projection_fallbacks() {
    auto input = base_input();
    auto stale = key(0, 0, 8U);
    auto negative = key(-1, 1);
    auto non_finite = key(2, 0);
    auto bad_projection = projection();
    bad_projection.clip_from_world[4] = std::numeric_limits<float>::quiet_NaN();
    input.requests.push_back(ShadowPageRequest{stale, projection(), 1U, true});
    input.requests.push_back(ShadowPageRequest{negative, projection(), 1U, true});
    input.requests.push_back(ShadowPageRequest{non_finite, bad_projection, 1U, true});
    const auto plan = plan_shadow_pages(input);
    if (!check(plan.valid && plan.statistics.requested_pages == 0U &&
                   plan.statistics.fallback_samples >= 3U &&
                   plan.statistics.missing_samples >= 3U &&
                   plan.fallback_active,
               "invalid page requests were not converted to explicit fallback samples")) return false;

    std::vector<ShadowPageStateRecord> resident{resident_record(key(3, 0))};
    auto stale_sample = key(3, 0, 8U);
    const auto stale_lookup = lookup_shadow_page(stale_sample, 9U, resident);
    if (!check(!stale_lookup.hit &&
                   stale_lookup.fallback_reason == ShadowPageFallbackReason::stale_epoch,
               "stale epoch sample hit a page from another epoch")) return false;
    auto current = key(3, 0);
    const auto hit = lookup_shadow_page(current, 9U, resident);
    return check(hit.hit && hit.fallback_reason == ShadowPageFallbackReason::none,
                 "current renderable page did not hit");
}

bool test_pool_exhaustion_and_eviction() {
    auto exhausted = base_input();
    exhausted.limits.pool_capacity = 1U;
    exhausted.limits.max_resident = 1U;
    exhausted.limits.max_requests = 2U;
    exhausted.requests.push_back(ShadowPageRequest{key(0, 0), projection(), 2U, true});
    exhausted.requests.push_back(ShadowPageRequest{key(1, 0), projection(), 1U, true});
    const auto no_eviction = plan_shadow_pages(exhausted);
    if (!check(no_eviction.valid && no_eviction.statistics.new_pages == 1U &&
                   no_eviction.statistics.pool_exhaustions == 1U &&
                   no_eviction.fallback_reason == ShadowPageFallbackReason::pool_exhausted &&
                   no_eviction.statistics.resident_pages == 1U,
               "pool exhaustion did not preserve a hard capacity and explicit fallback")) return false;

    auto with_eviction = base_input();
    with_eviction.limits.pool_capacity = 1U;
    with_eviction.limits.max_resident = 1U;
    const auto old_page = key(0, 0);
    const auto new_page = key(1, 0);
    with_eviction.resident_pages.push_back(resident_record(old_page));
    with_eviction.requests.push_back(ShadowPageRequest{new_page, projection(), 1U, true});
    with_eviction.eviction_candidates.push_back(old_page);
    const auto evicted = plan_shadow_pages(with_eviction);
    return check(evicted.statistics.evicted_pages == 1U &&
                     evicted.statistics.new_pages == 1U &&
                     evicted.statistics.pool_exhaustions == 0U &&
                     evicted.resident_pages.size() == 1U &&
                     evicted.resident_pages.front().key == new_page,
                 "bounded eviction did not make deterministic room for a request");
}

bool test_hard_caps_and_canonical_evidence() {
    auto input = base_input();
    input.limits.max_requests = 3U;
    input.limits.max_fallback_samples = 3U;
    for (std::int32_t index = 0; index < 300; ++index)
        input.requests.push_back(ShadowPageRequest{key(index, 0), projection(), 1U, true});
    const auto plan = plan_shadow_pages(input);
    const auto evidence = shadow_page_contract_canonical_evidence(plan);
    if (!check(plan.valid && plan.requested_pages.size() <= 3U &&
                   plan.fallback_samples.size() <= 3U &&
                   plan.statistics.feedback_overflow > 0U && evidence.size() < 128U * 1024U,
               "request, fallback or evidence hard caps were not enforced")) return false;

    auto missing = base_input();
    missing.backend_supported = false;
    missing.requests.push_back(ShadowPageRequest{key(0, 0), projection(), 1U, true});
    const auto unsupported = plan_shadow_pages(missing);
    return check(unsupported.valid && unsupported.fallback_reason ==
                     ShadowPageFallbackReason::unsupported_backend &&
                     shadow_page_contract_canonical_evidence(unsupported).find(
                         "unsupported-backend") != std::string::npos,
                 "unsupported backend did not produce explicit fallback evidence");
}

} // namespace

int main() {
    if (!test_vocabulary_and_key_bounds()) return 1;
    if (!test_duplicate_requests_and_determinism()) return 2;
    if (!test_stale_epoch_and_invalid_projection_fallbacks()) return 3;
    if (!test_pool_exhaustion_and_eviction()) return 4;
    if (!test_hard_caps_and_canonical_evidence()) return 5;
    std::cout << "shadow_page_contract_tests: ok\n";
    return 0;
}
