#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace noemancer {

struct ShadowVec3 final { float x{}; float y{}; float z{}; };
struct ShadowMat4 final { std::array<float,16> value{}; };

struct CascadedShadowConfig final {
    static constexpr std::uint32_t maximum_cascades=4;
    std::uint32_t cascade_count{4};
    std::uint32_t resolution{2048};
    float maximum_distance{80.0F};
    float split_lambda{0.65F};
    float depth_padding{10.0F};
};

struct ShadowCascade final {
    float near_distance{};
    float far_distance{};
    float radius{};
    float world_units_per_texel{};
    ShadowVec3 snapped_center{};
    ShadowMat4 view_projection{};
};

struct CascadedShadowPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    CascadedShadowConfig config;
    std::array<ShadowCascade,CascadedShadowConfig::maximum_cascades> cascades{};
    std::uint64_t texture_bytes{};
};

[[nodiscard]] CascadedShadowPlan build_cascaded_shadow_plan(
    ShadowVec3 camera_position,
    ShadowVec3 camera_target,
    ShadowVec3 camera_up,
    float vertical_fov_radians,
    float aspect_ratio,
    float near_clip,
    float far_clip,
    ShadowVec3 light_direction,
    const CascadedShadowConfig& config = {});

} // namespace noemancer
