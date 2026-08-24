#pragma once

#include "engine/render_world.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace noemancer {

struct ClusteredLightingConfig final {
    std::uint32_t tiles_x{16};
    std::uint32_t tiles_y{9};
    std::uint32_t depth_slices{24};
    std::uint32_t maximum_lights{256};
    std::uint32_t maximum_lights_per_cluster{64};
};

struct ClusteredLightingCamera final {
    std::array<float, 3> position{};
    std::array<float, 3> target{};
    float vertical_fov_degrees{45.0F};
    float aspect_ratio{16.0F / 9.0F};
    float near_clip{0.1F};
    float far_clip{100.0F};
    bool orthographic{};
    float orthographic_height{10.0F};
};

struct ClusterLightRange final {
    std::uint32_t offset{};
    std::uint32_t count{};
};

struct ClusteredLightingAssignment final {
    std::vector<ClusterLightRange> clusters;
    std::vector<std::uint32_t> light_indices;
    std::uint32_t accepted_light_count{};
    std::uint32_t dropped_light_count{};
    std::uint32_t overflowed_assignments{};
};

[[nodiscard]] ClusteredLightingAssignment build_clustered_lighting(
    const ClusteredLightingConfig& config,
    const ClusteredLightingCamera& camera,
    std::span<const RenderLocalLightSnapshot> lights);

} // namespace noemancer
