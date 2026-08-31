#include "runtime/scene_raytracing_geometry_source_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace noemancer {

namespace {

bool ascii_equal_insensitive(const std::string_view left,
                             const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](const char lhs, const char rhs) {
        const auto lower = [](const char value) noexcept {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        };
        return lower(lhs) == lower(rhs);
    });
}

SceneRayTracingAlphaMode alpha_mode(const std::string_view value) noexcept {
    if (ascii_equal_insensitive(value, "MASK")) return SceneRayTracingAlphaMode::mask;
    if (ascii_equal_insensitive(value, "BLEND")) return SceneRayTracingAlphaMode::blend;
    if (ascii_equal_insensitive(value, "OPAQUE") || value.empty()) {
        return SceneRayTracingAlphaMode::opaque;
    }
    // The cache has no fourth alpha enum.  Map an unknown decoder value to
    // BLEND so it is conservatively excluded instead of becoming an opaque
    // AS primitive by accident.
    return SceneRayTracingAlphaMode::blend;
}

std::string primitive_id(const GltfDecodedPrimitive& primitive,
                         const std::size_t ordinal) {
    std::string base;
    if (!primitive.node_name.empty() && !primitive.mesh_name.empty()) {
        base = primitive.node_name + "/" + primitive.mesh_name;
    } else if (!primitive.node_name.empty()) {
        base = primitive.node_name;
    } else if (!primitive.mesh_name.empty()) {
        base = primitive.mesh_name;
    } else {
        base = "primitive";
    }
    // The source order is part of the decoded payload.  Appending it makes
    // empty and repeated names deterministic and globally unique per mesh.
    base += "#";
    base += std::to_string(ordinal);
    return base;
}

} // namespace

SceneRayTracingGeometryInput make_scene_raytracing_geometry_input(
    const std::string_view asset_id,
    const DecodedSceneAsset& decoded_asset) {
    SceneRayTracingGeometryInput result;
    result.geometry_id = std::string(asset_id);
    result.source = SceneRayTracingGeometrySourceKind::imported;
    result.positions.reserve(decoded_asset.vertices.size());
    for (const auto& vertex : decoded_asset.vertices) {
        result.positions.push_back(vertex.position);
    }
    result.indices = decoded_asset.indices;
    result.primitives.reserve(decoded_asset.primitives.size());
    for (std::size_t ordinal = 0U; ordinal < decoded_asset.primitives.size(); ++ordinal) {
        const auto& decoded = decoded_asset.primitives[ordinal];
        SceneRayTracingPrimitiveInput primitive;
        primitive.primitive_id = primitive_id(decoded, ordinal);
        primitive.first_index = decoded.first_index;
        primitive.index_count = decoded.index_count;
        primitive.enabled = true;
        primitive.alpha_mode = alpha_mode(decoded.alpha_mode);
        primitive.skinned = decoded.skin >= 0;
        result.primitives.push_back(std::move(primitive));
    }
    return result;
}

SceneRayTracingGeometryInput make_builtin_scene_raytracing_geometry_input(
    const std::string_view geometry_id,
    const std::span<const std::array<float, 3U>> positions,
    const std::span<const std::uint32_t> indices,
    const std::span<const SceneRayTracingPrimitiveInput> primitive_ranges) {
    SceneRayTracingGeometryInput result;
    result.geometry_id = std::string(geometry_id);
    result.source = SceneRayTracingGeometrySourceKind::builtin;
    result.positions.assign(positions.begin(), positions.end());
    result.indices.assign(indices.begin(), indices.end());
    result.primitives.assign(primitive_ranges.begin(), primitive_ranges.end());
    return result;
}

} // namespace noemancer
