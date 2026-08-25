#pragma once

#include "editor/animation_graph_canvas.hpp"
#include "editor/editor_model.hpp"
#include "editor/hybrid_pixel_profile_panel.hpp"
#include "editor/project_settings_input_map_panel.hpp"
#include "editor/project_ui_authoring_panel.hpp"
#include "editor/sky_atmosphere_authoring/panel.hpp"
#include "editor/startup_hub.hpp"
#include "engine/render_world.hpp"
#include "engine/hybrid_pixel_profile.hpp"
#include "engine/sky_environment.hpp"

#include <cstdint>
#include <array>
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noemancer {

struct ScenePickRequest final {
    std::uint32_t x{};
    std::uint32_t y{};
};

struct ScenePointerPosition final {
    std::int32_t x{};
    std::int32_t y{};
};

struct SceneWindowPosition final {
    std::int32_t x{};
    std::int32_t y{};
    float scale_y{1.0F};
};

struct EditorProjectContext final {
    std::string project_id{"project.engine-bootstrap"};
    std::string name{"Engine Bootstrap"};
    std::string root{"engine://"};
    std::string startup_scene;
    std::string script_project;
    std::vector<std::string> asset_roots;
    std::vector<InputActionDefinition> input_actions;
    std::uint64_t input_revision{1};
    std::optional<HybridPixelProfile> hybrid_pixel_profile;
    std::uint64_t hybrid_pixel_profile_revision{1};
    bool hybrid_pixel_profile_can_undo{};
    bool hybrid_pixel_profile_can_redo{};
    std::optional<SkyAtmosphereSettings> sky_atmosphere;
    std::uint64_t sky_atmosphere_revision{1};
    bool sky_atmosphere_can_undo{};
    bool sky_atmosphere_can_redo{};
    std::optional<SkyEnvironmentSettings> sky_environment;
    std::uint64_t sky_environment_revision{1};
    std::string project_ui_document_json;
    std::uint64_t project_ui_revision{1};
    std::string project_ui_fingerprint;
    bool project_ui_can_undo{};
    bool project_ui_can_redo{};
};

struct EditorAssetThumbnailArtifact final {
    std::string asset_id;
    std::string uri;
};

enum class EditorSimulationState { edit, playing, paused };
enum class EditorSimulationCommand { play, pause, resume, step, stop, apply_back_and_stop };
enum class EditorManagedDebugCommand { start_attach, set_breakpoint, continue_execution, pause, step_over,
    step_in, step_out, refresh_stack, stop };

struct EditorManagedDebugRequest final {
    EditorManagedDebugCommand command{EditorManagedDebugCommand::start_attach};
    std::string package_output_path;
    std::string source_path;
    std::uint32_t line{1};
    std::uint64_t thread_id{1};
};

struct EditorPackageRequest final {
    std::string output_path;
    std::string target_profile{"windows-x64-release"};
    bool dry_run{};
};

enum class EditorProjectCommand { create,open };
struct EditorProjectRequest final {
    EditorProjectCommand command{EditorProjectCommand::open};
    std::string path;
    std::string name;
    std::string preset{"starter"};
};

struct EditorSourceOpenRequest final {
    std::string path;
    std::uint32_t line{1};
    std::uint32_t column{1};
};

struct EditorScriptBuildCompletion final {
    std::string configuration;
    std::string trigger;
    std::string result_json;
};

// Plain-data bridge for the live Editor context.  The EditorModel remains the
// sole selection/focus authority; this value is a bounded projection for the
// live Agent adapter and never contains ImGui/RmlUi handles.
struct EditorUiContextSnapshot final {
    std::string schema_version{"noemancer.editor-context/0.1"};
    std::uint64_t revision{1};
    std::uint64_t world_revision{};
    std::string authority;
    std::string simulation_state;
    bool writable{};
    std::string project_id;
    std::string project_name;
    std::string project_root;
    std::string scene_source;
    bool scene_dirty{};
    std::vector<std::string> selected_entity_ids;
    std::string primary_selected_entity_id;
    std::string selected_asset_id;
    std::string focused_panel_id;
    std::optional<std::string> active_tab_id;
    std::string last_action_status;
    std::vector<std::string> selected_play_world_change_ids;
    bool scene_can_undo{};
    bool scene_can_redo{};
    bool script_build_busy{};
};

struct EditorUiContextIntent final {
    std::uint64_t expected_revision{};
    bool dry_run{};
    std::optional<std::vector<std::string>> selected_entity_ids;
    std::optional<std::string> primary_selected_entity_id;
    std::optional<std::string> selected_asset_id;
    std::optional<std::string> focused_panel_id;
    std::optional<std::string> active_tab_id;
};

struct EditorUiContextApplyReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::uint64_t revision_before{};
    std::uint64_t revision_after{};
};

