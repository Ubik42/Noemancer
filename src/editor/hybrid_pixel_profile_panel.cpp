#include "editor/hybrid_pixel_profile_panel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::ordered_json;

Json profile_json(const HybridPixelProfile& profile) {
    return Json{{"schema", profile.schema},
                {"profileId", profile.profile_id},
                {"enabled", profile.enabled},
                {"virtualWidth", profile.virtual_width},
                {"virtualHeight", profile.virtual_height},
                {"pixelsPerUnit", profile.pixels_per_unit},
                {"integerScaling", profile.integer_scaling},
                {"snapCamera", profile.snap_camera},
                {"snapSprites", profile.snap_sprites},
                {"presentationFilter", profile.presentation_filter}};
}

Json diagnostic_json(const HybridPixelProfileError& diagnostic) {
    return Json{{"code", diagnostic.code},
                {"path", diagnostic.path},
                {"message", diagnostic.message}};
}

std::uint64_t stable_hash(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto value : text) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

std::string hex_hash(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

std::uint32_t scaled_extent(const std::uint32_t extent,
                            const std::uint32_t scale) noexcept {
    const auto value = static_cast<std::uint64_t>(extent) * scale;
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return 0U;
    }
    return static_cast<std::uint32_t>(value);
}

void set_letterbox(HybridPixelProfilePanelPreview& preview) noexcept {
    if (preview.physical_width < preview.presented_width ||
        preview.physical_height < preview.presented_height) {
        preview.letterbox_left = 0U;
        preview.letterbox_top = 0U;
        preview.letterbox_right = 0U;
        preview.letterbox_bottom = 0U;
        return;
    }
    const auto remaining_width = preview.physical_width - preview.presented_width;
    const auto remaining_height = preview.physical_height - preview.presented_height;
    preview.letterbox_left = remaining_width / 2U;
    preview.letterbox_top = remaining_height / 2U;
    preview.letterbox_right = remaining_width - preview.letterbox_left;
    preview.letterbox_bottom = remaining_height - preview.letterbox_top;
}

Json preview_json(const HybridPixelProfilePanelPreview& preview) {
    return Json{{"profilePresent", preview.profile_present},
                {"hybridPixelActive", preview.hybrid_pixel_active},
                {"valid", preview.valid},
                {"undersized", preview.undersized},
                {"code", preview.code},
                {"detail", preview.detail},
                {"virtualWidth", preview.virtual_width},
                {"virtualHeight", preview.virtual_height},
                {"aspectRatio", preview.aspect_ratio},
                {"pixelsPerUnit", preview.pixels_per_unit},
                {"integerScaling", preview.integer_scaling},
                {"physicalWidth", preview.physical_width},
                {"physicalHeight", preview.physical_height},
                {"integerScale", preview.integer_scale},
                {"presentedWidth", preview.presented_width},
                {"presentedHeight", preview.presented_height},
                {"letterbox", Json{{"left", preview.letterbox_left},
                                    {"top", preview.letterbox_top},
                                    {"right", preview.letterbox_right},
                                    {"bottom", preview.letterbox_bottom}}}};
}

} // namespace

std::string_view hybrid_pixel_profile_panel_request_kind_name(
    const HybridPixelProfilePanelRequestKind kind) noexcept {
    switch (kind) {
    case HybridPixelProfilePanelRequestKind::apply:
        return "apply";
    case HybridPixelProfilePanelRequestKind::disable_remove:
        return "disable-remove";
    case HybridPixelProfilePanelRequestKind::undo:
        return "undo";
    case HybridPixelProfilePanelRequestKind::redo:
        return "redo";
    }
    return "unknown";
}

HybridPixelProfilePanel::HybridPixelProfilePanel(
    HybridPixelProfileSnapshot snapshot,
    HybridPixelProfilePreviewSurface preview_surface)
    : snapshot_(std::move(snapshot)),
      draft_{snapshot_.profile},
      preview_surface_(preview_surface) {
    rebuild_projection();
}

