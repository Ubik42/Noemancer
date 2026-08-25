#include "editor/sky_atmosphere_authoring/panel.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::ordered_json;
constexpr std::size_t maximum_history = 128U;

bool same_settings(const std::optional<SkyAtmosphereSettings>& left,
                   const std::optional<SkyAtmosphereSettings>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    return SkyAtmosphereSettingsCodec::write_canonical_json(*left) ==
        SkyAtmosphereSettingsCodec::write_canonical_json(*right);
}

Json settings_json(const std::optional<SkyAtmosphereSettings>& settings) {
    if (!settings) return nullptr;
    const auto parsed = Json::parse(
        SkyAtmosphereSettingsCodec::write_canonical_json(*settings), nullptr, false);
    return parsed.is_discarded() ? Json(nullptr) : parsed;
}

Json diagnostic_json(const SkyAtmosphereDiagnostic& diagnostic) {
    return Json{{"code", diagnostic.code}, {"path", diagnostic.path},
                {"message", diagnostic.message}};
}

Json rgb_json(const std::array<float, 3>& values) {
    return Json::array({values[0], values[1], values[2]});
}

Json budget_json(const SkyAtmosphereLutBudget& budget) {
    return Json{
        {"enabled", budget.enabled},
        {"transmittance", Json{{"width", budget.transmittance_width},
                                {"height", budget.transmittance_height},
                                {"samples", budget.transmittance_samples}}},
        {"multiScattering", Json{{"width", budget.multi_scattering_width},
                                   {"height", budget.multi_scattering_height},
                                   {"samples", budget.multi_scattering_samples}}},
        {"skyView", Json{{"width", budget.sky_view_width},
                          {"height", budget.sky_view_height},
                          {"samples", budget.sky_view_samples}}},
        {"cameraVolume", Json{{"width", budget.camera_volume_width},
                               {"height", budget.camera_volume_height},
                               {"slices", budget.camera_volume_slices},
                               {"samples", budget.aerial_perspective_samples}}},
        {"lutStorageBytes", budget.lut_storage_bytes}};
}

std::uint64_t stable_hash(const std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : text) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

std::string hex_hash(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

std::string request_settings_payload(
    const std::optional<SkyAtmosphereSettings>& settings) {
    return settings ? SkyAtmosphereSettingsCodec::write_canonical_json(*settings) : "null";
}

} // namespace

std::string_view sky_atmosphere_authoring_request_kind_name(
    const SkyAtmosphereAuthoringRequestKind kind) noexcept {
    switch (kind) {
    case SkyAtmosphereAuthoringRequestKind::apply: return "apply";
    case SkyAtmosphereAuthoringRequestKind::disable: return "disable";
    case SkyAtmosphereAuthoringRequestKind::remove: return "remove";
    case SkyAtmosphereAuthoringRequestKind::undo: return "undo";
    case SkyAtmosphereAuthoringRequestKind::redo: return "redo";
    }
    return "unknown";
}

std::string SkyAtmosphereAuthoringReceipt::to_json() const {
    Json result{{"success", success}, {"changed", changed},
                {"committed", committed}, {"dryRun", dry_run},
                {"operation", operation}, {"code", code}, {"detail", detail},
                {"revision", revision}, {"settings", settings_json(settings)},
                {"canUndo", can_undo}, {"canRedo", can_redo}};
    auto diagnostics_json = Json::array();
    for (const auto& diagnostic : diagnostics) diagnostics_json.push_back(diagnostic_json(diagnostic));
    result["diagnostics"] = std::move(diagnostics_json);
    return result.dump();
}

SkyAtmosphereAuthoringSession::SkyAtmosphereAuthoringSession(
    std::optional<SkyAtmosphereSettings> settings, const std::uint64_t revision)
    : settings_(std::move(settings)), revision_(revision == 0U ? 1U : revision) {}