struct EditorUiFrameTimings final {
    // Stable order consumed by performance evidence: refresh, chrome, scene,
    // animation, outliner, inspector, assets, console, agent context.
    std::array<double,9> milliseconds{};
};

class EditorUi final {
public:
    EditorUi(World& world, AssetRegistry& assets);
    void render();
    [[nodiscard]] const EditorUiFrameTimings& frame_timings() const noexcept { return frame_timings_; }
    void refresh_world_model();
    void set_engine_status(std::string status_json);
    void set_render_surface(std::uintptr_t texture_id, std::uint32_t width, std::uint32_t height);
    void set_retained_inspector_surface(std::uintptr_t texture_id,std::uint32_t width,std::uint32_t height);
    void set_retained_outliner_surface(std::uintptr_t texture_id,std::uint32_t width,std::uint32_t height);
    void set_retained_asset_browser_surface(std::uintptr_t texture_id,std::uint32_t width,std::uint32_t height);
    void set_render_status(std::string status_json);
    void set_input_status(std::string status_json);
    void set_project_input_capture(ProjectSettingsInputMapCaptureObservation observation);
    void set_project_input_actions(std::vector<InputActionDefinition> actions,std::uint64_t revision);
    void set_project_hybrid_pixel_profile(std::optional<HybridPixelProfile> profile,
                                          std::uint64_t revision,bool can_undo,bool can_redo);
    void set_project_sky_atmosphere(std::optional<SkyAtmosphereSettings> settings,
                                    std::uint64_t revision,bool can_undo,bool can_redo);
    void set_project_sky_environment(std::optional<SkyEnvironmentSettings> settings,
                                     std::uint64_t revision);
    void set_project_ui_document(std::string document_json,std::uint64_t revision,
                                 std::string fingerprint,bool can_undo,bool can_redo);
    void set_project_settings_open(bool open) noexcept { project_settings_open_ = open; }
    // Refreshes the visible Editor projection after an Agent/domain apply.
    // It deliberately refreshes the existing EditorModel instead of copying
    // selection or inspector state into a second context database.
    void refresh_visible_state();
    [[nodiscard]] std::optional<ProjectSettingsInputMapPanelRequest> consume_project_input_request();
    [[nodiscard]] std::optional<HybridPixelProfilePanelRequest> consume_hybrid_pixel_profile_request();
    [[nodiscard]] std::optional<SkyAtmosphereAuthoringRequest> consume_sky_atmosphere_request();
    [[nodiscard]] std::optional<ProjectUiAuthoringPanelRequest> consume_project_ui_request();
    void set_asset_thumbnail_texture(std::string asset_id,std::uintptr_t texture_id);
    [[nodiscard]] std::vector<EditorAssetThumbnailArtifact> asset_thumbnail_artifacts() const;
    void set_recent_projects(std::vector<StartupHubRecentProject> projects,
                             std::string_view persistence_observation_json = {});
    [[nodiscard]] bool set_ui_locale(std::string_view locale);
    [[nodiscard]] std::string_view ui_locale() const noexcept { return ui_locale_; }
    void set_project_context(EditorProjectContext context);
    [[nodiscard]] std::string compile_scripts(std::string_view configuration = "Debug");
    [[nodiscard]] bool begin_compile_scripts(std::string_view configuration = "Debug");
    [[nodiscard]] bool script_compile_busy() const noexcept;
    void set_auto_compile_scripts(bool enabled) noexcept;
    [[nodiscard]] bool auto_compile_scripts() const noexcept;
    [[nodiscard]] bool open_script_source(std::string_view source_path, std::uint32_t line, std::uint32_t column);
    [[nodiscard]] std::optional<EditorSourceOpenRequest> consume_source_open_request();
    [[nodiscard]] std::optional<EditorScriptBuildCompletion> consume_script_build_completion();
    void set_play_script_reload_status(std::string status_json);
    [[nodiscard]] std::uint32_t requested_scene_width() const;
    [[nodiscard]] std::uint32_t requested_scene_height() const;
    [[nodiscard]] std::uint32_t requested_inspector_width() const noexcept;
    [[nodiscard]] std::uint32_t requested_inspector_height() const noexcept;
    [[nodiscard]] std::string retained_inspector_document_json() const;
    [[nodiscard]] std::uint32_t requested_outliner_width() const noexcept;
    [[nodiscard]] std::uint32_t requested_outliner_height() const noexcept;
    [[nodiscard]] std::string retained_outliner_document_json() const;
    [[nodiscard]] std::uint32_t requested_asset_browser_width() const noexcept;
    [[nodiscard]] std::uint32_t requested_asset_browser_height() const noexcept;
    [[nodiscard]] std::string retained_asset_browser_document_json() const;
    void set_asset_browser_query(std::string_view query);
    void set_asset_browser_cursor(std::size_t cursor) noexcept;
    void set_asset_browser_page_size(std::size_t page_size) noexcept;
    [[nodiscard]] bool asset_browser_next_page();
    [[nodiscard]] bool asset_browser_previous_page();
    [[nodiscard]] std::size_t asset_browser_cursor() const noexcept;
    [[nodiscard]] std::size_t asset_browser_page_size() const noexcept;
    [[nodiscard]] float requested_exposure() const;
    void set_exposure(float exposure);
    [[nodiscard]] std::optional<RenderCameraSnapshot> render_camera_override() const;
    [[nodiscard]] std::string semantic_snapshot_json() const;
    [[nodiscard]] std::optional<ScenePickRequest> consume_scene_pick_request();
    [[nodiscard]] std::optional<ScenePointerPosition> scene_pointer_at(float window_x, float window_y) const;
    [[nodiscard]] std::optional<ScenePointerPosition> retained_inspector_pointer_at(float window_x,float window_y) const;
    [[nodiscard]] std::optional<ScenePointerPosition> retained_outliner_pointer_at(float window_x,float window_y) const;
    [[nodiscard]] std::optional<ScenePointerPosition> retained_asset_browser_pointer_at(float window_x,float window_y) const;
    [[nodiscard]] std::optional<SceneWindowPosition> scene_window_at(std::int32_t scene_x, std::int32_t scene_y) const;
    [[nodiscard]] std::optional<SceneWindowPosition> retained_inspector_window_at(std::int32_t surface_x,std::int32_t surface_y) const;
    [[nodiscard]] std::optional<SceneWindowPosition> retained_outliner_window_at(std::int32_t surface_x,std::int32_t surface_y) const;
    [[nodiscard]] std::optional<SceneWindowPosition> retained_asset_browser_window_at(std::int32_t surface_x,std::int32_t surface_y) const;
    [[nodiscard]] bool select_entity(std::string_view entity_id);
    [[nodiscard]] bool select_asset(std::string_view asset_id) noexcept;
    // Revision-bound bridge used by retained surfaces. The binding and value
    // stay plain JSON; all mutations still execute through EditorModel.
    [[nodiscard]] std::string invoke_retained_authoring_action(
        std::string_view action_id,
        std::string_view binding_json,
        std::string_view value_json = "{}");
    [[nodiscard]] EditorUiContextSnapshot editor_context_snapshot() const;
    [[nodiscard]] std::string editor_context_snapshot_json() const;
    [[nodiscard]] EditorUiContextApplyReceipt apply_editor_context_intent(
        const EditorUiContextIntent& intent);
    [[nodiscard]] std::string apply_editor_context_intent_json(std::string_view request_json);
    [[nodiscard]] std::uint64_t editor_context_revision() const noexcept { return editor_context_revision_; }
    void set_simulation_state(EditorSimulationState state) noexcept;
    void set_play_world_context(std::string observation_json,std::string inspector_json,std::string apply_plan_json);
    [[nodiscard]] const std::string& play_world_selected_entity_id() const noexcept;
    [[nodiscard]] std::vector<std::string> selected_play_world_change_ids() const;
    void set_last_action_status(std::string status);
    [[nodiscard]] EditorSimulationState simulation_state() const noexcept;
    [[nodiscard]] std::optional<EditorSimulationCommand> consume_simulation_command();
    void set_managed_debug_context(std::string events_json,std::string last_action_json);
    [[nodiscard]] std::optional<EditorManagedDebugRequest> consume_managed_debug_request();
    void set_package_status(bool busy, std::string status_json);
    [[nodiscard]] std::optional<EditorPackageRequest> consume_package_request();
    void set_project_status(std::string status_json);
    [[nodiscard]] std::optional<EditorProjectRequest> consume_project_request();
    [[nodiscard]] bool consume_exit_request() noexcept;
    void request_close();

private:
    enum class GizmoMode { select, translate, rotate, scale, tilemap };
    enum class TileBrushShape { brush,rectangle,flood };
    void draw_root_dockspace();
    void draw_startup_hub();
    void draw_scene_view();
    void draw_world_outliner();
    void draw_inspector();
    void draw_asset_browser();
    void synchronize_asset_browser_navigation() const;
    void draw_animation_graph();
    void draw_console();
    void draw_agent_context();
    void draw_transform_gizmo(float x,float y,float width,float height);
    void reset_viewport_camera();
    void update_viewport_camera_navigation(bool hovered);
    void handle_tilemap_brush(float x,float y,float width,float height,bool hovered);
    void poll_script_compile_job();
    void evaluate_auto_compile();
    void set_focused_panel(std::string_view panel_id);
    void prepare_panel_window(std::string_view panel_id);
    void mark_editor_context_changed() noexcept;
    void synchronize_editor_context_revision();
    [[nodiscard]] std::string editor_context_signature() const;
    [[nodiscard]] bool apply_entity_selection(const std::vector<std::string>& entity_ids,
                                              std::string_view primary_entity_id);
    [[nodiscard]] bool entity_exists_in_play_world(std::string_view entity_id) const;
    [[nodiscard]] bool chinese_ui() const noexcept;
    [[nodiscard]] const char* localized(const char* english,const char* simplified_chinese) const noexcept;

