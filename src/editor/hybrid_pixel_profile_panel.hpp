#pragma once

#include "engine/hybrid_pixel_profile.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// The panel is a projection over the Project/Authority profile value.  It
// owns only an in-progress draft and emits requests; it never writes a
// manifest or mutates the engine-owned profile directly.
inline constexpr std::string_view hybrid_pixel_profile_panel_schema =
    "noemancer.hybrid-pixel-profile-panel/0.1";
inline constexpr std::string_view hybrid_pixel_profile_panel_node_id =
    "editor.project-settings.hybrid-pixel-profile";
inline constexpr std::string_view hybrid_pixel_profile_panel_apply_id =
    "editor.project-settings.hybrid-pixel-profile.apply";
inline constexpr std::string_view hybrid_pixel_profile_panel_disable_remove_id =
    "editor.project-settings.hybrid-pixel-profile.disable-remove";
inline constexpr std::string_view hybrid_pixel_profile_panel_undo_id =
    "editor.project-settings.hybrid-pixel-profile.undo";
inline constexpr std::string_view hybrid_pixel_profile_panel_redo_id =
    "editor.project-settings.hybrid-pixel-profile.redo";

// This is deliberately only revision plus the optional authoritative value.
// Project identity and persistence remain owned by the surrounding Project
// transaction authority.
struct HybridPixelProfileSnapshot final {
    std::uint64_t revision{1U};
    std::optional<HybridPixelProfile> profile;
};

// Keep the panel-specific spelling available to callers that group all panel
// values by type, while retaining the short Snapshot name used by Engine
// profile consumers.
using HybridPixelProfilePanelSnapshot = HybridPixelProfileSnapshot;

struct HybridPixelProfileDraft final {
    std::optional<HybridPixelProfile> profile;
};

using HybridPixelProfilePanelDraft = HybridPixelProfileDraft;

struct HybridPixelProfilePanelValidation final {
    bool valid{};
    bool profile_present{};
    std::vector<HybridPixelProfileError> diagnostics;
};

struct HybridPixelProfilePreviewSurface final {
    std::uint32_t width{1280U};
    std::uint32_t height{720U};
};

struct HybridPixelProfilePanelPreview final {
    bool profile_present{};
    bool hybrid_pixel_active{};
    bool valid{};
    bool undersized{};
    std::string code;
    std::string detail;

    std::uint32_t virtual_width{};
    std::uint32_t virtual_height{};
    float aspect_ratio{};
    float pixels_per_unit{};
    bool integer_scaling{};

    std::uint32_t physical_width{};
    std::uint32_t physical_height{};
    std::uint32_t integer_scale{};
    std::uint32_t presented_width{};
    std::uint32_t presented_height{};
    std::uint32_t letterbox_left{};
    std::uint32_t letterbox_top{};
    std::uint32_t letterbox_right{};
    std::uint32_t letterbox_bottom{};
};

enum class HybridPixelProfilePanelRequestKind : std::uint8_t {
    apply,
    disable_remove,
    undo,
    redo,

    // Compatibility aliases keep the intent obvious to callers that use a
    // single-word operation name.  They serialize as "disable-remove".
    disable = disable_remove,
    remove = disable_remove,
};

[[nodiscard]] std::string_view hybrid_pixel_profile_panel_request_kind_name(
    HybridPixelProfilePanelRequestKind kind) noexcept;

struct HybridPixelProfilePanelRequest final {
    HybridPixelProfilePanelRequestKind kind{
        HybridPixelProfilePanelRequestKind::apply};
    std::string request_id;

    // Both names are carried so a Project transaction adapter can map the
    // request without inventing a second revision convention.  They always
    // contain the same value when emitted by this controller.
    std::uint64_t expected_revision{};
    std::uint64_t base_revision{};
    bool dry_run{};
    std::optional<HybridPixelProfile> profile;
};

struct HybridPixelProfilePanelState final {
    HybridPixelProfileSnapshot snapshot;
    HybridPixelProfileDraft draft;
    HybridPixelProfilePanelValidation validation;
    HybridPixelProfilePanelPreview preview;
    bool can_undo{};
    bool can_redo{};
    bool has_pending_request{};
    std::optional<HybridPixelProfilePanelRequest> pending_request;
    std::string last_error;
};

