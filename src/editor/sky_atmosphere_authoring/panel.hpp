#pragma once

#include "engine/sky_atmosphere.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// The panel is a projection over the engine-owned atmosphere contract.  It
// owns an editor draft and emits revision-bound requests; it never persists a
// project document or mutates a renderer directly.
inline constexpr std::string_view sky_atmosphere_authoring_panel_schema =
    "noemancer.sky-atmosphere-authoring-panel/0.1";
inline constexpr std::string_view sky_atmosphere_authoring_panel_node_id =
    "editor.project-settings.sky-atmosphere";
inline constexpr std::string_view sky_atmosphere_authoring_apply_id =
    "editor.project-settings.sky-atmosphere.apply";
inline constexpr std::string_view sky_atmosphere_authoring_apply_dry_run_id =
    "editor.project-settings.sky-atmosphere.apply-dry-run";
inline constexpr std::string_view sky_atmosphere_authoring_disable_id =
    "editor.project-settings.sky-atmosphere.disable";
inline constexpr std::string_view sky_atmosphere_authoring_remove_id =
    "editor.project-settings.sky-atmosphere.remove";
inline constexpr std::string_view sky_atmosphere_authoring_undo_id =
    "editor.project-settings.sky-atmosphere.undo";
inline constexpr std::string_view sky_atmosphere_authoring_redo_id =
    "editor.project-settings.sky-atmosphere.redo";

struct SkyAtmosphereAuthoringSnapshot final {
    std::uint64_t revision{1U};
    std::optional<SkyAtmosphereSettings> settings;
    bool can_undo{};
    bool can_redo{};
};

struct SkyAtmosphereAuthoringDraft final {
    std::optional<SkyAtmosphereSettings> settings;
};

struct SkyAtmosphereAuthoringPreview final {
    bool settings_present{};
    bool enabled{};
    bool valid{};
    std::string code;
    std::string detail;
    std::string quality;
    std::string debug_view;
    std::array<float, 3> sun_direction{};
    float sun_intensity{};
    SkyAtmosphereLutBudget lut_budget{};
};

struct SkyAtmosphereAuthoringValidation final {
    bool valid{};
    bool settings_present{};
    std::vector<SkyAtmosphereDiagnostic> diagnostics;
};

enum class SkyAtmosphereAuthoringRequestKind : std::uint8_t {
    apply,
    disable,
    remove,
    undo,
    redo,
};

[[nodiscard]] std::string_view sky_atmosphere_authoring_request_kind_name(
    SkyAtmosphereAuthoringRequestKind kind) noexcept;

struct SkyAtmosphereAuthoringRequest final {
    SkyAtmosphereAuthoringRequestKind kind{SkyAtmosphereAuthoringRequestKind::apply};
    std::string request_id;
    std::uint64_t expected_revision{};
    std::uint64_t base_revision{};
    bool dry_run{};
    std::optional<SkyAtmosphereSettings> settings;
};

// A request is a plain-data seam between the panel and the surrounding
// Project/World transaction authority.  This receipt is also usable by a
// headless authoring session when the editor is not running.
struct SkyAtmosphereAuthoringReceipt final {
    bool success{};
    bool changed{};
    bool committed{};
    bool dry_run{};
    std::string operation;
    std::string code;
    std::string detail;
    std::uint64_t revision{};
    std::optional<SkyAtmosphereSettings> settings;
    bool can_undo{};
    bool can_redo{};
    std::vector<SkyAtmosphereDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
    [[nodiscard]] std::string to_json() const;
};

struct SkyAtmosphereAuthoringEditOptions final {
    std::optional<std::uint64_t> expected_revision;
    bool dry_run{};
};

// Engine-contract-backed authoring authority.  It deliberately has the same
// transaction semantics as the existing project authoring sessions while
// remaining independent of a manifest path.  The surrounding project owner
// can consume a successful receipt and persist the canonical settings JSON.
class SkyAtmosphereAuthoringSession final {
public:
    explicit SkyAtmosphereAuthoringSession(
        std::optional<SkyAtmosphereSettings> settings = {},
        std::uint64_t revision = 1U);

    [[nodiscard]] const std::optional<SkyAtmosphereSettings>& settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] SkyAtmosphereAuthoringSnapshot snapshot() const;

    [[nodiscard]] SkyAtmosphereAuthoringReceipt replace(
        std::optional<SkyAtmosphereSettings> settings,
        SkyAtmosphereAuthoringEditOptions options = {});
    [[nodiscard]] SkyAtmosphereAuthoringReceipt apply(
        SkyAtmosphereSettings settings,
        SkyAtmosphereAuthoringEditOptions options = {});
    [[nodiscard]] SkyAtmosphereAuthoringReceipt disable(
        SkyAtmosphereAuthoringEditOptions options = {});
    [[nodiscard]] SkyAtmosphereAuthoringReceipt remove(
        SkyAtmosphereAuthoringEditOptions options = {});
    [[nodiscard]] SkyAtmosphereAuthoringReceipt undo(
        SkyAtmosphereAuthoringEditOptions options = {});
    [[nodiscard]] SkyAtmosphereAuthoringReceipt redo(
        SkyAtmosphereAuthoringEditOptions options = {});

