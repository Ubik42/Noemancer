#pragma once

#include "engine/physics_constraint_types.hpp"
#include "engine/gltf_mesh.hpp"

#include <string>
#include <string_view>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace noemancer {

enum class PhysicsMotionType { static_body, dynamic_body, kinematic_body };
enum class PhysicsShapeType { box, sphere, capsule, convex_hull };

struct PhysicsBodyState final {
    std::string entity_id;
    PhysicsMotionType motion_type{PhysicsMotionType::dynamic_body};
    PhysicsShapeType shape_type{PhysicsShapeType::box};
    float position_x{};
    float position_y{};
    float position_z{};
    float rotation_x{};
    float rotation_y{};
    float rotation_z{};
    float rotation_w{1.0F};
    float velocity_x{};
    float velocity_y{};
    float velocity_z{};
    float half_x{0.5F};
    float half_y{0.5F};
    float half_z{0.5F};
    float radius{0.5F};
    float half_height{0.5F};
    std::vector<std::array<float, 3>> convex_points;
    float gravity_factor{1.0F};
    float linear_damping{0.05F};
    float restitution{};
    float friction{0.5F};
    float mass{1.0F};
    bool one_way{};
    bool is_trigger{};
    // Restrict this body to the XY gameplay plane and disable every rotation.
    // This is intentionally opt-in so ordinary 3D rigid bodies retain all six
    // degrees of freedom. CharacterMotor2D supplies this flag at the world
    // bridge; the physics backend translates it to Jolt's allowed-DOF mask.
    bool constrain_to_2d{};
    float angular_velocity_x{};
    float angular_velocity_y{};
    float angular_velocity_z{};
    float angular_damping{0.05F};
    bool continuous_collision{};
    bool allow_sleeping{true};
    // Engine-owned collision category and query mask. The defaults preserve
    // the historical behavior: every body is in category 1 and can interact
    // with every category. A zero layer is useful for explicitly non-colliding
    // sensor-only bodies.
    std::uint32_t collision_layer{1U};
    std::uint32_t collision_mask{0xffffffffU};
};

struct PhysicsContact final {
    std::string body_a;
    std::string body_b;
    float normal_x{};
    float normal_y{1.0F};
    float normal_z{};
    float penetration{};
    bool is_trigger{};
};

struct PhysicsRayCastHit final {
    bool hit{};
    std::string entity_id;
    float fraction{1.0F};
    float position_x{};
    float position_y{};
    float position_z{};
};

struct PhysicsSweepHit final {
    bool hit{};
    std::string entity_id;
    float fraction{1.0F};
    float position_x{};
    float position_y{};
    float position_z{};
    float normal_x{};
    float normal_y{1.0F};
    float normal_z{};
    float penetration_depth{};
};

// Query filters are deliberately plain data so the same contract can be used
// by native gameplay, the editor and the Agent command layer without leaking
// Jolt types. ignored_entity_ids is a borrowed view and only needs to remain
// alive for the duration of the query call.
struct PhysicsQueryFilter final {
    std::uint32_t layer{1U};
    std::uint32_t mask{0xffffffffU};
    std::string_view ignored_entity_id{};
    std::span<const std::string_view> ignored_entity_ids{};
};

struct PhysicsOverlapHit final {
    std::string entity_id;
    float position_x{};
    float position_y{};
    float position_z{};
    float normal_x{};
    float normal_y{1.0F};
    float normal_z{};
    float penetration_depth{};
    bool is_trigger{};
};

struct PhysicsOverlapResult final {
    std::vector<PhysicsOverlapHit> hits;
    bool truncated{};
};

inline constexpr std::size_t physics_query_maximum_overlap_hits = 128U;

