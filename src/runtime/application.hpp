#pragma once

#include "editor/editor_ui.hpp"
#include "engine/asset_registry.hpp"
#include "engine/command_registry.hpp"
#include "engine/engine_host.hpp"
#include "engine/hybrid_pixel_profile.hpp"
#include "engine/live_editor_session.hpp"
#include "engine/log.hpp"
#include "engine/sky_atmosphere.hpp"
#include "engine/world.hpp"
#include "engine/virtual_file_system.hpp"
#include "runtime/asset_vfs_catalog.hpp"
#include "runtime/performance_evidence.hpp"
#include "runtime/live_editor_transport.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

class DebugPlayerProcess;
class GamePersistenceStore;
class ProjectInputEditSession;
class ProjectHybridPixelAuthoring;
class ProjectUiAuthoringSession;
class RecentWorkspaceStore;

struct RuntimeInputSample final {
    std::string source;
    float value{};
};

struct RuntimeInputEvent final {
    std::uint32_t frame{};
    std::string source;
    float value{};
};

struct RunOptions {
    bool headless{false};
    std::uint32_t frames{0};
    LogFormat log_format{LogFormat::Human};
    std::string capture_frame_path;
    std::string capture_editor_frame_path;
    std::optional<ScenePickRequest> probe_pixel;
    std::optional<float> exposure;
    std::optional<float> render_scale;
    std::string shadow_quality{"high"};
    std::uint32_t texture_streaming_budget_kib{512};
    std::uint32_t texture_streaming_resident_budget_kib{262144};
    std::string texture_streaming_workload;
    std::string temporal_debug_mode{"final"};
    std::string ssr_quality{"high"};
    std::string ssr_debug_mode{"final"};
    std::string ssgi_quality{"high"};
    std::string ssgi_debug_mode{"final"};
    std::string reference_scene_id;
    std::uint32_t render_stress_instances{0};
    bool animation_physics_stress{};
    bool physics_showcase{};
    std::uint32_t vfx_respawn_interval{0};
    std::string gpu_backend{"auto"};
    bool gpu_debug{};
    bool disable_gpu_driven{};
    bool enable_gpu_occlusion{};
    bool disable_ambient_occlusion{};
    bool disable_auto_exposure{};
    bool disable_ssr{};
    bool disable_ssgi{};
    // Development-facing production binding proof. It is opt-in until the
    // native output texture can be shared with the SDL_GPU raster frame.
    bool enable_native_rt_session{};
    bool gpu_visibility_readback{};
    // Explicit opt-in fixture for exercising real GPU HiZ occlusion.  The
    // regular bootstrap/editor scene must remain unchanged when this is off.
    bool gpu_occlusion_stress{};
    std::uint32_t gpu_occlusion_stress_instances{256};
    bool shadow_scalability_stress{};
    std::uint32_t shadow_scalability_stress_instances{128};
    std::uint32_t render_stress_offscreen_percent{};
    std::string ui_locale{"en-US"};
    float ui_scale{1.0F};
    std::string project_path;
    std::vector<RuntimeInputSample> input_samples;
    std::vector<RuntimeInputEvent> input_events;
    std::string editor_selected_asset_id;
    bool editor_project_settings{};
    std::string runtime_executable;
    bool player_mode{};
    std::string player_profile_path;
    std::string player_display_name;
    std::string debug_ready_event;
    std::string debug_wait_event;
    std::string performance_evidence_path;
    bool performance_hidden{};
    std::string performance_workload_id{"lumen-run.vertical-slice/0.1"};
    std::uint32_t performance_warmup_frames{120};
    std::uint32_t performance_sample_frames{600};
    std::uint32_t window_width{1440};
    std::uint32_t window_height{900};
    // Optional process-entry clock supplied by main().  Tests and embedded
    // hosts may omit it; Application owns a bounded fallback clock then.
    StartupTelemetry* startup_telemetry{};
};