// Headless-first editor controller.  Every method except render() is free of
// ImGui types and can be used by tests, Agent projections and retained UI.
class HybridPixelProfilePanel final {
public:
    explicit HybridPixelProfilePanel(
        HybridPixelProfileSnapshot snapshot,
        HybridPixelProfilePreviewSurface preview_surface = {});

    [[nodiscard]] const HybridPixelProfileSnapshot& snapshot() const noexcept;
    [[nodiscard]] const HybridPixelProfileDraft& draft() const noexcept;
    [[nodiscard]] const HybridPixelProfilePanelValidation& validation() const noexcept;
    [[nodiscard]] const HybridPixelProfilePanelPreview& preview() const noexcept;
    [[nodiscard]] HybridPixelProfilePanelState state() const;

    void set_snapshot(HybridPixelProfileSnapshot snapshot);
    void set_snapshot(std::uint64_t revision,
                      std::optional<HybridPixelProfile> profile);
    void set_preview_surface(HybridPixelProfilePreviewSurface surface);
    void set_preview_extent(std::uint32_t width, std::uint32_t height);
    void set_undo_redo_available(bool can_undo, bool can_redo) noexcept;

    // Draft operations are transient and do not persist anything.  A null
    // draft means that the optional Project profile should be removed.
    void set_draft(std::optional<HybridPixelProfile> profile);
    [[nodiscard]] bool create_default_profile();
    void clear_draft();
    [[nodiscard]] bool set_profile_id(std::string profile_id);
    [[nodiscard]] bool set_enabled(bool enabled);
    [[nodiscard]] bool set_virtual_width(std::uint32_t width);
    [[nodiscard]] bool set_virtual_height(std::uint32_t height);
    [[nodiscard]] bool set_pixels_per_unit(float pixels_per_unit);
    [[nodiscard]] bool set_integer_scaling(bool integer_scaling);
    [[nodiscard]] bool set_snap_camera(bool snap_camera);
    [[nodiscard]] bool set_snap_sprites(bool snap_sprites);
    [[nodiscard]] bool set_presentation_filter(std::string presentation_filter);

    // The no-argument form uses the current snapshot revision and draft.
    // An absent draft is an explicit disable/remove request.
    [[nodiscard]] bool request(bool dry_run = false);
    [[nodiscard]] bool request(std::uint64_t expected_revision, bool dry_run);
    [[nodiscard]] bool request(
        std::uint64_t expected_revision, bool dry_run,
        std::optional<HybridPixelProfile> profile);

    [[nodiscard]] bool request(HybridPixelProfilePanelRequestKind kind,
                               std::uint64_t expected_revision,
                               bool dry_run = false,
                               std::optional<HybridPixelProfile> profile = std::nullopt);
    [[nodiscard]] bool request_apply(bool dry_run = false);
    [[nodiscard]] bool request_disable_remove(bool dry_run = false);
    [[nodiscard]] bool request_undo(bool dry_run = false);
    [[nodiscard]] bool request_redo(bool dry_run = false);
    [[nodiscard]] std::optional<HybridPixelProfilePanelRequest> consume_request();

    // Stable, bounded semantic state for Agent/retained-UI consumers.  It is
    // a projection only; it never serializes or writes Project manifest data.
    [[nodiscard]] std::string semantic_state_json() const;
    [[nodiscard]] std::string state_json() const { return semantic_state_json(); }

    // This is the only method in this module that touches ImGui.  Headless
    // callers can omit it entirely.
    void render();

private:
    void rebuild_projection();
    [[nodiscard]] bool ensure_draft_profile();
    [[nodiscard]] bool queue_request(HybridPixelProfilePanelRequest request);
    [[nodiscard]] std::string make_request_id(
        HybridPixelProfilePanelRequestKind kind,
        std::uint64_t expected_revision,
        bool dry_run,
        const std::optional<HybridPixelProfile>& profile) const;
    void set_error(std::string message);

    HybridPixelProfileSnapshot snapshot_;
    HybridPixelProfileDraft draft_;
    HybridPixelProfilePreviewSurface preview_surface_;
    HybridPixelProfilePanelValidation validation_;
    HybridPixelProfilePanelPreview preview_;
    bool can_undo_{};
    bool can_redo_{};
    std::optional<HybridPixelProfilePanelRequest> pending_request_;
    std::string last_error_;
};

} // namespace noemancer