const HybridPixelProfileSnapshot& HybridPixelProfilePanel::snapshot() const noexcept {
    return snapshot_;
}

const HybridPixelProfileDraft& HybridPixelProfilePanel::draft() const noexcept {
    return draft_;
}

const HybridPixelProfilePanelValidation& HybridPixelProfilePanel::validation() const noexcept {
    return validation_;
}

const HybridPixelProfilePanelPreview& HybridPixelProfilePanel::preview() const noexcept {
    return preview_;
}

HybridPixelProfilePanelState HybridPixelProfilePanel::state() const {
    return {.snapshot = snapshot_,
            .draft = draft_,
            .validation = validation_,
            .preview = preview_,
            .can_undo = can_undo_,
            .can_redo = can_redo_,
            .has_pending_request = pending_request_.has_value(),
            .pending_request = pending_request_,
            .last_error = last_error_};
}

void HybridPixelProfilePanel::set_snapshot(HybridPixelProfileSnapshot snapshot) {
    const auto revision_changed = snapshot.revision != snapshot_.revision;
    if (pending_request_ &&
        pending_request_->expected_revision != snapshot.revision) {
        pending_request_.reset();
        set_error("The pending Hybrid Pixel request was discarded because the snapshot revision changed.");
    } else if (revision_changed) {
        pending_request_.reset();
    }

    snapshot_ = std::move(snapshot);
    draft_.profile = snapshot_.profile;
    rebuild_projection();
}

void HybridPixelProfilePanel::set_snapshot(
    const std::uint64_t revision,
    std::optional<HybridPixelProfile> profile) {
    set_snapshot({.revision = revision, .profile = std::move(profile)});
}

void HybridPixelProfilePanel::set_preview_surface(
    const HybridPixelProfilePreviewSurface surface) {
    preview_surface_ = surface;
    rebuild_projection();
}

void HybridPixelProfilePanel::set_preview_extent(const std::uint32_t width,
                                                 const std::uint32_t height) {
    set_preview_surface({.width = width, .height = height});
}

void HybridPixelProfilePanel::set_undo_redo_available(
    const bool can_undo, const bool can_redo) noexcept {
    can_undo_ = can_undo;
    can_redo_ = can_redo;
}

void HybridPixelProfilePanel::set_draft(
    std::optional<HybridPixelProfile> profile) {
    draft_.profile = std::move(profile);
    rebuild_projection();
}

bool HybridPixelProfilePanel::create_default_profile() {
    if (!draft_.profile) {
        draft_.profile = HybridPixelProfile{};
        rebuild_projection();
    }
    return true;
}

void HybridPixelProfilePanel::clear_draft() {
    draft_.profile.reset();
    rebuild_projection();
}

bool HybridPixelProfilePanel::ensure_draft_profile() {
    if (!draft_.profile) {
        draft_.profile.emplace();
    }
    return true;
}

bool HybridPixelProfilePanel::set_profile_id(std::string profile_id) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->profile_id = std::move(profile_id);
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_enabled(const bool enabled) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->enabled = enabled;
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_virtual_width(const std::uint32_t width) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->virtual_width = width;
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_virtual_height(const std::uint32_t height) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->virtual_height = height;
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_pixels_per_unit(const float pixels_per_unit) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->pixels_per_unit = pixels_per_unit;
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_integer_scaling(const bool integer_scaling) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->integer_scaling = integer_scaling;
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_snap_camera(const bool snap_camera) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->snap_camera = snap_camera;
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_snap_sprites(const bool snap_sprites) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->snap_sprites = snap_sprites;
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::set_presentation_filter(
    std::string presentation_filter) {
    if (!ensure_draft_profile()) return false;
    draft_.profile->presentation_filter = std::move(presentation_filter);
    rebuild_projection();
    return true;
}

bool HybridPixelProfilePanel::request(const bool dry_run) {
    return request(snapshot_.revision, dry_run, draft_.profile);
}

