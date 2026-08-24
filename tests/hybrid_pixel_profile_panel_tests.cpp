#include "editor/hybrid_pixel_profile_panel.hpp"

#include <cmath>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

noemancer::HybridPixelProfile profile() {
    noemancer::HybridPixelProfile result;
    result.profile_id = "panel.acceptance";
    result.virtual_width = 320U;
    result.virtual_height = 180U;
    result.pixels_per_unit = 16.0F;
    return result;
}

bool near(const float left, const float right) {
    return std::abs(left - right) <= 1.0e-6F;
}

int fail(const char* message, const int code) {
    std::cerr << "hybrid_pixel_profile_panel_tests: " << message << '\n';
    return code;
}

} // namespace

int main() {
    using namespace noemancer;

    HybridPixelProfilePanel panel(
        HybridPixelProfileSnapshot{.revision = 7U, .profile = std::nullopt},
        HybridPixelProfilePreviewSurface{.width = 1440U, .height = 900U});
    const auto initial = panel.state();
    if (initial.snapshot.revision != 7U || initial.draft.profile ||
        !initial.validation.valid || !initial.preview.valid ||
        initial.preview.hybrid_pixel_active ||
        initial.preview.code != "hybrid-pixel-profile.absent") {
        return fail("optional absent profile did not produce a stable Raster projection", 1);
    }

    const auto initial_json = panel.semantic_state_json();
    if (initial_json != panel.semantic_state_json() ||
        initial_json.find("noemancer.hybrid-pixel-profile-panel/0.1") == std::string::npos ||
        initial_json.find("editor.project-settings.hybrid-pixel-profile.apply") == std::string::npos) {
        return fail("semantic state JSON was not stable and Agent-readable", 2);
    }

    if (!panel.create_default_profile()) {
        return fail("draft setup failed", 3);
    }
    panel.set_draft(profile());
    panel.set_preview_extent(1440U, 900U);
    const auto& preview = panel.preview();
    if (!preview.valid || !preview.hybrid_pixel_active ||
        preview.virtual_width != 320U || preview.virtual_height != 180U ||
        !near(preview.aspect_ratio, 16.0F / 9.0F) ||
        !near(preview.pixels_per_unit, 16.0F) || preview.integer_scale != 4U ||
        preview.presented_width != 1280U || preview.presented_height != 720U ||
        preview.letterbox_left != 80U || preview.letterbox_right != 80U ||
        preview.letterbox_top != 90U || preview.letterbox_bottom != 90U) {
        return fail("integer scale or letterbox preview did not match Pixel Presentation semantics", 4);
    }

    if (!panel.set_virtual_width(0U) || !panel.set_presentation_filter("linear") ||
        panel.validation().valid || panel.validation().diagnostics.size() < 2U) {
        return fail("field-level profile diagnostics were not projected", 5);
    }
    if (panel.request_apply() || panel.consume_request()) {
        return fail("invalid draft was allowed to queue an apply request", 6);
    }

    if (!panel.set_virtual_width(320U) || !panel.set_presentation_filter("nearest") ||
        !panel.request_apply(true)) {
        return fail("valid dry-run apply request was not queued", 7);
    }
    const auto apply = panel.consume_request();
    if (!apply || apply->kind != HybridPixelProfilePanelRequestKind::apply ||
        apply->expected_revision != 7U || apply->base_revision != 7U ||
        !apply->dry_run || !apply->profile || apply->profile->virtual_width != 320U ||
        apply->request_id.empty() || panel.consume_request()) {
        return fail("apply request did not preserve revision, dry-run and draft profile", 8);
    }

    if (!panel.request_disable_remove()) return fail("disable/remove request failed", 9);
    const auto disable = panel.consume_request();
    if (!disable || disable->kind != HybridPixelProfilePanelRequestKind::disable_remove ||
        disable->profile || disable->expected_revision != 7U) {
        return fail("disable/remove request did not carry an optional null profile", 10);
    }

    if (!panel.request_undo(true)) return fail("undo request failed", 11);
    const auto undo = panel.consume_request();
    if (!undo || undo->kind != HybridPixelProfilePanelRequestKind::undo ||
        !undo->dry_run || undo->profile) {
        return fail("undo request was not plain-data and dry-run capable", 12);
    }
    if (!panel.request_redo()) return fail("redo request failed", 13);
    const auto redo = panel.consume_request();
    if (!redo || redo->kind != HybridPixelProfilePanelRequestKind::redo || redo->profile) {
        return fail("redo request was not revision-bound", 14);
    }

    if (!panel.request(HybridPixelProfilePanelRequestKind::apply, 7U, false)) {
        return fail("generic apply request did not default to the current draft", 15);
    }
    const auto generic_apply = panel.consume_request();
    if (!generic_apply || generic_apply->kind != HybridPixelProfilePanelRequestKind::apply ||
        !generic_apply->profile || generic_apply->expected_revision != 7U) {
        return fail("generic apply request did not preserve the current draft", 16);
    }

    if (!panel.request(99U, false, profile())) return fail("explicit revision request failed", 17);
    panel.set_snapshot(HybridPixelProfileSnapshot{.revision = 100U, .profile = profile()});
    if (panel.state().has_pending_request || panel.state().last_error.empty() ||
        panel.snapshot().revision != 100U || !panel.draft().profile) {
        return fail("snapshot revision change did not discard stale pending request", 18);
    }

    panel.set_undo_redo_available(true, false);
    if (!panel.state().can_undo || panel.state().can_redo) {
        return fail("undo/redo availability was not projected", 19);
    }

    panel.set_preview_extent(160U, 90U);
    const auto& undersized = panel.preview();
    if (!undersized.valid || !undersized.undersized || undersized.integer_scale != 1U ||
        undersized.presented_width != 160U || undersized.presented_height != 90U) {
        return fail("undersized preview did not preserve centered 1:1 behavior", 20);
    }

    std::cout << "hybrid_pixel_profile_panel_tests: ok\n";
    return 0;
}
