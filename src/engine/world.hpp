#pragma once

#include <flecs.h>

#include "engine/scene_document.hpp"
#include "engine/animation_graph.hpp"
#include "engine/animation_state_machine.hpp"
#include "engine/character_motor_2d.hpp"
#include "engine/semantic_state.hpp"
#include "engine/semantic_ui.hpp"
#include "engine/simulation_runtime.hpp"
#include "engine/gameplay_runtime.hpp"
#include "engine/gameplay_ability.hpp"
#include "engine/scripting_runtime.hpp"
#include "engine/sprite_asset.hpp"
#include "engine/tilemap_asset.hpp"
#include "engine/vfx_runtime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <span>

namespace noemancer {

struct Transform {
    float x{};
    float y{};
    float z{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float scale_z{1.0F};
    float rotation_x{};
    float rotation_y{};
    float rotation_z{};
    float rotation_w{1.0F};
};

struct Velocity {
    float x{};
    float y{};
    float z{};
};

struct RigidBody {
    PhysicsMotionType motion_type{PhysicsMotionType::dynamic_body};
    float mass{1.0F};
    float gravity_factor{1.0F};
    float linear_damping{0.05F};
};

struct BoxCollider {
    float half_x{0.5F};
    float half_y{0.5F};
    float half_z{0.5F};
    float friction{0.5F};
    float restitution{};
    bool is_trigger{};
};

struct SphereCollider {
    float radius{0.5F};
    float friction{0.5F};
    float restitution{};
    bool is_trigger{};
};

struct CapsuleCollider {
    float radius{0.5F};
    float half_height{0.5F};
    float friction{0.5F};
    float restitution{};
    bool is_trigger{};
};

struct CharacterMotor2D final {
    CharacterMotor2DConfig config;
    CharacterMotor2DState state;
};

struct Platform2D final {
    std::string collision_mode{"solid"};
    float axis_x{1.0F};
    float axis_y{};
    float axis_z{};
    float motion_distance{};
    float motion_period_seconds{2.0F};
    float motion_phase{};
    float origin_x{};
    float origin_y{};
    float origin_z{};
    float elapsed_seconds{};
};

struct ConvexHullCollider {
    std::vector<std::array<float, 3>> points;
    float friction{0.5F};
    float restitution{};
    bool is_trigger{};
};

struct AnimationPlayer {
    std::string clip_asset;
    float playback_speed{1.0F};
    float time_seconds{};
    float base_y{};
    bool looping{true};
    bool playing{true};
    std::string next_clip_asset;
    float next_time_seconds{};
    bool next_looping{true};
    float transition_duration_seconds{};
    float transition_elapsed_seconds{};
    std::string root_motion_mode{"ignore"};
    std::string state_machine_asset{"animation.machine.basic-locomotion"};
    std::string active_state{"idle"};
    std::string previous_state;
    float state_elapsed_seconds{};
    std::uint64_t state_transition_count{};
    std::unordered_map<std::string,float> state_parameters{{"speed",0.0F},{"grounded",1.0F}};
    std::string animation_graph_asset;
    std::unordered_map<std::string,float> graph_parameters;
    std::unordered_map<std::string,float> graph_node_times;
    std::unordered_map<std::string,float> graph_sync_phases;
};

struct AnimationCueState final {
    std::string cue;
    std::string source_event_type;
    std::uint64_t event_sequence{};
    float age_seconds{};
};

struct Camera {
    float target_x{};
    float target_y{};
    float target_z{};
    float vertical_fov_degrees{45.0F};
    float near_clip{0.1F};
    float far_clip{100.0F};
    bool primary{true};
    std::string projection{"perspective"};
    float orthographic_height{10.0F};
};

struct CameraFollow2D final {
    std::string target_entity_id;
    float offset_x{};
    float offset_y{2.0F};
    float offset_z{17.0F};
    float dead_zone_x{1.5F};
    float dead_zone_y{0.8F};
    float look_ahead_distance{1.5F};
    float smoothing{7.0F};
    float minimum_x{-10000.0F};
    float minimum_y{-10000.0F};
    float maximum_x{10000.0F};
    float maximum_y{10000.0F};
    float center_x{};
    float center_y{};
    std::string decision{"hold"};
};

struct DirectionalLight {
    float direction_x{-0.55F};
    float direction_y{-1.0F};
    float direction_z{-0.35F};
    float color_r{1.0F};
    float color_g{0.96F};
    float color_b{0.88F};
    float intensity{0.95F};
    float ambient_intensity{0.18F};
    bool casts_shadows{true};
};

struct LocalLight {
    std::string kind{"point"};
    float color_r{1.0F};
    float color_g{0.95F};
    float color_b{0.85F};
    float luminous_power_lumens{800.0F};
    float range_meters{8.0F};
    float direction_x{};
    float direction_y{-1.0F};
    float direction_z{};
    float inner_cone_degrees{25.0F};
    float outer_cone_degrees{35.0F};
    float source_radius_meters{0.05F};
    bool casts_shadows{};
};

struct MeshRenderer {
    std::string mesh_asset;
    bool visible{true};
    bool casts_shadows{true};
    bool receives_shadows{true};
};

struct SpriteRenderer final {
    SpritePlaybackState playback;
    float playback_speed{1.0F};
    bool flip_x{};
    bool flip_y{};
    std::string sorting_layer{"default"};
    std::int32_t sorting_order{};
    bool visible{true};
};

struct TilemapRenderer final {
    std::string tilemap_asset;
    bool visible{true};
    bool collision_enabled{true};
};

struct PbrMaterial {
    float base_r{0.8F};
    float base_g{0.8F};
    float base_b{0.8F};
    float metallic{};
    float roughness{0.6F};
    std::string base_color_texture;
    float emissive_r{};
    float emissive_g{};
    float emissive_b{};
    float emissive_intensity{};
};

struct SemanticIdentity final {
    std::string id;
    std::string path;
    std::string type;
    std::string schema_ref;
    std::string display_name;
    std::string scene_guid;
    std::string parent_guid;
    SourceAnchor source;
};

struct SceneLoadResult final {
    bool success{};
    std::size_t entity_count{};
    std::uint64_t revision{};
    std::vector<SceneDocumentError> errors;
};

struct ResolvedTilemapCellView final {
    std::string stable_id;
    std::string tilemap_asset;
    std::string layer_id;
    std::string sorting_layer{"default"};
    std::int32_t sorting_order{};
    std::int32_t cell_x{};
    std::int32_t cell_y{};
    std::int32_t chunk_x{};
    std::int32_t chunk_y{};
    std::string chunk_content_fingerprint;
    float cell_width{1.0F};
    float cell_height{1.0F};
    std::string tile_id;
    std::string autotile_group;
    std::uint8_t autotile_mask{};
    bool flip_x{};
    bool flip_y{};
    SpriteResolvedFrame sprite_frame;
};

struct TilemapVisibilityCamera final {
    std::array<float,3> position{};
    std::array<float,3> target{};
    float vertical_fov_degrees{45.0F};
    float near_clip{0.1F};
    float far_clip{100.0F};
    std::string projection{"perspective"};
    float orthographic_height{10.0F};
};

struct TilemapViewQuery final {
    std::uint32_t viewport_width{};
    std::uint32_t viewport_height{};
    std::optional<TilemapVisibilityCamera> camera_override;
};

struct WorldEntityView final {
    std::string id;
    std::string display_name;
    std::string type;
    std::string scene_guid;
    std::string parent_guid;
    SourceAnchor source;
    std::optional<Transform> transform;
    std::optional<Velocity> velocity;
    std::optional<RigidBody> rigid_body;
    std::optional<BoxCollider> box_collider;
    std::optional<SphereCollider> sphere_collider;
    std::optional<CapsuleCollider> capsule_collider;
    std::optional<CharacterMotor2D> character_motor_2d;
    std::optional<Platform2D> platform_2d;
    std::optional<ConvexHullCollider> convex_hull_collider;
    std::optional<AnimationPlayer> animation_player;
    std::optional<AnimationCueState> animation_cue;
    std::optional<SkeletalPose> skeletal_pose;
    std::optional<Camera> camera;
    std::optional<CameraFollow2D> camera_follow_2d;
    std::optional<DirectionalLight> directional_light;
    std::optional<LocalLight> local_light;
    std::optional<MeshRenderer> mesh_renderer;
    std::optional<SpriteRenderer> sprite_renderer;
    std::optional<TilemapRenderer> tilemap_renderer;
    std::optional<ResolvedTilemapAsset> tilemap_asset;
    std::vector<ResolvedTilemapCellView> tilemap_cells;
    std::size_t tilemap_total_cell_count{};
    std::size_t tilemap_compiled_chunk_count{};
    std::size_t tilemap_resolved_chunk_count{};
    std::size_t tilemap_skipped_chunk_count{};
    std::size_t tilemap_cells_skipped_before_resolution{};
    bool tilemap_early_visibility_applied{};
    std::uint64_t tilemap_compilation_revision{};
    bool tilemap_cells_truncated{};
    std::optional<SpriteResolvedFrame> sprite_frame;
    std::optional<PbrMaterial> pbr_material;
    std::uint64_t revision{};
};

struct TransformUpdateResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::uint64_t revision{};
};

struct GameplayPersistenceRequest final {
    std::uint64_t sequence{};
    std::string action;
    std::string slot_id;
    std::string requester_entity_id;
};

class World final {
public:
    World();