bool HybridPixelProfilePanel::request(const std::uint64_t expected_revision,
                                     const bool dry_run) {
    return request(expected_revision, dry_run, draft_.profile);
}

bool HybridPixelProfilePanel::request(
    const std::uint64_t expected_revision, const bool dry_run,
    std::optional<HybridPixelProfile> profile) {
    const auto kind = profile.has_value()
        ? HybridPixelProfilePanelRequestKind::apply
        : HybridPixelProfilePanelRequestKind::disable_remove;
    return request(kind, expected_revision, dry_run, std::move(profile));
}

bool HybridPixelProfilePanel::request(
    const HybridPixelProfilePanelRequestKind kind,
    const std::uint64_t expected_revision,
    const bool dry_run,
    std::optional<HybridPixelProfile> profile) {
    if (kind == HybridPixelProfilePanelRequestKind::apply) {
        if (!profile && draft_.profile) {
            profile = draft_.profile;
        }
        if (!profile) {
            set_error("Apply requires a Hybrid Pixel profile draft.");
            return false;
        }
        const auto diagnostics = HybridPixelProfileCodec::validate(*profile);
        if (!diagnostics.empty()) {
            set_error("Cannot queue an invalid Hybrid Pixel profile draft.");
            return false;
        }
    } else if (kind == HybridPixelProfilePanelRequestKind::disable_remove) {
        profile.reset();
    } else if (profile) {
        set_error("Undo and redo requests do not carry a profile draft.");
        return false;
    }

    HybridPixelProfilePanelRequest request_value{
        .kind = kind,
        .request_id = make_request_id(kind, expected_revision, dry_run, profile),
        .expected_revision = expected_revision,
        .base_revision = expected_revision,
        .dry_run = dry_run,
        .profile = std::move(profile)};
    return queue_request(std::move(request_value));
}

bool HybridPixelProfilePanel::request_apply(const bool dry_run) {
    return request(HybridPixelProfilePanelRequestKind::apply,
                   snapshot_.revision, dry_run, draft_.profile);
}

bool HybridPixelProfilePanel::request_disable_remove(const bool dry_run) {
    return request(HybridPixelProfilePanelRequestKind::disable_remove,
                   snapshot_.revision, dry_run, std::nullopt);
}

bool HybridPixelProfilePanel::request_undo(const bool dry_run) {
    return request(HybridPixelProfilePanelRequestKind::undo,
                   snapshot_.revision, dry_run, std::nullopt);
}

bool HybridPixelProfilePanel::request_redo(const bool dry_run) {
    return request(HybridPixelProfilePanelRequestKind::redo,
                   snapshot_.revision, dry_run, std::nullopt);
}

std::optional<HybridPixelProfilePanelRequest>
HybridPixelProfilePanel::consume_request() {
    auto result = std::move(pending_request_);
    pending_request_.reset();
    return result;
}

std::string HybridPixelProfilePanel::semantic_state_json() const {
    Json root;
    root["schema"] = hybrid_pixel_profile_panel_schema;
    root["nodeId"] = hybrid_pixel_profile_panel_node_id;

    Json snapshot;
    snapshot["revision"] = snapshot_.revision;
    snapshot["profile"] = snapshot_.profile ? profile_json(*snapshot_.profile) : Json(nullptr);
    root["snapshot"] = std::move(snapshot);

    Json draft;
    draft["profile"] = draft_.profile ? profile_json(*draft_.profile) : Json(nullptr);
    root["draft"] = std::move(draft);

    Json validation;
    validation["valid"] = validation_.valid;
    validation["profilePresent"] = validation_.profile_present;
    auto diagnostics = Json::array();
    for (const auto& diagnostic : validation_.diagnostics) {
        diagnostics.push_back(diagnostic_json(diagnostic));
    }
    validation["diagnostics"] = std::move(diagnostics);
    root["validation"] = std::move(validation);
    root["preview"] = preview_json(preview_);

    root["actions"] = Json{{"apply", hybrid_pixel_profile_panel_apply_id},
                            {"disableRemove", hybrid_pixel_profile_panel_disable_remove_id},
                            {"undo", hybrid_pixel_profile_panel_undo_id},
                            {"redo", hybrid_pixel_profile_panel_redo_id}};
    root["history"] = Json{{"canUndo", can_undo_}, {"canRedo", can_redo_}};

    if (pending_request_) {
        const auto& request_value = *pending_request_;
        Json request;
        request["kind"] = hybrid_pixel_profile_panel_request_kind_name(request_value.kind);
        request["requestId"] = request_value.request_id;
        request["expectedRevision"] = request_value.expected_revision;
        request["baseRevision"] = request_value.base_revision;
        request["dryRun"] = request_value.dry_run;
        request["profile"] = request_value.profile
            ? profile_json(*request_value.profile)
            : Json(nullptr);
        root["request"] = std::move(request);
    } else {
        root["request"] = Json(nullptr);
    }
    root["lastError"] = last_error_;
    return root.dump(2) + "\n";
}