class PhysicsRuntime final {
public:
    PhysicsRuntime();
    ~PhysicsRuntime();
    PhysicsRuntime(const PhysicsRuntime&) = delete;
    PhysicsRuntime& operator=(const PhysicsRuntime&) = delete;
    void step(std::vector<PhysicsBodyState>& bodies, float delta_seconds);
    // The explicit overload applies a complete, stable-ID constraint snapshot
    // to the same Jolt PhysicsSystem used by the bodies.  The snapshot is
    // copied into engine-owned state before the simulation step, so callers
    // may pass a transient span.  Existing callers can keep using the legacy
    // no-constraint overload unchanged.
    [[nodiscard]] PhysicsConstraintResult step(std::vector<PhysicsBodyState>& bodies, float delta_seconds,
                                               std::span<const PhysicsConstraintSpec> constraints);
    [[nodiscard]] std::optional<PhysicsConstraintObservation> observe_constraint(
        std::string_view constraint_id) const;
    [[nodiscard]] std::vector<PhysicsConstraintObservation> observe_constraints() const;
    [[nodiscard]] std::uint64_t constraint_revision() const noexcept;
    [[nodiscard]] PhysicsRayCastHit ray_cast(float origin_x, float origin_y, float origin_z,
                                             float direction_x, float direction_y, float direction_z) const;
    [[nodiscard]] PhysicsRayCastHit ray_cast(float origin_x, float origin_y, float origin_z,
                                             float direction_x, float direction_y, float direction_z,
                                             const PhysicsQueryFilter& filter) const;
    [[nodiscard]] PhysicsSweepHit sphere_sweep(float origin_x,float origin_y,float origin_z,
                                               float direction_x,float direction_y,float direction_z,
                                               float radius,std::string_view ignored_entity_id={}) const;
    [[nodiscard]] PhysicsSweepHit sphere_sweep(float origin_x,float origin_y,float origin_z,
                                               float direction_x,float direction_y,float direction_z,
                                               float radius,const PhysicsQueryFilter& filter) const;
    [[nodiscard]] PhysicsSweepHit box_sweep(float origin_x, float origin_y, float origin_z,
                                            float direction_x, float direction_y, float direction_z,
                                            float half_x, float half_y, float half_z,
                                            const PhysicsQueryFilter& filter = {}) const;
    [[nodiscard]] PhysicsSweepHit capsule_sweep(float origin_x, float origin_y, float origin_z,
                                                float direction_x, float direction_y, float direction_z,
                                                float radius, float half_height,
                                                const PhysicsQueryFilter& filter = {}) const;
    [[nodiscard]] PhysicsOverlapResult overlap_box(float center_x, float center_y, float center_z,
                                                    float half_x, float half_y, float half_z,
                                                    const PhysicsQueryFilter& filter = {},
                                                    std::size_t maximum_hits = physics_query_maximum_overlap_hits) const;
    [[nodiscard]] PhysicsOverlapResult overlap_sphere(float center_x, float center_y, float center_z,
                                                      float radius,
                                                      const PhysicsQueryFilter& filter = {},
                                                      std::size_t maximum_hits = physics_query_maximum_overlap_hits) const;
    [[nodiscard]] PhysicsOverlapResult overlap_capsule(float center_x, float center_y, float center_z,
                                                       float radius, float half_height,
                                                       const PhysicsQueryFilter& filter = {},
                                                       std::size_t maximum_hits = physics_query_maximum_overlap_hits) const;
    [[nodiscard]] bool apply_force(std::string_view entity_id,
                                   float force_x, float force_y, float force_z);
    [[nodiscard]] bool apply_impulse(std::string_view entity_id,
                                     float impulse_x, float impulse_y, float impulse_z);
    [[nodiscard]] bool apply_angular_impulse(std::string_view entity_id,
                                             float impulse_x, float impulse_y, float impulse_z);
    [[nodiscard]] const std::vector<PhysicsContact>& contacts() const noexcept { return contacts_; }
    [[nodiscard]] std::string_view backend_id() const noexcept { return "jolt/5.6.0"; }

private:
    [[nodiscard]] PhysicsSweepHit shape_sweep(PhysicsShapeType shape_type,
                                              float origin_x, float origin_y, float origin_z,
                                              float direction_x, float direction_y, float direction_z,
                                              float size_x, float size_y, float size_z,
                                              const PhysicsQueryFilter& filter) const;
    [[nodiscard]] PhysicsOverlapResult overlap_shape(PhysicsShapeType shape_type,
                                                      float center_x, float center_y, float center_z,
                                                      float size_x, float size_y, float size_z,
                                                      const PhysicsQueryFilter& filter,
                                                      std::size_t maximum_hits) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<PhysicsContact> contacts_;
};

