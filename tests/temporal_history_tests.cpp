#include "engine/temporal_history.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

int fail(const char* message) {
    std::cerr << "temporal_history_tests: " << message << '\n';
    return 1;
}

using namespace noemancer;

TemporalHistoryIdentity identity(const std::uint64_t frame = 1U) {
    TemporalHistoryIdentity result;
    result.extent = {1920U, 1080U};
    result.format = "rgba16f";
    result.frame_index = frame;
    result.camera_identity = "camera.main";
    result.projection_identity = "perspective:fov=60";
    result.profile_identity = "raster.high";
    result.quality_identity = "quality.high";
    return result;
}

TemporalHistoryCommitResult publish(TemporalHistoryAuthority& authority,
                                    const TemporalHistoryConsumer consumer,
                                    const TemporalHistoryIdentity& value,
                                    const bool enabled = true) {
    TemporalHistoryRequest request;
    request.consumer = consumer;
    request.identity = value;
    request.history_enabled = enabled;
    const auto plan = authority.plan(request);
    if (!plan.valid) return {};
    const auto begin = authority.begin(plan);
    if (!begin.success) return {};
    return authority.commit({consumer, begin.transaction_id, begin.base_revision, true, {}});
}

bool has_reason(const TemporalHistoryResetMask mask,
                const TemporalHistoryResetReason reason) {
    return (mask & temporal_history_reset_mask(reason)) != 0U;
}

} // namespace