void HybridPixelProfilePanel::rebuild_projection() {
    validation_ = {};
    validation_.profile_present = draft_.profile.has_value();
    if (draft_.profile) {
        validation_.diagnostics = HybridPixelProfileCodec::validate(*draft_.profile);
    }
    validation_.valid = validation_.diagnostics.empty();

    preview_ = {};
    preview_.profile_present = draft_.profile.has_value();
    preview_.physical_width = preview_surface_.width;
    preview_.physical_height = preview_surface_.height;

    if (!draft_.profile) {
        preview_.valid = true;
        preview_.code = "hybrid-pixel-profile.absent";
        preview_.detail = "No optional Hybrid Pixel profile is authored; ordinary Raster remains active.";
        return;
    }

    const auto& profile = *draft_.profile;
    preview_.hybrid_pixel_active = profile.enabled;
    preview_.valid = validation_.valid;
    preview_.virtual_width = profile.virtual_width;
    preview_.virtual_height = profile.virtual_height;
    preview_.pixels_per_unit = profile.pixels_per_unit;
    preview_.integer_scaling = profile.integer_scaling;
    if (profile.virtual_height != 0U) {
        preview_.aspect_ratio = static_cast<float>(profile.virtual_width) /
            static_cast<float>(profile.virtual_height);
    }

    if (!profile.enabled) {
        preview_.code = "hybrid-pixel-profile.raster-disabled";
        preview_.detail = "The authored profile is disabled; ordinary Raster presentation is active.";
        return;
    }
    if (profile.virtual_width == 0U || profile.virtual_height == 0U) {
        preview_.code = "hybrid-pixel-profile.preview-virtual-extent-zero";
        preview_.detail = "Preview requires non-zero virtual width and height.";
        preview_.valid = false;
        return;
    }
    if (!profile.integer_scaling) {
        preview_.code = "hybrid-pixel-profile.preview-integer-scaling-required";
        preview_.detail = "An enabled Hybrid Pixel preview requires integer scaling.";
        preview_.valid = false;
        return;
    }
    if (preview_surface_.width == 0U || preview_surface_.height == 0U) {
        preview_.code = "hybrid-pixel-profile.preview-surface-zero";
        preview_.detail = "Preview surface width and height must be non-zero.";
        preview_.valid = false;
        return;
    }

    if (preview_surface_.width < profile.virtual_width ||
        preview_surface_.height < profile.virtual_height) {
        // Match PixelPresentation's stable undersized contract: retain a 1:1
        // source crop rather than introducing fractional scaling.
        preview_.undersized = true;
        preview_.integer_scale = 1U;
        preview_.presented_width = std::min(preview_surface_.width,
                                            profile.virtual_width);
        preview_.presented_height = std::min(preview_surface_.height,
                                             profile.virtual_height);
        set_letterbox(preview_);
        preview_.code = validation_.valid
            ? "hybrid-pixel-profile.preview-undersized"
            : "hybrid-pixel-profile.preview-undersized-with-diagnostics";
        preview_.detail = "Physical preview is smaller than the virtual extent; a centered 1:1 crop is shown.";
        return;
    }

    preview_.integer_scale = std::min(
        preview_surface_.width / profile.virtual_width,
        preview_surface_.height / profile.virtual_height);
    if (preview_.integer_scale == 0U) {
        preview_.code = "hybrid-pixel-profile.preview-integer-scale-zero";
        preview_.detail = "Preview could not derive a positive integer scale.";
        preview_.valid = false;
        return;
    }
    preview_.presented_width = scaled_extent(profile.virtual_width,
                                             preview_.integer_scale);
    preview_.presented_height = scaled_extent(profile.virtual_height,
                                              preview_.integer_scale);
    if (preview_.presented_width == 0U || preview_.presented_height == 0U) {
        preview_.code = "hybrid-pixel-profile.preview-geometry-overflow";
        preview_.detail = "Integer-scaled preview geometry exceeds the supported extent.";
        preview_.valid = false;
        return;
    }
    set_letterbox(preview_);
    preview_.code = validation_.valid
        ? "hybrid-pixel-profile.preview"
        : "hybrid-pixel-profile.preview-with-diagnostics";
    preview_.detail = validation_.valid
        ? "Virtual pixels are centered with deterministic integer scaling and letterbox margins."
        : "Preview geometry is shown while field diagnostics remain unresolved.";
}