SkyAtmosphereAuthoringSnapshot SkyAtmosphereAuthoringSession::snapshot() const {
    return {.revision = revision_, .settings = settings_, .can_undo = can_undo(),
            .can_redo = can_redo()};
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::failure(
    const std::string_view operation, const std::string_view code,
    const std::string_view detail,
    std::vector<SkyAtmosphereDiagnostic> diagnostics) const {
    return {.success = false,
            .changed = false,
            .committed = false,
            .dry_run = false,
            .operation = std::string(operation),
            .code = std::string(code),
            .detail = std::string(detail),
            .revision = revision_,
            .settings = settings_,
            .can_undo = can_undo(),
            .can_redo = can_redo(),
            .diagnostics = std::move(diagnostics)};
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::success(
    const std::string_view operation, const std::string_view code,
    const std::string_view detail, const bool changed, const bool committed,
    const bool dry_run, const std::uint64_t revision,
    std::optional<SkyAtmosphereSettings> settings,
    std::vector<SkyAtmosphereDiagnostic> diagnostics) const {
    return {.success = true,
            .changed = changed,
            .committed = committed,
            .dry_run = dry_run,
            .operation = std::string(operation),
            .code = std::string(code),
            .detail = std::string(detail),
            .revision = revision,
            .settings = std::move(settings),
            .can_undo = can_undo(),
            .can_redo = can_redo(),
            .diagnostics = std::move(diagnostics)};
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::commit_candidate(
    std::optional<SkyAtmosphereSettings> candidate,
    const SkyAtmosphereAuthoringEditOptions& options,
    const std::string_view operation, const HistoryDirection direction,
    std::optional<HistoryEntry> history_entry) {
    if (options.expected_revision && *options.expected_revision != revision_) {
        auto result = failure(operation, "sky-atmosphere.revision-conflict",
                              "The atmosphere revision changed; refresh before applying this edit.");
        result.dry_run = options.dry_run;
        return result;
    }

    std::vector<SkyAtmosphereDiagnostic> diagnostics;
    if (candidate) diagnostics = SkyAtmosphereSettingsCodec::validate(*candidate);
    if (!diagnostics.empty()) {
        auto result = failure(operation, "sky-atmosphere.invalid-settings",
                              "The atmosphere draft is invalid and cannot be committed.",
                              std::move(diagnostics));
        result.dry_run = options.dry_run;
        return result;
    }

    const auto changed = !same_settings(settings_, candidate);
    if (changed && revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return failure(operation, "sky-atmosphere.revision-exhausted",
                       "The atmosphere revision cannot advance further.");
    }
    if (options.dry_run) {
        return success(operation, "sky-atmosphere.edit.preview",
                       changed ? "The atmosphere edit passed validation and is ready to commit."
                               : "The atmosphere edit is a no-op.",
                       changed, false, true, revision_, std::move(candidate));
    }
    if (!changed) {
        return success(operation, "sky-atmosphere.edit.noop",
                       "The atmosphere edit did not change the authored settings.",
                       false, false, false, revision_, std::move(candidate));
    }

    const HistoryEntry entry = history_entry.value_or(HistoryEntry{settings_, candidate});
    settings_ = std::move(candidate);
    ++revision_;
    switch (direction) {
    case HistoryDirection::replace:
        undo_.push_back(entry);
        redo_.clear();
        break;
    case HistoryDirection::undo:
        if (!undo_.empty()) undo_.pop_back();
        redo_.push_back(entry);
        break;
    case HistoryDirection::redo:
        if (!redo_.empty()) redo_.pop_back();
        undo_.push_back(entry);
        break;
    }
    if (undo_.size() > maximum_history) undo_.erase(undo_.begin());
    if (redo_.size() > maximum_history) redo_.erase(redo_.begin());
    return success(operation, "sky-atmosphere.edit.committed",
                   "The atmosphere settings were published at the new revision.",
                   true, true, false, revision_, settings_);
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::replace(
    std::optional<SkyAtmosphereSettings> settings,
    const SkyAtmosphereAuthoringEditOptions options) {
    return commit_candidate(std::move(settings), options, "replace", HistoryDirection::replace);
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::apply(
    SkyAtmosphereSettings settings, const SkyAtmosphereAuthoringEditOptions options) {
    return replace(std::move(settings), options);
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::disable(
    const SkyAtmosphereAuthoringEditOptions options) {
    if (!settings_) return replace(std::nullopt, options);
    auto candidate = *settings_;
    candidate.enabled = false;
    return commit_candidate(std::move(candidate), options, "disable", HistoryDirection::replace);
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::remove(
    const SkyAtmosphereAuthoringEditOptions options) {
    return commit_candidate(std::nullopt, options, "remove", HistoryDirection::replace);
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::undo(
    const SkyAtmosphereAuthoringEditOptions options) {
    if (undo_.empty()) {
        auto result = failure("undo", "sky-atmosphere.undo-empty",
                              "There is no committed atmosphere edit to undo.");
        result.dry_run = options.dry_run;
        return result;
    }
    const auto entry = undo_.back();
    return commit_candidate(entry.before, options, "undo", HistoryDirection::undo, entry);
}

SkyAtmosphereAuthoringReceipt SkyAtmosphereAuthoringSession::redo(
    const SkyAtmosphereAuthoringEditOptions options) {
    if (redo_.empty()) {
        auto result = failure("redo", "sky-atmosphere.redo-empty",
                              "There is no undone atmosphere edit to redo.");
        result.dry_run = options.dry_run;
        return result;
    }
    const auto entry = redo_.back();
    return commit_candidate(entry.after, options, "redo", HistoryDirection::redo, entry);
}

SkyAtmosphereAuthoringPanel::SkyAtmosphereAuthoringPanel(
    SkyAtmosphereAuthoringSnapshot snapshot)
    : snapshot_(std::move(snapshot)), draft_{snapshot_.settings} {
    rebuild_projection();
}

const SkyAtmosphereAuthoringSnapshot& SkyAtmosphereAuthoringPanel::snapshot() const noexcept {
    return snapshot_;
}

const SkyAtmosphereAuthoringDraft& SkyAtmosphereAuthoringPanel::draft() const noexcept {
    return draft_;
}

const SkyAtmosphereAuthoringValidation& SkyAtmosphereAuthoringPanel::validation() const noexcept {
    return validation_;
}

const SkyAtmosphereAuthoringPreview& SkyAtmosphereAuthoringPanel::preview() const noexcept {
    return preview_;
}

void SkyAtmosphereAuthoringPanel::set_snapshot(SkyAtmosphereAuthoringSnapshot snapshot) {
    if (pending_request_ && pending_request_->expected_revision != snapshot.revision) {
        pending_request_.reset();
        set_error("The pending atmosphere request was discarded because the snapshot revision changed.");
    }
    snapshot_ = std::move(snapshot);
    draft_.settings = snapshot_.settings;
    rebuild_projection();
}

void SkyAtmosphereAuthoringPanel::set_undo_redo_available(
    const bool can_undo, const bool can_redo) noexcept {
    snapshot_.can_undo = can_undo;
    snapshot_.can_redo = can_redo;
}

void SkyAtmosphereAuthoringPanel::set_draft(
    std::optional<SkyAtmosphereSettings> settings) {
    draft_.settings = std::move(settings);
    rebuild_projection();
}

bool SkyAtmosphereAuthoringPanel::create_default_settings(
    const SkyAtmosphereQuality quality) {
    draft_.settings = make_sky_atmosphere_settings(quality);
    rebuild_projection();
    return true;
}

void SkyAtmosphereAuthoringPanel::clear_draft() {
    draft_.settings.reset();
    rebuild_projection();
}

bool SkyAtmosphereAuthoringPanel::ensure_draft_settings() {
    if (!draft_.settings) draft_.settings = make_sky_atmosphere_settings(SkyAtmosphereQuality::high);
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_enabled(const bool enabled) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->enabled = enabled;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_quality(const SkyAtmosphereQuality quality) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->quality = quality;
    if (quality == SkyAtmosphereQuality::off) draft_.settings->enabled = false;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_debug_view(const SkyAtmosphereDebugView view) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->debug_view = view;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_sun_direction(
    const std::array<float, 3> direction) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->sun_direction = direction;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_sun_intensity(const float intensity) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->sun_irradiance = {intensity, intensity, intensity};
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_rayleigh_scattering(
    const std::array<float, 3> values) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->rayleigh_scattering_per_m = values;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_mie_scattering(
    const std::array<float, 3> values) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->mie_scattering_per_m = values;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_rayleigh_scale_height(const float value) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->rayleigh_scale_height_m = value;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::set_mie_scale_height(const float value) {
    if (!ensure_draft_settings()) return false;
    draft_.settings->mie_scale_height_m = value;
    rebuild_projection();
    return true;
}

bool SkyAtmosphereAuthoringPanel::request_apply(const bool dry_run) {
    if (!draft_.settings) {
        set_error("Apply requires an authored atmosphere settings draft.");
        return false;
    }
    if (!validation_.valid) {
        set_error("Cannot queue an invalid atmosphere settings draft.");
        return false;
    }
    return queue_request({.kind = SkyAtmosphereAuthoringRequestKind::apply,
                          .request_id = make_request_id(SkyAtmosphereAuthoringRequestKind::apply,
                                                         snapshot_.revision, dry_run, draft_.settings),
                          .expected_revision = snapshot_.revision,
                          .base_revision = snapshot_.revision,
                          .dry_run = dry_run,
                          .settings = draft_.settings});
}

bool SkyAtmosphereAuthoringPanel::request_disable(const bool dry_run) {
    return queue_request({.kind = SkyAtmosphereAuthoringRequestKind::disable,
                          .request_id = make_request_id(SkyAtmosphereAuthoringRequestKind::disable,
                                                         snapshot_.revision, dry_run, std::nullopt),
                          .expected_revision = snapshot_.revision,
                          .base_revision = snapshot_.revision,
                          .dry_run = dry_run,
                          .settings = std::nullopt});
}

bool SkyAtmosphereAuthoringPanel::request_remove(const bool dry_run) {
    return queue_request({.kind = SkyAtmosphereAuthoringRequestKind::remove,
                          .request_id = make_request_id(SkyAtmosphereAuthoringRequestKind::remove,
                                                         snapshot_.revision, dry_run, std::nullopt),
                          .expected_revision = snapshot_.revision,
                          .base_revision = snapshot_.revision,
                          .dry_run = dry_run,
                          .settings = std::nullopt});
}

bool SkyAtmosphereAuthoringPanel::request_undo(const bool dry_run) {
    return queue_request({.kind = SkyAtmosphereAuthoringRequestKind::undo,
                          .request_id = make_request_id(SkyAtmosphereAuthoringRequestKind::undo,
                                                         snapshot_.revision, dry_run, std::nullopt),
                          .expected_revision = snapshot_.revision,
                          .base_revision = snapshot_.revision,
                          .dry_run = dry_run,
                          .settings = std::nullopt});
}

bool SkyAtmosphereAuthoringPanel::request_redo(const bool dry_run) {
    return queue_request({.kind = SkyAtmosphereAuthoringRequestKind::redo,
                          .request_id = make_request_id(SkyAtmosphereAuthoringRequestKind::redo,
                                                         snapshot_.revision, dry_run, std::nullopt),
                          .expected_revision = snapshot_.revision,
                          .base_revision = snapshot_.revision,
                          .dry_run = dry_run,
                          .settings = std::nullopt});
}

std::optional<SkyAtmosphereAuthoringRequest>
SkyAtmosphereAuthoringPanel::consume_request() {
    auto result = std::move(pending_request_);
    pending_request_.reset();
    return result;
}

std::string SkyAtmosphereAuthoringPanel::semantic_state_json() const {
    Json root{{"schema", sky_atmosphere_authoring_panel_schema},
              {"nodeId", sky_atmosphere_authoring_panel_node_id},
              {"title", "Sky Atmosphere"}};
    root["snapshot"] = Json{{"revision", snapshot_.revision},
                             {"settings", settings_json(snapshot_.settings)}};
    root["draft"] = Json{{"settings", settings_json(draft_.settings)}};

    Json validation{{"valid", validation_.valid},
                    {"settingsPresent", validation_.settings_present}};
    auto diagnostics = Json::array();
    for (const auto& diagnostic : validation_.diagnostics) diagnostics.push_back(diagnostic_json(diagnostic));
    validation["diagnostics"] = std::move(diagnostics);
    root["validation"] = std::move(validation);

    root["preview"] = Json{{"settingsPresent", preview_.settings_present},
                            {"enabled", preview_.enabled}, {"valid", preview_.valid},
                            {"code", preview_.code}, {"detail", preview_.detail},
                            {"quality", preview_.quality}, {"debugView", preview_.debug_view},
                            {"sunDirection", rgb_json(preview_.sun_direction)},
                            {"sunIntensity", preview_.sun_intensity},
                            {"lutBudget", budget_json(preview_.lut_budget)}};

    // Stable node IDs and intent bindings are the same vocabulary used by a
    // retained renderer and by Agent observations.  Values are projections,
    // never an independent authorization or scene state store.
    root["fields"] = Json::array({
        Json{{"nodeId", std::string(sky_atmosphere_authoring_panel_node_id) + ".enabled"},
             {"role", "checkbox"}, {"label", "Enabled"},
             {"binding", "settings.enabled"},
             {"value", draft_.settings ? Json(draft_.settings->enabled) : Json(nullptr)},
             {"intent", "set-atmosphere-enabled"}},
        Json{{"nodeId", std::string(sky_atmosphere_authoring_panel_node_id) + ".quality"},
             {"role", "select"}, {"label", "Quality"},
             {"binding", "settings.quality"},
             {"value", draft_.settings ? Json(sky_atmosphere_quality_name(draft_.settings->quality)) : Json(nullptr)},
             {"intent", "set-atmosphere-quality"}},
        Json{{"nodeId", std::string(sky_atmosphere_authoring_panel_node_id) + ".sun-direction"},
             {"role", "vector3"}, {"label", "Sun direction"},
             {"binding", "settings.physical.sunDirection"},
             {"value", draft_.settings ? rgb_json(draft_.settings->sun_direction) : Json(nullptr)},
             {"intent", "set-atmosphere-sun-direction"}},
        Json{{"nodeId", std::string(sky_atmosphere_authoring_panel_node_id) + ".sun-intensity"},
             {"role", "number"}, {"label", "Sun intensity"},
             {"binding", "settings.physical.sunIrradiance"},
             {"value", draft_.settings ? Json(preview_.sun_intensity) : Json(nullptr)},
             {"intent", "set-atmosphere-sun-intensity"}},
        Json{{"nodeId", std::string(sky_atmosphere_authoring_panel_node_id) + ".rayleigh"},
             {"role", "vector3"}, {"label", "Rayleigh scattering"},
             {"binding", "settings.physical.rayleighScatteringPerM"},
             {"value", draft_.settings ? rgb_json(draft_.settings->rayleigh_scattering_per_m) : Json(nullptr)},
             {"intent", "set-atmosphere-rayleigh-scattering"}},
        Json{{"nodeId", std::string(sky_atmosphere_authoring_panel_node_id) + ".mie"},
             {"role", "vector3"}, {"label", "Mie scattering"},
             {"binding", "settings.physical.mieScatteringPerM"},
             {"value", draft_.settings ? rgb_json(draft_.settings->mie_scattering_per_m) : Json(nullptr)},
             {"intent", "set-atmosphere-mie-scattering"}}});

    root["actions"] = Json{{"apply", sky_atmosphere_authoring_apply_id},
                            {"applyDryRun", sky_atmosphere_authoring_apply_dry_run_id},
                            {"disable", sky_atmosphere_authoring_disable_id},
                            {"remove", sky_atmosphere_authoring_remove_id},
                            {"undo", sky_atmosphere_authoring_undo_id},
                            {"redo", sky_atmosphere_authoring_redo_id}};
    root["history"] = Json{{"canUndo", snapshot_.can_undo}, {"canRedo", snapshot_.can_redo}};
    if (pending_request_) {
        root["request"] = Json{{"kind", sky_atmosphere_authoring_request_kind_name(pending_request_->kind)},
                                {"requestId", pending_request_->request_id},
                                {"expectedRevision", pending_request_->expected_revision},
                                {"baseRevision", pending_request_->base_revision},
                                {"dryRun", pending_request_->dry_run},
                                {"settings", settings_json(pending_request_->settings)}};
    } else {
        root["request"] = nullptr;
    }
    root["lastError"] = last_error_;
    return root.dump(2) + "\n";
}

void SkyAtmosphereAuthoringPanel::rebuild_projection() {
    validation_ = {};
    validation_.settings_present = draft_.settings.has_value();
    if (draft_.settings) validation_.diagnostics = SkyAtmosphereSettingsCodec::validate(*draft_.settings);
    validation_.valid = validation_.diagnostics.empty();

    preview_ = {};
    preview_.settings_present = draft_.settings.has_value();
    if (!draft_.settings) {
        preview_.valid = true;
        preview_.code = "sky-atmosphere.settings.absent";
        preview_.detail = "No authored atmosphere settings; the renderer may use its default background.";
        return;
    }

    const auto& settings = *draft_.settings;
    preview_.enabled = settings.enabled;
    preview_.quality = std::string(sky_atmosphere_quality_name(settings.quality));
    preview_.debug_view = std::string(sky_atmosphere_debug_view_name(settings.debug_view));
    preview_.sun_direction = settings.sun_direction;
    preview_.sun_intensity = (settings.sun_irradiance[0] + settings.sun_irradiance[1] +
                              settings.sun_irradiance[2]) / 3.0F;
    preview_.lut_budget = sky_atmosphere_quality_budget(settings.quality);
    preview_.valid = validation_.valid;
    preview_.code = preview_.valid ? "sky-atmosphere.preview"
                                   : "sky-atmosphere.preview-with-diagnostics";
    preview_.detail = preview_.valid
        ? "The authored atmosphere is valid; apply or dry-run uses the same engine contract."
        : "The preview remains visible while atmosphere validation diagnostics are unresolved.";
}

bool SkyAtmosphereAuthoringPanel::queue_request(SkyAtmosphereAuthoringRequest request) {
    if (pending_request_) {
        set_error("Consume the pending atmosphere request before issuing another request.");
        return false;
    }
    pending_request_ = std::move(request);
    last_error_.clear();
    return true;
}

std::string SkyAtmosphereAuthoringPanel::make_request_id(
    const SkyAtmosphereAuthoringRequestKind kind, const std::uint64_t revision,
    const bool dry_run, const std::optional<SkyAtmosphereSettings>& settings) const {
    const auto payload = std::string(sky_atmosphere_authoring_request_kind_name(kind)) + ":" +
        std::to_string(revision) + ":" + (dry_run ? "dry-run:" : "apply:") +
        request_settings_payload(settings);
    return std::string(sky_atmosphere_authoring_panel_node_id) + "." +
        std::string(sky_atmosphere_authoring_request_kind_name(kind)) + "." +
        std::to_string(revision) + "." + hex_hash(stable_hash(payload));
}

void SkyAtmosphereAuthoringPanel::set_error(std::string message) {
    last_error_ = std::move(message);
}

void SkyAtmosphereAuthoringPanel::render() {
    ImGui::SetNextWindowSize({640.0F, 720.0F}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings / Sky Atmosphere")) {
        ImGui::End();
        return;
    }
    ImGui::Text("Revision %llu  |  %s  |  %zu diagnostics",
                static_cast<unsigned long long>(snapshot_.revision),
                draft_.settings ? (draft_.settings->enabled ? "enabled" : "disabled") : "not authored",
                validation_.diagnostics.size());
    ImGui::SeparatorText("Atmosphere profile");
    if (!draft_.settings) {
        ImGui::TextDisabled("No authored Sky Atmosphere settings.");
        if (ImGui::Button("Create High Quality Defaults"))
            static_cast<void>(create_default_settings());
    } else {
        auto& settings = *draft_.settings;
        bool enabled = settings.enabled;
        if (ImGui::Checkbox("Enabled", &enabled)) static_cast<void>(set_enabled(enabled));

        int quality = static_cast<int>(settings.quality);
        const std::array<const char*, 5> quality_names{"Off", "Low", "Medium", "High", "Ultra"};
        if (ImGui::Combo("Quality", &quality, quality_names.data(), static_cast<int>(quality_names.size())))
            static_cast<void>(set_quality(static_cast<SkyAtmosphereQuality>(quality)));

        float direction[3]{settings.sun_direction[0], settings.sun_direction[1], settings.sun_direction[2]};
        if (ImGui::InputFloat3("Sun direction", direction, "%.6f"))
            static_cast<void>(set_sun_direction({direction[0], direction[1], direction[2]}));
        float intensity = preview_.sun_intensity;
        if (ImGui::InputFloat("Sun intensity", &intensity, 0.05F, 0.5F, "%.4f"))
            static_cast<void>(set_sun_intensity(intensity));
        float rayleigh[3]{settings.rayleigh_scattering_per_m[0], settings.rayleigh_scattering_per_m[1], settings.rayleigh_scattering_per_m[2]};
        if (ImGui::InputFloat3("Rayleigh scattering / m", rayleigh, "%.7g"))
            static_cast<void>(set_rayleigh_scattering({rayleigh[0], rayleigh[1], rayleigh[2]}));
        float mie[3]{settings.mie_scattering_per_m[0], settings.mie_scattering_per_m[1], settings.mie_scattering_per_m[2]};
        if (ImGui::InputFloat3("Mie scattering / m", mie, "%.7g"))
            static_cast<void>(set_mie_scattering({mie[0], mie[1], mie[2]}));
        float rayleigh_height = settings.rayleigh_scale_height_m;
        if (ImGui::InputFloat("Rayleigh scale height (m)", &rayleigh_height, 100.0F, 1000.0F, "%.1f"))
            static_cast<void>(set_rayleigh_scale_height(rayleigh_height));
        float mie_height = settings.mie_scale_height_m;
        if (ImGui::InputFloat("Mie scale height (m)", &mie_height, 50.0F, 500.0F, "%.1f"))
            static_cast<void>(set_mie_scale_height(mie_height));
        if (ImGui::Button("Remove Draft")) clear_draft();
    }

    ImGui::SeparatorText("Preview");
    ImGui::Text("Quality %s | LUT storage budget %llu bytes", preview_.quality.c_str(),
                static_cast<unsigned long long>(preview_.lut_budget.lut_storage_bytes));
    if (!preview_.detail.empty()) ImGui::TextDisabled("%s", preview_.detail.c_str());
    if (!validation_.diagnostics.empty()) {
        ImGui::SeparatorText("Validation");
        for (const auto& diagnostic : validation_.diagnostics)
            ImGui::TextColored({1.0F, 0.55F, 0.28F, 1.0F}, "%s %s: %s",
                               diagnostic.code.c_str(), diagnostic.path.c_str(), diagnostic.message.c_str());
    }

    ImGui::SeparatorText("Project transaction");
    ImGui::BeginDisabled(!draft_.settings || !validation_.valid);
    if (ImGui::Button("Apply")) static_cast<void>(request_apply(false));
    ImGui::SameLine();
    if (ImGui::Button("Apply Dry Run")) static_cast<void>(request_apply(true));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!snapshot_.settings);
    if (ImGui::Button("Disable")) static_cast<void>(request_disable(false));
    ImGui::SameLine();
    if (ImGui::Button("Remove")) static_cast<void>(request_remove(false));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!snapshot_.can_undo);
    if (ImGui::Button("Undo")) static_cast<void>(request_undo(false));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!snapshot_.can_redo);
    if (ImGui::Button("Redo")) static_cast<void>(request_redo(false));
    ImGui::EndDisabled();
    if (pending_request_)
        ImGui::TextDisabled("Pending %s request at revision %llu",
                           std::string(sky_atmosphere_authoring_request_kind_name(pending_request_->kind)).c_str(),
                           static_cast<unsigned long long>(pending_request_->expected_revision));
    if (!last_error_.empty())
        ImGui::TextColored({1.0F, 0.35F, 0.30F, 1.0F}, "%s", last_error_.c_str());
    ImGui::End();
}

} // namespace noemancer
