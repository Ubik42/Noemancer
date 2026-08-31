#pragma once

#include "engine/physics_constraint_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct SceneVector3 final {
    double x{};
    double y{};
    double z{};
};

struct SceneTransform final {
    SceneVector3 position;
    SceneVector3 scale{1.0, 1.0, 1.0};
    SceneVector3 rotation_euler_degrees;
};

struct SceneVelocity final {
    SceneVector3 linear;
    SceneVector3 angular;
};

struct SceneRigidBody final {
    std::string motion_type{"dynamic"};
    double mass{1.0};
    double gravity_factor{1.0};
    double linear_damping{0.05};
    double angular_damping{0.05};
    bool continuous_collision{false};
    bool allow_sleeping{true};
    std::uint32_t collision_layer{1U};
    std::uint32_t collision_mask{0xffffffffU};
};

struct SceneBoxCollider final {
    SceneVector3 half_extents{0.5, 0.5, 0.5};
    double friction{0.5};
    double restitution{0.0};
    bool is_trigger{};
};

struct SceneSphereCollider final {
    double radius{0.5};
    double friction{0.5};
    double restitution{0.0};
    bool is_trigger{};
};

struct SceneCapsuleCollider final {
    double radius{0.5};
    double half_height{0.5};
    double friction{0.5};
    double restitution{0.0};
    bool is_trigger{};
};

struct SceneCharacterMotor2D final {
    double maximum_speed{7.0};
    double ground_acceleration{55.0};
    double air_acceleration{24.0};
    double ground_deceleration{70.0};
    double jump_speed{10.5};
    double maximum_fall_speed{24.0};
    double coyote_time_seconds{0.10};
    double jump_buffer_seconds{0.12};
    double ground_probe_distance{0.16};
    double minimum_ground_normal_y{0.65};
    double jump_release_velocity_factor{0.45};
};

struct ScenePlatform2D final {
    std::string collision_mode{"solid"};
    SceneVector3 motion_axis{1.0, 0.0, 0.0};
    double motion_distance{};
    double motion_period_seconds{2.0};
    double motion_phase{};
};

struct SceneConvexHullCollider final {
    std::vector<SceneVector3> points;
    double friction{0.5};
    double restitution{0.0};
    bool is_trigger{};
};

struct SceneAnimationPlayer final {
    std::string clip_asset;
    double playback_speed{1.0};
    bool looping{true};
    bool playing{true};
    std::string next_clip_asset;
    double transition_duration_seconds{};
    std::string root_motion_mode{"ignore"};
    std::string state_machine_asset;
    std::string animation_graph_asset;
};

struct SceneCamera final {
    SceneVector3 target;
    double vertical_fov_degrees{45.0};
    double near_clip{0.1};
    double far_clip{100.0};
    bool primary{true};
    std::string projection{"perspective"};
    double orthographic_height{10.0};
};

struct SceneCameraFollow2D final {
    std::string target_entity_id;
    SceneVector3 position_offset{0.0, 2.0, 17.0};
    SceneVector3 dead_zone{1.5, 0.8, 0.0};
    double look_ahead_distance{1.5};
    double smoothing{7.0};
    SceneVector3 minimum_center{-10000.0, -10000.0, -10000.0};
    SceneVector3 maximum_center{10000.0, 10000.0, 10000.0};
};

struct SceneDirectionalLight final {
    SceneVector3 direction{-0.55, -1.0, -0.35};
    SceneVector3 color{1.0, 0.96, 0.88};
    double intensity{0.95};
    double ambient_intensity{0.18};
    bool casts_shadows{true};
};

struct SceneLocalLight final {
    std::string kind{"point"};
    SceneVector3 color{1.0, 0.95, 0.85};
    double luminous_power_lumens{800.0};
    double range_meters{8.0};
    SceneVector3 direction{0.0, -1.0, 0.0};
    double inner_cone_degrees{25.0};
    double outer_cone_degrees{35.0};
    double source_radius_meters{0.05};
    bool casts_shadows{};
};

struct SceneMeshRenderer final {
    std::string mesh_asset;
    bool visible{true};
    bool casts_shadows{true};
    bool receives_shadows{true};
};

struct SceneSpriteRenderer final {
    std::string sprite_asset;
    std::string clip;
    double playback_speed{1.0};
    bool playing{true};
    bool flip_x{};
    bool flip_y{};
    std::string sorting_layer{"default"};
    std::int32_t sorting_order{};
    bool visible{true};
};

struct SceneTilemapRenderer final {
    std::string tilemap_asset;
    bool visible{true};
    bool collision_enabled{true};
};

struct ScenePbrMaterial final {
    SceneVector3 base_color{0.8, 0.8, 0.8};
    double metallic{};
    double roughness{0.6};
    std::string base_color_texture;
    SceneVector3 emissive_color{};
    double emissive_intensity{};
};

struct SceneManagedScript final {
    std::string instance_id;
    std::string assembly_asset{"project.script"};
    std::string type_name;
    bool enabled{true};
    std::string properties_json{"{}"};
};

struct SceneEntityDocument final {
    std::string guid;
    std::string name;
    std::string parent_guid;
    std::optional<SceneTransform> transform;
    std::optional<SceneVelocity> velocity;
    std::optional<SceneRigidBody> rigid_body;
    std::optional<SceneBoxCollider> box_collider;
    std::optional<SceneSphereCollider> sphere_collider;
    std::optional<SceneCapsuleCollider> capsule_collider;
    std::optional<SceneCharacterMotor2D> character_motor_2d;
    std::optional<ScenePlatform2D> platform_2d;
    std::optional<SceneConvexHullCollider> convex_hull_collider;
    std::optional<SceneAnimationPlayer> animation_player;
    std::optional<SceneCamera> camera;
    std::optional<SceneCameraFollow2D> camera_follow_2d;
    std::optional<SceneDirectionalLight> directional_light;
    std::optional<SceneLocalLight> local_light;
    std::optional<SceneMeshRenderer> mesh_renderer;
    std::optional<SceneSpriteRenderer> sprite_renderer;
    std::optional<SceneTilemapRenderer> tilemap_renderer;
    std::optional<ScenePbrMaterial> pbr_material;
    std::optional<SceneManagedScript> managed_script;
};

struct SceneDocument final {
    std::string schema{"noemancer.scene/0.1"};
    std::string scene_guid;
    std::string name;
    std::string source_uri;
    std::vector<SceneEntityDocument> entities;
    std::vector<PhysicsConstraintSpec> physics_constraints;
};

struct SceneDocumentError final {
    std::string code;
    std::string path;
    std::string message;
};

struct SceneDocumentParseResult final {
    std::optional<SceneDocument> document;
    std::vector<SceneDocumentError> errors;

    [[nodiscard]] explicit operator bool() const noexcept { return document.has_value(); }
};

class SceneDocumentCodec final {
public:
    [[nodiscard]] static SceneDocumentParseResult parse_json(
        std::string_view json,
        std::string source_uri = {});
    [[nodiscard]] static std::vector<SceneDocumentError> validate(const SceneDocument& document);
    [[nodiscard]] static std::string write_canonical_json(const SceneDocument& document);
};

[[nodiscard]] SceneDocument make_bootstrap_scene_document();
[[nodiscard]] SceneDocument make_render_stress_scene_document(std::uint32_t instance_count,
                                                               std::uint32_t offscreen_percent=0);
[[nodiscard]] SceneDocument make_animation_physics_stress_scene_document();

} // namespace noemancer
