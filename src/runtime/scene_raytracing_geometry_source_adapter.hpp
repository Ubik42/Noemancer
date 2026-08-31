#pragma once

#include "engine/gltf_mesh.hpp"
#include "runtime/scene_raytracing_geometry_cache.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace noemancer {

// Convert an already decoded Engine payload into the cache's plain-data input.
// The adapter deliberately does not validate positions, indices or ranges;
// SceneRayTracingGeometryCache remains the single fail-closed validator.
[[nodiscard]] SceneRayTracingGeometryInput
make_scene_raytracing_geometry_input(std::string_view asset_id,
                                     const DecodedSceneAsset& decoded_asset);

// Lightweight path for renderer-owned builtin meshes.  The caller supplies
// explicit primitive ranges so cubes, planes and spheres use the same cache
// contract as imported geometry without a second validation path.
[[nodiscard]] SceneRayTracingGeometryInput
make_builtin_scene_raytracing_geometry_input(
    std::string_view geometry_id,
    std::span<const std::array<float, 3U>> positions,
    std::span<const std::uint32_t> indices,
    std::span<const SceneRayTracingPrimitiveInput> primitive_ranges);

} // namespace noemancer