struct AnimationKeyframe final {
    float time{};
    float value{};
};

struct SkeletalPose final {
    static constexpr std::size_t maximum_joints = 64U;
    bool valid{};
    std::string skeleton_asset;
    std::string clip_asset;
    std::vector<std::array<float, 16>> skinning_matrices;
    struct JointDebug final {
        std::string name;
        int parent{-1};
        float model_x{};
        float model_y{};
        float model_z{};
    };
    std::vector<JointDebug> joints;
};

struct RootMotionDelta final {
    bool valid{};
    float x{};
    float y{};
    float z{};
};

enum class AnimationCompressionMode {
    ozz_runtime_baseline,
    ozz_hierarchical_key_reduction
};

struct AnimationCompressionEvidence final {
    std::string schema_version{"noemancer.animation-compression/0.1"};
    std::string backend{"ozz-animation/0.17.0"};
    AnimationCompressionMode requested_mode{AnimationCompressionMode::ozz_runtime_baseline};
    bool optimizer_attempted{};
    bool optimizer_applied{};
    bool fallback_used{};
    float hierarchical_tolerance_meters{0.001F};
    float measurement_distance_meters{0.1F};
    std::size_t input_raw_resident_bytes{};
    std::size_t optimized_raw_resident_bytes{};
    std::size_t input_raw_archive_bytes{};
    std::size_t optimized_raw_archive_bytes{};
    std::size_t baseline_runtime_resident_bytes{};
    std::size_t selected_runtime_resident_bytes{};
    std::size_t baseline_runtime_archive_bytes{};
    std::size_t selected_runtime_archive_bytes{};
    std::size_t skeleton_archive_bytes{};
    std::string input_raw_archive_hash;
    std::string optimized_raw_archive_hash;
    std::string baseline_runtime_archive_hash;
    std::string selected_runtime_archive_hash;
    std::string skeleton_archive_hash;
    std::size_t input_translation_keys{};
    std::size_t input_rotation_keys{};
    std::size_t input_scale_keys{};
    std::size_t selected_translation_keys{};
    std::size_t selected_rotation_keys{};
    std::size_t selected_scale_keys{};
};

struct AnimationCompileResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string skeleton_asset;
    std::string clip_asset;
    std::size_t joint_count{};
    float duration{};
    AnimationCompressionEvidence compression;
};

struct AnimationClipComparisonResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::size_t sample_count{};
    std::size_t joint_count{};
    float maximum_local_translation_error_meters{};
    float maximum_local_rotation_error_degrees{};
    float maximum_local_scale_error{};
    float maximum_model_translation_error_meters{};
    float maximum_model_probe_error_meters{};
    float maximum_skinning_matrix_absolute_error{};
    float maximum_root_motion_delta_error_meters{};
};

struct AnimationCookedArtifactResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string schema_version{"noemancer.animation-cooked-artifact/0.1"};
    std::string asset_id;
    std::string source_hash;
    std::string payload_hash;
    std::vector<std::byte> payload;
    std::vector<std::string> clip_assets;
    std::size_t joint_count{};
};

struct AnimationCookedArtifactLoadResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::string schema_version{"noemancer.animation-cooked-artifact/0.1"};
    std::string asset_id;
    std::string source_hash;
    std::string payload_hash;
    std::vector<std::string> clip_assets;
    std::size_t joint_count{};
};

// These are engine-owned values for the runtime slice of an Animation Graph.
// They intentionally do not expose ozz types: sampling and blending stay
// private to AnimationRuntime, while callers can submit a deterministic,
// plain-data layer request.
enum class AnimationPoseLayerMode { override_layer, additive };

struct AnimationPoseMaskJoint final {
    std::string name;
    float weight{1.0F};
};

struct AnimationPoseMask final {
    std::string id;
    bool include_descendants{true};
    std::vector<AnimationPoseMaskJoint> joints;
};