bool HybridPixelProfilePanel::queue_request(
    HybridPixelProfilePanelRequest request_value) {
    if (pending_request_) {
        set_error("Consume the pending Hybrid Pixel request before issuing another request.");
        return false;
    }
    pending_request_ = std::move(request_value);
    last_error_.clear();
    return true;
}

std::string HybridPixelProfilePanel::make_request_id(
    const HybridPixelProfilePanelRequestKind kind,
    const std::uint64_t expected_revision,
    const bool dry_run,
    const std::optional<HybridPixelProfile>& profile) const {
    const auto payload = profile
        ? HybridPixelProfileCodec::write_canonical_json(*profile)
        : std::string("null");
    return std::string(hybrid_pixel_profile_panel_node_id) + "." +
        std::string(hybrid_pixel_profile_panel_request_kind_name(kind)) + "." +
        std::to_string(expected_revision) + "." + (dry_run ? "dry-run" : "apply") + "." +
        hex_hash(stable_hash(payload));
}

void HybridPixelProfilePanel::set_error(std::string message) {
    last_error_ = std::move(message);
}

void HybridPixelProfilePanel::render() {
    ImGui::SetNextWindowSize({640.0F, 760.0F}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings / Hybrid Pixel")) {
        ImGui::End();
        return;
    }

    ImGui::Text("Revision %llu  |  %s  |  %zu diagnostics",
                static_cast<unsigned long long>(snapshot_.revision),
                draft_.profile ? (draft_.profile->enabled ? "enabled" : "disabled") : "not authored",
                validation_.diagnostics.size());

    ImGui::SeparatorText("Hybrid Pixel Profile");
    if (!draft_.profile) {
        ImGui::TextDisabled("No optional Hybrid Pixel profile is authored.");
        if (ImGui::SmallButton("Create Default Profile")) {
            static_cast<void>(create_default_profile());
        }
    } else {
        auto& profile = *draft_.profile;
        bool enabled = profile.enabled;
        if (ImGui::Checkbox("Enabled", &enabled)) static_cast<void>(set_enabled(enabled));

        std::array<char, 129> profile_id{};
        std::snprintf(profile_id.data(), profile_id.size(), "%s", profile.profile_id.c_str());
        if (ImGui::InputText("Profile ID", profile_id.data(), profile_id.size())) {
            static_cast<void>(set_profile_id(profile_id.data()));
        }

        int width = static_cast<int>(profile.virtual_width);
        int height = static_cast<int>(profile.virtual_height);
        if (ImGui::InputInt("Virtual width", &width)) {
            static_cast<void>(set_virtual_width(width < 0 ? 0U : static_cast<std::uint32_t>(width)));
        }
        if (ImGui::InputInt("Virtual height", &height)) {
            static_cast<void>(set_virtual_height(height < 0 ? 0U : static_cast<std::uint32_t>(height)));
        }
        float pixels_per_unit = profile.pixels_per_unit;
        if (ImGui::InputFloat("Pixels per unit", &pixels_per_unit, 0.5F, 4.0F, "%.3f")) {
            static_cast<void>(set_pixels_per_unit(pixels_per_unit));
        }
        bool integer_scaling = profile.integer_scaling;
        if (ImGui::Checkbox("Integer scaling", &integer_scaling)) {
            static_cast<void>(set_integer_scaling(integer_scaling));
        }
        bool snap_camera = profile.snap_camera;
        if (ImGui::Checkbox("Snap camera", &snap_camera)) {
            static_cast<void>(set_snap_camera(snap_camera));
        }
        bool snap_sprites = profile.snap_sprites;
        if (ImGui::Checkbox("Snap sprites", &snap_sprites)) {
            static_cast<void>(set_snap_sprites(snap_sprites));
        }
        std::array<char, 32> filter{};
        std::snprintf(filter.data(), filter.size(), "%s", profile.presentation_filter.c_str());
        if (ImGui::InputText("Presentation filter", filter.data(), filter.size())) {
            static_cast<void>(set_presentation_filter(filter.data()));
        }
        if (ImGui::SmallButton("Remove Profile Draft")) {
            clear_draft();
        }
    }

    ImGui::SeparatorText("Preview");
    ImGui::Text("Virtual %u x %u  |  aspect %.6f  |  PPU %.3f",
                preview_.virtual_width, preview_.virtual_height,
                preview_.aspect_ratio, preview_.pixels_per_unit);
    ImGui::Text("Surface %u x %u  |  integer scale %u  |  presented %u x %u",
                preview_.physical_width, preview_.physical_height,
                preview_.integer_scale, preview_.presented_width,
                preview_.presented_height);
    ImGui::Text("Letterbox L/T/R/B: %u / %u / %u / %u",
                preview_.letterbox_left, preview_.letterbox_top,
                preview_.letterbox_right, preview_.letterbox_bottom);
    if (!preview_.detail.empty()) ImGui::TextDisabled("%s", preview_.detail.c_str());

    if (!validation_.diagnostics.empty()) {
        ImGui::SeparatorText("Validation");
        for (const auto& diagnostic : validation_.diagnostics) {
            ImGui::TextColored({1.0F, 0.55F, 0.28F, 1.0F}, "%s %s: %s",
                               diagnostic.code.c_str(), diagnostic.path.c_str(),
                               diagnostic.message.c_str());
        }
    }

    ImGui::SeparatorText("Project Transaction");
    ImGui::BeginDisabled(!draft_.profile||!validation_.valid);
    if (ImGui::Button("Apply")) static_cast<void>(request_apply(false));
    ImGui::SameLine();
    if (ImGui::Button("Apply Dry Run")) static_cast<void>(request_apply(true));
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!snapshot_.profile);
    if (ImGui::Button("Remove Profile")) static_cast<void>(request_disable_remove(false));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_undo_);
    if (ImGui::Button("Undo")) static_cast<void>(request_undo(false));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo_);
    if (ImGui::Button("Redo")) static_cast<void>(request_redo(false));
    ImGui::EndDisabled();

    if (pending_request_) {
        ImGui::TextDisabled("Pending %s request at revision %llu (%s)",
                           std::string(hybrid_pixel_profile_panel_request_kind_name(
                               pending_request_->kind)).c_str(),
                           static_cast<unsigned long long>(pending_request_->expected_revision),
                           pending_request_->dry_run ? "dry-run" : "apply");
    }
    if (!last_error_.empty()) {
        ImGui::TextColored({1.0F, 0.35F, 0.30F, 1.0F}, "%s", last_error_.c_str());
    }
    ImGui::End();
}

} // namespace noemancer