    [[nodiscard]] SceneLoadResult load_scene(const SceneDocument& document);
    [[nodiscard]] std::vector<AnimationCompileResult> register_gltf_animations(std::string_view asset_id,
                                                                               const GltfMeshData& source);
    [[nodiscard]] AnimationCookedArtifactLoadResult register_cooked_animation(
        std::span<const std::byte> payload, std::string_view expected_asset_id = {},
        std::string_view expected_source_hash = {}, std::string_view expected_payload_hash = {});
    [[nodiscard]] bool register_animation_state_machine(AnimationStateMachineDocument document);
    [[nodiscard]] bool register_animation_graph(AnimationGraphDocument document);
    [[nodiscard]] bool register_sprite_asset(SpriteAssetDocument document);
    [[nodiscard]] SpritePageBindingUpdateResult register_sprite_page_bindings(
        std::string_view asset_id, const std::vector<SpriteRuntimePageBinding>& bindings,
        std::optional<std::uint64_t> expected_revision = std::nullopt);
    [[nodiscard]] bool register_tile_palette(TilePaletteDocument document);
    [[nodiscard]] bool register_tilemap_asset(TilemapDocument document);
    void tick(float delta_seconds);
    [[nodiscard]] std::size_t entity_count() const;
    [[nodiscard]] std::vector<WorldEntityView> entity_views(std::optional<TilemapViewQuery> tilemap_query = std::nullopt) const;
    [[nodiscard]] std::string observe_json(const ObservationQuery& query) const;
    [[nodiscard]] std::string delta_json(std::uint64_t since_revision) const;
    [[nodiscard]] TransformChangePlan plan_transform_update(
        std::string_view entity_id,
        Transform transform,
        std::uint64_t base_revision,
        std::string_view manager) const;
    [[nodiscard]] PropertyChangePlan plan_property_update(std::string_view entity_id, std::string_view property,
                                                          std::string_view value_json, std::uint64_t base_revision,
                                                          std::string_view manager) const;
    [[nodiscard]] ActionReceipt apply_transform_plan(
        const TransformChangePlan& plan,
        bool dry_run);
    [[nodiscard]] ActionReceipt apply_property_plan(const PropertyChangePlan& plan, bool dry_run);
    [[nodiscard]] TransformUpdateResult update_transform(
        std::string_view entity_id,
        Transform transform,
        std::uint64_t expected_revision);
    [[nodiscard]] ActionReceipt undo(std::uint64_t expected_revision, std::string_view manager);
    [[nodiscard]] ActionReceipt redo(std::uint64_t expected_revision, std::string_view manager);
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::string scene_source_uri() const;
    [[nodiscard]] std::string edit_scene_entity_json(std::string_view operation, std::string_view entity_id,
                                                     std::string_view new_entity_id, std::string_view display_name,
                                                     std::string_view parent_entity_id, std::string_view component, bool recursive,
                                                     std::uint64_t base_revision, std::string_view manager, bool dry_run);
    [[nodiscard]] std::string edit_transform_json(std::string_view entity_id, Transform transform,
                                                  std::uint64_t base_revision, std::string_view manager, bool dry_run);
    [[nodiscard]] std::string replace_scene_document_json(std::string_view document_json,
                                                           std::uint64_t base_revision,
                                                           std::string_view manager, bool dry_run);
    [[nodiscard]] std::string save_scene_to_source_json();
    [[nodiscard]] std::string save_scene_as_source_json(std::string_view source_path, bool overwrite);
    [[nodiscard]] std::string open_scene_from_source_json(std::string_view source_path);
    [[nodiscard]] std::string snapshot_json() const;
    [[nodiscard]] std::string render_observation_json() const;
    [[nodiscard]] std::string physics_observation_json() const;
    [[nodiscard]] std::string physics_ray_cast_json(Transform origin, Transform direction) const;
    [[nodiscard]] std::string physics_sphere_sweep_json(Transform origin,Transform direction,float radius,
                                                        std::string_view ignored_entity_id={}) const;
    [[nodiscard]] std::string animation_observation_json() const;
    [[nodiscard]] std::string sprite_observation_json(std::string_view entity_id={}) const;
    [[nodiscard]] std::string set_sprite_playback_json(std::string_view entity_id,std::string_view clip_id,
                                                       bool playing,float playback_speed,bool flip_x,bool restart);
    [[nodiscard]] std::string animation_skeleton_json(std::string_view entity_id, std::size_t max_joints) const;
    [[nodiscard]] std::string animation_state_machine_json(std::string_view entity_id) const;
    [[nodiscard]] std::string animation_state_parameter_set_json(std::string_view entity_id,std::string_view parameter,float value);
    [[nodiscard]] std::string animation_graph_json(std::string_view entity_id) const;
    [[nodiscard]] std::string animation_graph_parameter_set_json(std::string_view entity_id,std::string_view parameter,float value);
    [[nodiscard]] std::string input_observation_json() const;
    [[nodiscard]] std::string inject_input_json(std::string_view source, float value);
    [[nodiscard]] bool configure_input_actions(std::span<const InputActionDefinition> definitions);
    [[nodiscard]] bool configure_project_hud(std::string_view project_document_json);
    [[nodiscard]] bool has_project_hud() const noexcept { return !project_hud_document_json_.empty(); }
    [[nodiscard]] std::string audio_observation_json() const;
    [[nodiscard]] std::string register_audio_asset_json(std::string_view asset_id,std::string_view content_hash,
                                                        AudioAssetStorage storage);
    [[nodiscard]] std::string set_audio_bus_json(std::string_view bus_id, float gain, bool muted);
    [[nodiscard]] std::string play_audio_json(std::string_view asset_id, std::string_view bus_id, float gain,
                                              float pitch, bool looping);
    [[nodiscard]] std::string set_audio_listener_json(Transform position, Transform forward, Transform up);
    [[nodiscard]] std::string set_audio_voice_spatial_json(std::uint64_t voice_id, bool spatial, Transform position,
                                                           float minimum_distance, float maximum_distance, float rolloff);
    void mix_audio(std::span<float> interleaved_stereo, std::uint32_t sample_rate);
    [[nodiscard]] std::uint64_t audio_revision() const noexcept { return audio_runtime_.revision(); }
    [[nodiscard]] AudioRenderSnapshot audio_render_snapshot() const { return audio_runtime_.render_snapshot(); }
    [[nodiscard]] std::string gameplay_observation_json(std::size_t max_events = 32) const;
    [[nodiscard]] std::string character_motor_2d_observation_json(std::string_view entity_id = {}) const;
    [[nodiscard]] std::string camera_follow_2d_observation_json(std::string_view entity_id = {}) const;
    [[nodiscard]] std::string gameplay_ability_catalog_json() const;
    [[nodiscard]] std::string gameplay_effect_catalog_json() const;
    [[nodiscard]] std::string gameplay_ability_observation_json(std::string_view entity_id = {}) const;
    [[nodiscard]] std::string gameplay_ability_grant_json(std::string_view entity_id,std::string_view ability_id);
    [[nodiscard]] std::string gameplay_ability_activate_json(std::string_view entity_id,std::string_view ability_id,
                                                             std::string_view target_id);
    [[nodiscard]] std::string gameplay_ability_activate_ray_json(std::string_view entity_id,std::string_view ability_id,
                                                                  Transform origin,Transform direction);
    [[nodiscard]] std::string gameplay_ability_activate_sweep_json(std::string_view entity_id,std::string_view ability_id,
                                                                    Transform origin,Transform direction,float radius);
    [[nodiscard]] std::string gameplay_effect_apply_json(std::string_view source_entity_id,std::string_view target_entity_id,
                                                         std::string_view effect_id);
    [[nodiscard]] std::string vfx_graph_json(std::string_view graph_id) const;
    [[nodiscard]] std::string vfx_gpu_program_json(std::string_view graph_id) const;
    [[nodiscard]] std::string vfx_preview_json(std::string_view graph_id, std::uint64_t seed,
                                               std::uint32_t steps, float fixed_delta_seconds,
                                               std::size_t max_particles = 32) const;
    [[nodiscard]] std::string vfx_observation_json(std::size_t max_particles = 32) const;
    [[nodiscard]] std::string vfx_benchmark_json(std::string_view graph_id,std::uint32_t particle_count,
                                                 std::uint32_t steps,float fixed_delta_seconds) const;
    [[nodiscard]] std::string vfx_spawn_json(std::string_view graph_id, Transform position, std::uint64_t seed);
    [[nodiscard]] std::string vfx_plan_graph_patch_json(std::string_view graph_id, std::string_view patch_json,
                                                        std::uint64_t base_revision) const;
    [[nodiscard]] std::string vfx_apply_graph_plan_json(std::string_view plan_json, bool dry_run);
    [[nodiscard]] std::string vfx_undo_graph_json(std::uint64_t expected_revision);
    [[nodiscard]] std::span<const VfxRuntime::Particle> vfx_particles() const { return vfx_runtime_.particles(); }
    [[nodiscard]] std::string network_snapshot_preview_json(std::uint64_t tick, std::size_t max_entities = 256) const;
    [[nodiscard]] std::string network_loopback_verify_json() const;
    [[nodiscard]] std::string spawn_prefab_json(std::string_view source_entity_id, std::string_view new_entity_id,
                                                 std::string_view display_name, Transform position);
    [[nodiscard]] std::string export_prefab_json(std::string_view entity_id) const;
    [[nodiscard]] std::string instantiate_prefab_json(std::string_view prefab_json,std::string_view new_entity_id,
                                                       std::string_view display_name,Transform position);
    [[nodiscard]] std::string despawn_entity_json(std::string_view entity_id);
    [[nodiscard]] std::string save_capture_json() const;
    [[nodiscard]] std::string save_restore_json(std::string_view document_json);
    [[nodiscard]] std::string replay_start_json();
    [[nodiscard]] std::string replay_stop_json();
    [[nodiscard]] std::string replay_apply_json(std::string_view replay_json);
    [[nodiscard]] std::vector<GameplayPersistenceRequest> consume_persistence_requests();
    void complete_persistence_request(const GameplayPersistenceRequest& request,bool success,
                                      std::string_view code,std::string_view detail);
    [[nodiscard]] std::string scripting_abi_json() const;
    [[nodiscard]] std::string scripting_observation_json() const;
    [[nodiscard]] std::string scripting_attach_json(std::string_view instance_id,std::string_view entity_id,
                                                     std::string_view assembly_asset,std::string_view type_name);
    [[nodiscard]] std::string scripting_invoke_json(std::string_view instance_id,std::string_view callback,
                                                     std::string_view arguments_json);
    [[nodiscard]] std::string scripting_project_configure_json(const std::filesystem::path& project_root,
                                                                const std::filesystem::path& script_project);
    [[nodiscard]] std::string scripting_project_load_assembly_json(const std::filesystem::path& assembly,
                                                                   std::string_view configuration = "Release");
    [[nodiscard]] std::string scripting_project_compile_json(std::string_view configuration);
    [[nodiscard]] std::string scripting_project_types_json() const;
    [[nodiscard]] std::string scripting_project_observation_json() const;
    [[nodiscard]] std::string scripting_debug_attach_json() const;
    [[nodiscard]] std::string scripting_debug_session_start_json();
    [[nodiscard]] std::string scripting_debug_session_status_json() const;
    [[nodiscard]] std::string scripting_debug_session_request_json(std::string_view command,
                                                                    std::string_view arguments_json = "{}",
                                                                    std::uint32_t timeout_ms = 5000U);
    [[nodiscard]] std::string scripting_debug_session_events_json();
    [[nodiscard]] std::string scripting_debug_session_stop_json(std::uint32_t timeout_ms = 2000U);
    [[nodiscard]] std::string inspector_document_json(std::string_view entity_id) const;
    [[nodiscard]] std::string semantic_ui_document_json(std::string_view entity_id, std::string_view locale = "en-US") const;
    [[nodiscard]] std::string semantic_ui_observation_json(std::string_view entity_id, const SemanticUiQuery& query,
                                                            std::string_view locale = "en-US") const;
    [[nodiscard]] std::string semantic_ui_project_document_json(std::string_view locale = "en-US") const;
    // Invoke one action declared by the canonical project UI document. Both
    // Retained UI and Agent adapters enter through this boundary; callers do
    // not supply a script callback or binding and therefore cannot bypass the
    // authored node/action relationship.
    [[nodiscard]] std::string project_ui_action_invoke_json(
        std::string_view node_id, std::string_view action_id,
        std::string_view event_kind = "invoke", std::string_view value_json = "null",
        std::optional<std::uint64_t> expected_document_revision = std::nullopt,
        bool dry_run = false, std::string_view source = "ui.retained",
        std::uint64_t sequence = 0);
    [[nodiscard]] std::string semantic_ui_delta_json(std::string_view entity_id, std::uint64_t since_revision,
                                                     const SemanticUiDeltaQuery& query,
                                                     std::string_view locale = "en-US") const;
    [[nodiscard]] std::string retained_ui_preview_json(std::string_view entity_id, std::uint32_t width = 960,
                                                       std::uint32_t height = 720, float density_scale = 1.0F,
                                                       std::string_view locale = "en-US") const;
    [[nodiscard]] std::string schema_json() const;
    [[nodiscard]] std::string managed_bindings_source() const;
    [[nodiscard]] std::string canonical_scene_json() const;
    // Canonical authoring document with durable runtime-authored component
    // values projected from ECS state. Transient subsystem state is excluded.
    [[nodiscard]] std::string runtime_authoring_scene_json() const;
    [[nodiscard]] static std::string change_plan_json(const TransformChangePlan& plan);
    [[nodiscard]] static std::string property_change_plan_json(const PropertyChangePlan& plan);
    [[nodiscard]] static std::string action_receipt_json(const ActionReceipt& receipt);

private:
    struct WorldHistoryEntry final {
        SemanticDelta delta;
        std::optional<SceneDocument> scene_before;
        std::optional<SceneDocument> scene_after;
    };