struct AnimationPoseLayer final {
    std::string id;
    std::string clip_asset;
    float time{};
    // Optional second local pose (for example, a resolved Blend 1D pair).
    // secondary_weight is the conventional [0,1] interpolation alpha.
    std::string secondary_clip_asset;
    float secondary_time{};
    float secondary_weight{};
    AnimationPoseLayerMode mode{AnimationPoseLayerMode::override_layer};
    // Additive sources are ordinary sampled local poses; the adapter derives
    // their rest-relative local delta before passing them to ozz.
    float weight{1.0F};
    std::string mask_id;

    // Optional weights in the runtime skeleton's depth-first joint order.
    // When omitted, the layer uses a unit weight for every joint. If mask_id
    // is present, these values are multiplied by the resolved mask weights.
    std::vector<float> joint_weights;
};

struct AnimationPoseExecutionRequest final {
    std::string base_clip_asset;
    float base_time{};
    bool base_looping{true};
    // The base can also be a resolved two-source local blend.
    std::string base_secondary_clip_asset;
    float base_secondary_time{};
    float base_secondary_weight{};
    bool base_secondary_looping{true};
    std::vector<AnimationPoseLayer> layers;
    std::vector<AnimationPoseMask> masks;
};

struct AnimationPoseExecutionResult final {
    bool success{};
    std::string code;
    std::string detail;
    SkeletalPose pose;
};

class AnimationRuntime final {
public:
    AnimationRuntime();
    ~AnimationRuntime();
    AnimationRuntime(const AnimationRuntime&) = delete;
    AnimationRuntime& operator=(const AnimationRuntime&) = delete;
    [[nodiscard]] float duration(std::string_view clip_asset) const noexcept;
    [[nodiscard]] float advance_time(float time, float delta_seconds, float speed, bool looping, bool playing,
                                     std::string_view clip_asset) const noexcept;
    [[nodiscard]] float sample_translation_y(std::string_view clip_asset, float time) const noexcept;
    [[nodiscard]] SkeletalPose sample_skeletal_pose(std::string_view clip_asset, float time) const;
    [[nodiscard]] SkeletalPose sample_blended_skeletal_pose(std::string_view source_clip, float source_time,
                                                            std::string_view target_clip, float target_time,
                                                            float target_weight) const;
    [[nodiscard]] AnimationPoseExecutionResult sample_layered_skeletal_pose(
        const AnimationPoseExecutionRequest& request) const;
    [[nodiscard]] AnimationPoseExecutionResult evaluate_layered_skeletal_pose(
        const AnimationPoseExecutionRequest& request) const {
        return sample_layered_skeletal_pose(request);
    }
    [[nodiscard]] RootMotionDelta root_motion_delta(std::string_view clip_asset, float previous_time,
                                                    float current_time, bool looping, float playback_speed) const;
    [[nodiscard]] AnimationCompileResult compile_gltf_asset(std::string_view asset_id, const GltfMeshData& source,
                                                            std::size_t skin_index, std::size_t animation_index,
                                                            AnimationCompressionMode compression_mode =
                                                                AnimationCompressionMode::ozz_runtime_baseline);
    [[nodiscard]] AnimationClipComparisonResult compare_compiled_clips(std::string_view reference_clip_asset,
                                                                       std::string_view candidate_clip_asset,
                                                                       std::size_t sample_count = 257U) const;
    [[nodiscard]] AnimationCookedArtifactResult cook_gltf_animation_artifact(
        std::string_view asset_id, std::string_view source_hash, const GltfMeshData& source,
        std::size_t skin_index, std::size_t animation_index,
        AnimationCompressionMode compression_mode = AnimationCompressionMode::ozz_runtime_baseline);
    [[nodiscard]] AnimationCookedArtifactLoadResult load_cooked_animation_artifact(
        std::span<const std::byte> payload, std::string_view expected_asset_id = {},
        std::string_view expected_source_hash = {}, std::string_view expected_payload_hash = {});
    [[nodiscard]] std::string_view backend_id() const noexcept { return "ozz-animation/0.17.0"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace noemancer