    EditorModel model_;
    EditorUiFrameTimings frame_timings_{};
    StartupHubModel startup_hub_;
    bool startup_hub_open_{true};
    std::string startup_hub_persistence_json_{"null"};
    std::string ui_locale_{"en-US"};
    std::string engine_status_json_;
    std::string render_status_json_;
    std::string input_status_json_{R"({"schemaVersion":"noemancer.input-sources/0.1","devices":[],"sources":[]})"};
    std::optional<ProjectSettingsInputMapPanel> project_input_panel_;
    std::optional<HybridPixelProfilePanel> hybrid_pixel_profile_panel_;
    std::optional<SkyAtmosphereAuthoringPanel> sky_atmosphere_panel_;
    std::optional<ProjectUiAuthoringPanel> project_ui_panel_;
    bool project_settings_open_{};
    std::string last_script_compile_json_;
    std::string script_source_location_json_;
    std::string scripting_status_cache_;
    std::string script_compile_job_json_;
    std::future<std::string> script_compile_future_;
    std::chrono::steady_clock::time_point script_compile_started_{};
    std::uint64_t script_compile_job_sequence_{};
    bool script_compile_busy_{};
    bool auto_compile_scripts_{true};
    std::string auto_compile_candidate_fingerprint_;
    std::string auto_compile_blocked_fingerprint_;
    std::chrono::steady_clock::time_point auto_compile_candidate_since_{};
    std::chrono::steady_clock::time_point last_model_refresh_{};
    std::uint64_t editor_context_revision_{1};
    std::string editor_context_signature_;
    std::string pending_panel_focus_id_;
    std::optional<std::string> active_tab_id_;
    EditorProjectContext project_context_;
    std::string last_action_status_{"No transaction has been committed in this editor session."};
    std::uintptr_t scene_texture_id_{0};
    std::unordered_map<std::string,std::uintptr_t> asset_thumbnail_textures_;
    std::uint32_t scene_texture_width_{0};
    std::uint32_t scene_texture_height_{0};
    std::uint32_t requested_scene_width_{960};
    std::uint32_t requested_scene_height_{540};
    std::uintptr_t retained_inspector_texture_id_{};
    std::uint32_t retained_inspector_texture_width_{384};
    std::uint32_t retained_inspector_texture_height_{640};
    std::uint32_t requested_inspector_width_{384};
    std::uint32_t requested_inspector_height_{640};
    float retained_inspector_canvas_x_{};
    float retained_inspector_canvas_y_{};
    float retained_inspector_canvas_width_{};
    float retained_inspector_canvas_height_{};
    std::uintptr_t retained_outliner_texture_id_{};
    std::uint32_t retained_outliner_texture_width_{320};
    std::uint32_t retained_outliner_texture_height_{640};
    std::uint32_t requested_outliner_width_{320};
    std::uint32_t requested_outliner_height_{640};
    float retained_outliner_canvas_x_{};
    float retained_outliner_canvas_y_{};
    float retained_outliner_canvas_width_{};
    float retained_outliner_canvas_height_{};
    std::uintptr_t retained_asset_browser_texture_id_{};
    std::uint32_t retained_asset_browser_texture_width_{960};
    std::uint32_t retained_asset_browser_texture_height_{320};
    std::uint32_t requested_asset_browser_width_{960};
    std::uint32_t requested_asset_browser_height_{320};
    float retained_asset_browser_canvas_x_{};
    float retained_asset_browser_canvas_y_{};
    float retained_asset_browser_canvas_width_{};
    float retained_asset_browser_canvas_height_{};
    bool layout_initialized_{false};
    float requested_exposure_{1.0F};
    std::optional<RenderCameraSnapshot> editor_camera_;
    std::optional<ScenePickRequest> scene_pick_request_;
    std::optional<EditorSimulationCommand> simulation_command_;
    std::optional<EditorManagedDebugRequest> managed_debug_request_;
    std::optional<EditorPackageRequest> package_request_;
    std::string package_status_json_;
    std::array<char,512> package_output_path_{};
    int package_profile_index_{};
    bool package_panel_open_{};
    bool package_busy_{};
    std::optional<EditorProjectRequest> project_request_;
    std::optional<EditorSourceOpenRequest> source_open_request_;
    std::optional<EditorScriptBuildCompletion> script_build_completion_;
    std::string play_script_reload_json_;
    std::string project_status_json_;
    std::string scene_recovery_candidates_json_;
    std::array<char,128> new_scene_name_{};
    std::array<char,512> project_path_{};
    std::array<char,128> project_name_{};
    int project_preset_index_{};
    int project_dialog_mode_{};
    bool exit_requested_{};
    bool close_dialog_open_{};
    std::string managed_debug_events_json_{R"({"schemaVersion":"noemancer.managed-debug-session-events/0.1","success":false,"events":[]})"};
    std::string managed_debug_last_action_json_;
    std::array<char,512> managed_debug_source_path_{};
    int managed_debug_breakpoint_line_{1};
    std::uint64_t managed_debug_thread_id_{1};
    EditorSimulationState simulation_state_{EditorSimulationState::edit};
    std::string play_world_observation_json_;
    std::string play_world_inspector_json_;
    std::string play_world_apply_plan_json_;
    std::string play_world_selected_entity_id_;
    std::unordered_set<std::string> selected_play_world_change_ids_;
    std::unordered_set<std::string> known_play_world_change_ids_;
    std::array<char,96> outliner_filter_{};
    std::array<char,96> inspector_filter_{};
    std::array<char,96> asset_browser_filter_{};
    mutable std::string asset_browser_query_;
    mutable std::size_t asset_browser_cursor_{};
    mutable std::size_t asset_browser_page_size_{64U};
    mutable std::uint64_t asset_browser_registry_revision_{};
    std::optional<AnimationGraphDocument> animation_graph_document_;
    AnimationGraphCanvasModel animation_graph_canvas_;
    std::string animation_graph_asset_id_;
    std::string animation_graph_fingerprint_;
    std::string animation_graph_drag_node_id_;
    std::string animation_graph_drag_asset_id_;
    std::string animation_graph_drag_fingerprint_;
    std::string animation_graph_inline_diagnostic_;
    int animation_graph_focus_frames_{};
    int animation_graph_create_kind_{};
    float animation_graph_drag_x_{};
    float animation_graph_drag_y_{};
    float animation_graph_drag_offset_x_{};
    float animation_graph_drag_offset_y_{};
    float animation_graph_connection_threshold_{0.5F};
    std::array<char,96> animation_graph_new_node_id_{};
    std::array<char,256> animation_graph_new_node_asset_{};
    std::array<char,96> animation_graph_new_node_parameter_{};
    std::unordered_map<std::string,float> animation_graph_layer_drafts_;
    std::unordered_map<std::string,float> animation_graph_mask_drafts_;
    GizmoMode gizmo_mode_{GizmoMode::translate};
    std::string gizmo_preview_entity_;
    std::array<float,16> gizmo_preview_matrix_{};
    bool gizmo_was_using_{};
    float scene_canvas_x_{};
    float scene_canvas_y_{};
    float scene_canvas_width_{};
    float scene_canvas_height_{};
    std::string tile_brush_layer_id_;
    std::string tile_brush_tile_id_;
    bool tile_brush_erase_{};
    bool tile_brush_flip_x_{};
    bool tile_brush_flip_y_{};
    TileBrushShape tile_brush_shape_{TileBrushShape::brush};
    bool tile_stroke_active_{};
    std::string tile_stroke_base_fingerprint_;
    std::vector<TilemapCellEdit> tile_stroke_edits_;
    std::optional<std::array<std::int32_t,2>> tile_brush_hover_cell_;
    std::optional<std::array<std::int32_t,2>> tile_region_anchor_;
    std::optional<std::array<std::int32_t,2>> tile_region_end_;
    std::string palette_rule_tile_id_;
    std::string palette_rule_fingerprint_;
    std::array<char,64> palette_rule_group_{};
    std::array<bool,16> palette_rule_enabled_{};
    std::array<std::array<char,128>,16> palette_rule_frames_{};
};

} // namespace noemancer