private:
    struct HistoryEntry final {
        std::optional<SkyAtmosphereSettings> before;
        std::optional<SkyAtmosphereSettings> after;
    };
    enum class HistoryDirection : std::uint8_t { replace, undo, redo };

    [[nodiscard]] SkyAtmosphereAuthoringReceipt commit_candidate(
        std::optional<SkyAtmosphereSettings> candidate,
        const SkyAtmosphereAuthoringEditOptions& options,
        std::string_view operation, HistoryDirection direction,
        std::optional<HistoryEntry> history_entry = {});
    [[nodiscard]] SkyAtmosphereAuthoringReceipt failure(
        std::string_view operation, std::string_view code,
        std::string_view detail,
        std::vector<SkyAtmosphereDiagnostic> diagnostics = {}) const;
    [[nodiscard]] SkyAtmosphereAuthoringReceipt success(
        std::string_view operation, std::string_view code,
        std::string_view detail, bool changed, bool committed,
        bool dry_run, std::uint64_t revision,
        std::optional<SkyAtmosphereSettings> settings,
        std::vector<SkyAtmosphereDiagnostic> diagnostics = {}) const;

    std::optional<SkyAtmosphereSettings> settings_;
    std::uint64_t revision_{1U};
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
};

// Headless-first controller for the retained/ImGui editor panel.  All state
// exposed by the panel is plain data and can be projected to an Agent without
// depending on the ImGui lifecycle.
class SkyAtmosphereAuthoringPanel final {
public:
    explicit SkyAtmosphereAuthoringPanel(SkyAtmosphereAuthoringSnapshot snapshot);

    [[nodiscard]] const SkyAtmosphereAuthoringSnapshot& snapshot() const noexcept;
    [[nodiscard]] const SkyAtmosphereAuthoringDraft& draft() const noexcept;
    [[nodiscard]] const SkyAtmosphereAuthoringValidation& validation() const noexcept;
    [[nodiscard]] const SkyAtmosphereAuthoringPreview& preview() const noexcept;

    void set_snapshot(SkyAtmosphereAuthoringSnapshot snapshot);
    void set_undo_redo_available(bool can_undo, bool can_redo) noexcept;
    void set_draft(std::optional<SkyAtmosphereSettings> settings);
    [[nodiscard]] bool create_default_settings(
        SkyAtmosphereQuality quality = SkyAtmosphereQuality::high);
    void clear_draft();

    [[nodiscard]] bool set_enabled(bool enabled);
    [[nodiscard]] bool set_quality(SkyAtmosphereQuality quality);
    [[nodiscard]] bool set_debug_view(SkyAtmosphereDebugView view);
    [[nodiscard]] bool set_sun_direction(std::array<float, 3> direction);
    [[nodiscard]] bool set_sun_intensity(float intensity);
    [[nodiscard]] bool set_rayleigh_scattering(std::array<float, 3> values);
    [[nodiscard]] bool set_mie_scattering(std::array<float, 3> values);
    [[nodiscard]] bool set_rayleigh_scale_height(float value);
    [[nodiscard]] bool set_mie_scale_height(float value);

    [[nodiscard]] bool request_apply(bool dry_run = false);
    [[nodiscard]] bool request_disable(bool dry_run = false);
    [[nodiscard]] bool request_remove(bool dry_run = false);
    [[nodiscard]] bool request_undo(bool dry_run = false);
    [[nodiscard]] bool request_redo(bool dry_run = false);
    [[nodiscard]] std::optional<SkyAtmosphereAuthoringRequest> consume_request();

    [[nodiscard]] std::string semantic_state_json() const;
    [[nodiscard]] std::string state_json() const { return semantic_state_json(); }
    [[nodiscard]] std::string_view last_error() const noexcept { return last_error_; }

    // Optional visual frontend.  Headless callers and tests do not need to
    // call it.
    void render();

private:
    void rebuild_projection();
    [[nodiscard]] bool ensure_draft_settings();
    [[nodiscard]] bool queue_request(SkyAtmosphereAuthoringRequest request);
    [[nodiscard]] std::string make_request_id(
        SkyAtmosphereAuthoringRequestKind kind, std::uint64_t revision,
        bool dry_run, const std::optional<SkyAtmosphereSettings>& settings) const;
    void set_error(std::string message);

    SkyAtmosphereAuthoringSnapshot snapshot_;
    SkyAtmosphereAuthoringDraft draft_;
    SkyAtmosphereAuthoringValidation validation_;
    SkyAtmosphereAuthoringPreview preview_;
    std::optional<SkyAtmosphereAuthoringRequest> pending_request_;
    std::string last_error_;
};

} // namespace noemancer