int main() {
    using namespace noemancer;

    if (temporal_history_schema != "noemancer.temporal-history/0.1" ||
        temporal_history_consumer_name(TemporalHistoryConsumer::taa) != "taa" ||
        temporal_history_consumer_from_string("ssgi") != TemporalHistoryConsumer::ssgi ||
        temporal_history_consumer_from_string("unknown").has_value() ||
        !temporal_history_consumer_valid(TemporalHistoryConsumer::rtgi) ||
        temporal_history_consumer_valid(static_cast<TemporalHistoryConsumer>(99U))) {
        return fail("schema or consumer vocabulary is not stable");
    }
    const auto reason_mask = TemporalHistoryResetReason::extent_changed |
                             TemporalHistoryResetReason::camera_changed;
    if (temporal_history_reset_reason_text(reason_mask) != "extent-changed|camera-changed" ||
        temporal_history_reset_reason_text(0U) != "none") {
        return fail("reset-reason vocabulary is not deterministic");
    }

    // First frame: no previous surface is eligible, but a successfully
    // produced current surface becomes usable by the next frame.
    TemporalHistoryAuthority authority;
    TemporalHistoryRequest first_request;
    first_request.identity = identity(1U);
    const auto first_plan = authority.plan(first_request);
    if (!first_plan.valid || !has_reason(first_plan.reset_reasons,
                                         TemporalHistoryResetReason::first_frame) ||
        first_plan.use_previous || first_plan.previous_valid ||
        first_plan.base_revision != 0U) {
        return fail("first-frame plan did not reject unavailable history");
    }
    const auto first_begin = authority.begin(first_plan);
    if (!first_begin.success || first_begin.transaction_id.empty() ||
        first_begin.use_previous || first_begin.previous_valid ||
        authority.state(TemporalHistoryConsumer::taa).lifecycle !=
            TemporalHistoryLifecycle::begun) {
        return fail("begin did not enter the explicit transaction state");
    }
    const auto first_commit = authority.commit(
        {TemporalHistoryConsumer::taa, first_begin.transaction_id, 0U, true, {}});
    const auto first_state = authority.state(TemporalHistoryConsumer::taa);
    if (!first_commit.success || first_commit.revision != 1U ||
        !first_state.current_valid || first_state.previous_valid ||
        first_state.lifecycle != TemporalHistoryLifecycle::idle ||
        first_state.commit_count != 1U || first_state.reset_count != 1U) {
        return fail("first-frame commit did not publish current validity");
    }

    // Consecutive frame: the previous surface is eligible and no reset is
    // requested.  TAA history can therefore be consumed without a special
    // renderer-side exception.
    auto second_identity = identity(2U);
    TemporalHistoryRequest second_request;
    second_request.identity = second_identity;
    second_request.expected_revision = 1U;
    const auto second_plan = authority.plan(second_request);
    if (!second_plan.valid || second_plan.reset_reasons != 0U ||
        !second_plan.use_previous || !second_plan.previous_valid) {
        return fail("consecutive frame did not reuse compatible history");
    }
    const auto second_begin = authority.begin(second_plan);
    const auto second_commit = authority.commit(
        {TemporalHistoryConsumer::taa, second_begin.transaction_id,
         second_begin.base_revision, true, {}});
    const auto second_state = authority.state(TemporalHistoryConsumer::taa);
    if (!second_begin.success || !second_commit.success || second_state.revision != 2U ||
        !second_state.current_valid || !second_state.previous_valid ||
        second_state.current.frame_index != 2U || second_state.previous.frame_index != 1U) {
        return fail("consecutive frame commit did not rotate current and previous");
    }

    // Frame discontinuity is independent from camera/resource identity.
    TemporalHistoryRequest discontinuity_request;
    discontinuity_request.identity = identity(4U);
    const auto discontinuity_plan = authority.plan(discontinuity_request);
    if (!discontinuity_plan.valid ||
        !has_reason(discontinuity_plan.reset_reasons,
                    TemporalHistoryResetReason::frame_discontinuity) ||
        discontinuity_plan.use_previous) {
        return fail("frame discontinuity did not reject history");
    }

    // Each identity dimension has its own stable reset reason.  Seed each
    // isolated authority with one committed frame so the test is not relying
    // on first-frame behavior.
    const auto reason_for = [](const TemporalHistoryIdentity& changed,
                               const TemporalHistoryResetReason expected_reason) {
        TemporalHistoryAuthority isolated;
        if (!publish(isolated, TemporalHistoryConsumer::taa, identity(1U)).success)
            return false;
        TemporalHistoryRequest request;
        request.identity = changed;
        const auto plan = isolated.plan(request);
        return plan.valid && has_reason(plan.reset_reasons, expected_reason) &&
               !plan.use_previous;
    };
    auto changed = identity(2U);
    changed.extent.width += 1U;
    if (!reason_for(changed, TemporalHistoryResetReason::extent_changed))
        return fail("extent identity change was not a reset reason");
    changed = identity(2U);
    changed.format = "r11g11b10f";
    if (!reason_for(changed, TemporalHistoryResetReason::format_changed))
        return fail("format identity change was not a reset reason");
    changed = identity(2U);
    changed.camera_identity = "camera.cut";
    if (!reason_for(changed, TemporalHistoryResetReason::camera_changed))
        return fail("camera identity change was not a reset reason");
    changed = identity(2U);
    changed.projection_identity = "orthographic:height=10";
    if (!reason_for(changed, TemporalHistoryResetReason::projection_changed))
        return fail("projection identity change was not a reset reason");
    changed = identity(2U);
    changed.profile_identity = "hybrid-pixel";
    if (!reason_for(changed, TemporalHistoryResetReason::profile_changed))
        return fail("profile identity change was not a reset reason");
    changed = identity(2U);
    changed.quality_identity = "quality.low";
    if (!reason_for(changed, TemporalHistoryResetReason::quality_changed))
        return fail("quality identity change was not a reset reason");

    // Policy-disabled histories never publish a usable current surface.  When
    // re-enabled, the next frame is a first frame rather than a stale reuse.
    TemporalHistoryAuthority policy_authority;
    if (!publish(policy_authority, TemporalHistoryConsumer::ssr, identity(1U)).success)
        return fail("could not seed SSR policy test");
    TemporalHistoryRequest disabled_request;
    disabled_request.consumer = TemporalHistoryConsumer::ssr;
    disabled_request.identity = identity(2U);
    disabled_request.history_enabled = false;
    const auto disabled_plan = policy_authority.plan(disabled_request);
    if (!disabled_plan.valid ||
        !has_reason(disabled_plan.reset_reasons,
                    TemporalHistoryResetReason::policy_disabled) ||
        disabled_plan.previous_valid || disabled_plan.use_previous) {
        return fail("disabled history policy was not explicit");
    }
    const auto disabled_begin = policy_authority.begin(disabled_plan);
    const auto disabled_commit = policy_authority.commit(
        {TemporalHistoryConsumer::ssr, disabled_begin.transaction_id,
         disabled_begin.base_revision, true, {}});
    if (!disabled_commit.success ||
        policy_authority.state(TemporalHistoryConsumer::ssr).current_valid) {
        return fail("disabled policy published stale history");
    }
    TemporalHistoryRequest reenabled_request;
    reenabled_request.consumer = TemporalHistoryConsumer::ssr;
    reenabled_request.identity = identity(3U);
    const auto reenabled_plan = policy_authority.plan(reenabled_request);
    if (!reenabled_plan.valid ||
        !has_reason(reenabled_plan.reset_reasons,
                    TemporalHistoryResetReason::first_frame)) {
        return fail("re-enabling policy did not require a first frame");
    }

    // Explicit reset is revisioned and does not affect another consumer.
    const auto taa_before_reset = authority.state(TemporalHistoryConsumer::taa);
    const auto ssr_before_reset = authority.state(TemporalHistoryConsumer::ssr);
    const auto reset = authority.reset({TemporalHistoryConsumer::taa,
                                        taa_before_reset.revision, 0U});
    const auto taa_after_reset = authority.state(TemporalHistoryConsumer::taa);
    if (!reset.success || !has_reason(reset.reset_reasons, TemporalHistoryResetReason::manual) ||
        taa_after_reset.revision != taa_before_reset.revision + 1U ||
        taa_after_reset.current_valid || taa_after_reset.previous_valid ||
        authority.state(TemporalHistoryConsumer::ssr).revision != ssr_before_reset.revision) {
        return fail("manual reset was not isolated and revisioned");
    }
    const auto stale_reset = authority.reset(
        {TemporalHistoryConsumer::taa, taa_before_reset.revision, 0U});
    if (stale_reset.success || stale_reset.code != "temporal-history.revision-conflict")
        return fail("stale manual reset was accepted");

    // Wrong transaction, wrong revision, invalid plans and commit-without-begin
    // must all fail without changing the authority.
    const auto unchanged = authority.state(TemporalHistoryConsumer::taa);
    const auto no_commit = authority.commit(
        {TemporalHistoryConsumer::taa, "missing", unchanged.revision, true, {}});
    if (no_commit.success || no_commit.code != "temporal-history.no-active-transaction" ||
        authority.state(TemporalHistoryConsumer::taa).revision != unchanged.revision) {
        return fail("commit without begin mutated state");
    }
    TemporalHistoryRequest begin_request;
    begin_request.identity = identity(10U);
    const auto begin_plan = authority.plan(begin_request);
    const auto begin_result = authority.begin(begin_plan);
    if (!begin_result.success) return fail("could not create transaction error probe");
    const auto wrong_transaction = authority.commit(
        {TemporalHistoryConsumer::taa, "wrong", begin_result.base_revision, true, {}});
    if (wrong_transaction.success ||
        wrong_transaction.code != "temporal-history.transaction-mismatch") {
        return fail("wrong transaction identity was accepted");
    }
    const auto wrong_revision = authority.commit(
        {TemporalHistoryConsumer::taa, begin_result.transaction_id,
         begin_result.base_revision + 1U, true, {}});
    if (wrong_revision.success || wrong_revision.code != "temporal-history.revision-conflict")
        return fail("wrong commit revision was accepted");
    const auto valid_commit = authority.commit(
        {TemporalHistoryConsumer::taa, begin_result.transaction_id,
         begin_result.base_revision, true, {}});
    if (!valid_commit.success) return fail("valid commit failed after rejected probes");

    TemporalHistoryRequest invalid_request;
    invalid_request.identity = identity(11U);
    invalid_request.identity.extent.width = 0U;
    const auto invalid_plan = authority.plan(invalid_request);
    if (invalid_plan.valid || invalid_plan.code != "temporal-history.invalid-identity" ||
        invalid_plan.diagnostics.empty()) {
        return fail("invalid extent escaped the bounded identity contract");
    }

    // A failed producer invalidates its output and is observable as a stable
    // output-invalid reset reason.
    TemporalHistoryRequest failed_request;
    failed_request.identity = identity(11U);
    const auto failed_plan = authority.plan(failed_request);
    const auto failed_begin = authority.begin(failed_plan);
    const auto failed_commit = authority.commit(
        {TemporalHistoryConsumer::taa, failed_begin.transaction_id,
         failed_begin.base_revision, false, {}});
    if (!failed_commit.success ||
        !has_reason(failed_commit.reset_reasons,
                    TemporalHistoryResetReason::output_invalid) ||
        authority.state(TemporalHistoryConsumer::taa).current_valid) {
        return fail("failed producer output was not invalidated");
    }

    // Evidence and fingerprint are deterministic.  Diagnostics are bounded
    // and do not perturb the state fingerprint.
    const auto before_errors = authority.fingerprint();
    for (std::size_t index = 0U; index < temporal_history_max_diagnostics + 8U; ++index)
        static_cast<void>(authority.commit({TemporalHistoryConsumer::taa, "missing", {}, true, {}}));
    const auto evidence = authority.canonical_evidence();
    if (authority.diagnostics().size() != temporal_history_max_diagnostics ||
        evidence.find("\"schema\":\"noemancer.temporal-history/0.1\"") == std::string::npos ||
        evidence.find("\"diagnosticsTruncated\":true") == std::string::npos ||
        authority.fingerprint() != before_errors ||
        temporal_history_canonical_evidence(authority) != evidence ||
        temporal_history_fingerprint(authority) != authority.fingerprint()) {
        return fail("canonical evidence or bounded diagnostics are unstable");
    }

    std::cout << "temporal_history_tests: schema=" << temporal_history_schema
              << " consumers=" << temporal_history_consumer_count
              << " resetReasons=" << temporal_history_reset_reason_text(reason_mask)
              << " diagnostics=" << authority.diagnostics().size() << '\n';
    return 0;
}