    void clear_scene_entities();
    [[nodiscard]] std::vector<WorldEntityView> entity_views_impl(
        std::optional<TilemapViewQuery> tilemap_query,bool include_skeletal_pose) const;
    void synchronize_managed_scripts();
    [[nodiscard]] SceneLoadResult load_scene_internal(const SceneDocument& document, bool reset_history);
    [[nodiscard]] SceneDocument runtime_authoring_scene_document() const;
    void consume_animation_cues(float delta_seconds);
    void configure_animation_player(AnimationPlayer& player) const;
    void configure_animation_graph_player(AnimationPlayer& player) const;
    [[nodiscard]] std::optional<AnimationPoseExecutionRequest> animation_graph_pose_request(
        const AnimationPlayer& player) const;
    [[nodiscard]] std::optional<std::string> property_value_json(std::string_view entity_id,
                                                                 std::string_view property) const;
    [[nodiscard]] bool set_property_json(std::string_view entity_id, std::string_view property,
                                         std::string_view value_json, std::string& error);

    flecs::world world_;
    std::uint64_t revision_{};
    std::string scene_guid_;
    std::string scene_name_;
    std::string scene_source_uri_;
    std::string scene_source_baseline_;
    bool scene_source_existed_at_load_{};
    SceneDocument scene_document_;
    std::unordered_map<std::string, flecs::entity_t> entity_ids_;
    std::vector<SemanticDelta> recent_deltas_;
    std::vector<WorldHistoryEntry> undo_stack_;
    std::vector<WorldHistoryEntry> redo_stack_;
    PhysicsRuntime physics_runtime_;
    AnimationRuntime animation_runtime_;
    AnimationStateMachineLibrary animation_state_machines_;
    AnimationGraphLibrary animation_graphs_;
    SpriteAssetLibrary sprite_assets_;
    TilemapAssetLibrary tilemap_assets_;
    InputActionRuntime input_runtime_;
    std::string project_hud_document_json_;
    AudioMixerRuntime audio_runtime_;
    GameplayRuntime gameplay_runtime_;
    GameplayAbilityRuntime gameplay_ability_runtime_;
    std::unordered_map<std::string,PhysicsContact> active_script_contacts_;
    std::uint64_t last_animation_cue_event_sequence_{};
    VfxRuntime vfx_runtime_;
    ManagedScriptRuntime scripting_runtime_;
    struct RecordedInput final { std::uint64_t sequence{}; std::uint64_t tick{}; std::string source; float value{}; };
    std::uint64_t simulation_tick_{};
    bool replay_recording_{};
    std::uint64_t replay_start_tick_{};
    std::string replay_initial_save_json_;
    std::uint64_t next_recorded_input_sequence_{1};
    std::vector<RecordedInput> recorded_inputs_;
    std::uint64_t next_persistence_request_sequence_{1};
    std::vector<GameplayPersistenceRequest> persistence_requests_;
};

} // namespace noemancer