class Application final {
public:
    explicit Application(RunOptions options);
    ~Application();
    [[nodiscard]] int run();

private:
    [[nodiscard]] int run_headless();
    [[nodiscard]] int run_interactive();
    void tick_frame(double delta_seconds);
    void apply_simulation_command(EditorSimulationCommand command);
    void apply_managed_debug_request(const EditorManagedDebugRequest& request);
    void apply_package_request(const EditorPackageRequest& request);
    void apply_project_request(const EditorProjectRequest& request);
    void apply_project_input_map_request(const ProjectSettingsInputMapPanelRequest& request);
    void apply_hybrid_pixel_profile_request(const HybridPixelProfilePanelRequest& request);
    void apply_sky_atmosphere_request(const SkyAtmosphereAuthoringRequest& request);
    void apply_project_ui_request(const ProjectUiAuthoringPanelRequest& request);
    void apply_source_open_request(const EditorSourceOpenRequest& request);
    void apply_script_build_completion(const EditorScriptBuildCompletion& completion);
    [[nodiscard]] bool start_live_editor_session();
    void stop_live_editor_session() noexcept;
    void refresh_live_editor_session();
    void rebuild_live_editor_command_registry();
    [[nodiscard]] LiveEditorTransportDispatchResult dispatch_live_editor_request(
        const LiveEditorTransportRequest& request);
    [[nodiscard]] std::string load_editor_project_json(const std::filesystem::path& project_path);
    void publish_recent_workspaces();
    void record_recent_workspace(std::string_view path, std::string_view display_name);
    void poll_package_job();
    void register_sprite_assets(World& world);
    [[nodiscard]] bool register_cooked_geometry_assets();
    [[nodiscard]] bool register_animation_clip_assets(World& world);
    void register_animation_state_machine_assets(World& world);
    void register_animation_graph_assets(World& world);
    void register_tilemap_assets(World& world);
    void register_audio_assets(World& world);
    [[nodiscard]] bool rebuild_asset_vfs_catalog();
    void configure_persistence_store(std::string project_id);
    void process_persistence_requests(World& world);
    void log_startup_telemetry(std::string_view mode, std::string_view outcome);
    [[nodiscard]] World& active_world() noexcept;

    StartupTelemetry startup_telemetry_storage_;
    StartupTelemetry* startup_telemetry_{};
    RunOptions options_;
    Logger logger_;
    EngineHost engine_host_;
    World world_;
    std::unique_ptr<World> play_world_;
    bool play_paused_{};
    bool play_single_step_{};
    std::uint64_t play_base_revision_{};
    std::string play_base_scene_json_;
    AssetRegistry asset_registry_;
    std::shared_ptr<VirtualFileSystem> virtual_file_system_;
    AssetVfsCatalog asset_vfs_catalog_;
    EditorUi editor_ui_;
    std::unique_ptr<RecentWorkspaceStore> recent_workspace_store_;
    TilemapRenderBakeCache tilemap_render_bake_cache_;
    std::string startup_error_json_;
    std::filesystem::path script_project_root_;
    std::filesystem::path script_project_path_;
    std::filesystem::path project_root_;
    std::string project_id_;
    std::string project_name_;
    std::vector<InputActionDefinition> project_input_actions_;
    std::unique_ptr<ProjectInputEditSession> project_input_session_;
    std::unique_ptr<ProjectHybridPixelAuthoring> project_hybrid_pixel_session_;
    std::unique_ptr<SkyAtmosphereAuthoringSession> project_sky_atmosphere_session_;
    std::unique_ptr<ProjectUiAuthoringSession> project_ui_session_;
    std::unique_ptr<CommandRegistry> live_editor_command_registry_;
    LiveEditorSessionStore live_editor_session_store_;
    LiveEditorTransportServer live_editor_transport_server_;
    LiveEditorSessionDescriptor live_editor_session_descriptor_;
    std::string live_editor_session_id_;
    std::string live_editor_process_identity_;
    std::filesystem::path live_editor_credential_path_;
    std::uint64_t live_editor_session_revision_{};
    std::uint64_t live_editor_generation_{};
    std::chrono::steady_clock::time_point live_editor_next_heartbeat_{};
    bool live_editor_session_active_{};
    std::string project_hud_document_json_;
    // Canonical, structured description of the generated occlusion fixture.
    // Empty for ordinary projects/scenes so production state cannot confuse
    // an editor scene with an acceptance workload.
    std::string gpu_occlusion_stress_contract_json_;
    std::string shadow_scalability_stress_contract_json_;
    std::string player_profile_document_json_;
    std::optional<HybridPixelProfile> hybrid_pixel_profile_;
    SkyAtmosphereSettings sky_atmosphere_base_{make_sky_atmosphere_settings(SkyAtmosphereQuality::high)};
    SkyAtmosphereSettings sky_atmosphere_{make_sky_atmosphere_settings(SkyAtmosphereQuality::high)};
    std::optional<SkyEnvironmentSettings> sky_environment_;
    double sky_environment_advance_accumulator_seconds_{};
    std::uint64_t sky_environment_revision_{1U};
    std::uint64_t sky_atmosphere_revision_{1U};
    std::uint64_t hybrid_pixel_profile_revision_{1U};
    std::unique_ptr<GamePersistenceStore> persistence_store_;
    std::string managed_debug_last_action_json_;
    bool managed_debug_external_player_{};
    std::unique_ptr<DebugPlayerProcess> managed_debug_player_;
    std::future<std::string> package_future_;
    bool package_busy_{};
    std::uint32_t cooked_animation_load_count_{};
    std::uint32_t source_animation_decode_count_{};
    std::uint32_t offline_animation_compile_count_{};
    std::uint32_t cooked_geometry_load_count_{};
    std::uint32_t source_geometry_decode_count_{};
    std::uint32_t offline_geometry_compile_count_{};
};

} // namespace noemancer
