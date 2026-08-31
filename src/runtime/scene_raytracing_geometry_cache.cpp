#include "runtime/scene_raytracing_geometry_cache.hpp"

#include "runtime/raytracing_context_session.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {

namespace {

constexpr std::uint64_t fnv_offset_basis = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

std::string bounded_text(const std::string_view value) {
    const auto length = std::min(value.size(), scene_raytracing_geometry_cache_max_text_bytes);
    return std::string(value.substr(0U, length));
}

void hash_byte(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= fnv_prime;
}

void hash_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
}

void hash_string(std::uint64_t& hash, const std::string_view value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const auto character : value) {
        hash_byte(hash, static_cast<std::uint8_t>(character));
    }
}

void hash_bool(std::uint64_t& hash, const bool value) noexcept {
    hash_byte(hash, value ? 1U : 0U);
}

void hash_float(std::uint64_t& hash, const float value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
}

void hash_float_array(std::uint64_t& hash, const std::array<float, 3U>& value) noexcept {
    for (const auto component : value) hash_float(hash, component);
}

void hash_matrix(std::uint64_t& hash, const std::array<float, 16U>& value) noexcept {
    for (const auto component : value) hash_float(hash, component);
}

bool finite_position(const std::array<float, 3U>& position) noexcept {
    return std::all_of(position.begin(), position.end(), [](const float value) {
        return std::isfinite(value);
    });
}

bool finite_transform(const std::array<float, 16U>& transform) noexcept {
    return std::all_of(transform.begin(), transform.end(), [](const float value) {
        return std::isfinite(value);
    });
}

std::array<float, 3U> transform_position(const std::array<float, 16U>& matrix,
                                         const std::array<float, 3U>& position) noexcept {
    return {
        matrix[0U] * position[0U] + matrix[4U] * position[1U] +
            matrix[8U] * position[2U] + matrix[12U],
        matrix[1U] * position[0U] + matrix[5U] * position[1U] +
            matrix[9U] * position[2U] + matrix[13U],
        matrix[2U] * position[0U] + matrix[6U] * position[1U] +
            matrix[10U] * position[2U] + matrix[14U],
    };
}

bool valid_geometry_source(const SceneRayTracingGeometrySourceKind source) noexcept {
    return source == SceneRayTracingGeometrySourceKind::builtin ||
        source == SceneRayTracingGeometrySourceKind::imported;
}

bool valid_alpha_mode(const SceneRayTracingAlphaMode mode) noexcept {
    return mode == SceneRayTracingAlphaMode::opaque ||
        mode == SceneRayTracingAlphaMode::mask || mode == SceneRayTracingAlphaMode::blend;
}

bool supported_for_as(const SceneRayTracingPrimitiveInput& primitive) noexcept {
    return !primitive.skinned && primitive.alpha_mode == SceneRayTracingAlphaMode::opaque;
}

SceneRayTracingGeometryCacheLimits normalize_limits(
    SceneRayTracingGeometryCacheLimits limits) noexcept {
    limits.max_triangles = std::min(
        limits.max_triangles, scene_raytracing_geometry_cache_hard_max_triangles);
    limits.max_geometries = std::min(
        limits.max_geometries, scene_raytracing_geometry_cache_hard_max_geometries);
    limits.max_primitives = std::min(
        limits.max_primitives, scene_raytracing_geometry_cache_hard_max_primitives);
    limits.max_instances = std::min(
        limits.max_instances, scene_raytracing_geometry_cache_hard_max_instances);
    limits.max_vertices_per_geometry = std::min(
        limits.max_vertices_per_geometry,
        scene_raytracing_geometry_cache_hard_max_vertices_per_geometry);
    limits.max_indices_per_geometry = std::min(
        limits.max_indices_per_geometry,
        scene_raytracing_geometry_cache_hard_max_indices_per_geometry);
    return limits;
}

std::uint32_t bounded_count(const std::size_t value) noexcept {
    return static_cast<std::uint32_t>(value);
}

struct ValidationResult final {
    bool valid{};
    std::string code;
    std::string detail;
    std::vector<const SceneRayTracingGeometryInput*> geometries;
    std::vector<const SceneRayTracingInstanceInput*> instances;
    std::uint64_t topology_fingerprint{};
    std::uint64_t content_fingerprint{};
    SceneRayTracingGeometryCacheStatistics statistics;
};

ValidationResult invalid_result(std::string_view code,
                                std::string_view detail) {
    ValidationResult result;
    result.code = bounded_text(code);
    result.detail = bounded_text(detail);
    return result;
}

ValidationResult validate_input(const SceneRayTracingGeometryCacheInput& input,
                                const SceneRayTracingGeometryCacheLimits& limits) {
    if (input.scene_id.empty()) {
        return invalid_result("scene-empty-id", "The source scene requires a stable scene_id.");
    }
    if (input.scene_id.size() > scene_raytracing_geometry_cache_max_text_bytes) {
        return invalid_result("scene-id-too-long", "The source scene id exceeds the bounded text limit.");
    }
    if (input.geometries.size() > limits.max_geometries) {
        return invalid_result("geometry-budget", "The source geometry count exceeds the cache budget.");
    }
    if (input.instances.size() > limits.max_instances) {
        return invalid_result("instance-budget", "The source instance count exceeds the cache budget.");
    }

    ValidationResult result;
    result.valid = true;
    result.statistics.input_geometry_count = bounded_count(input.geometries.size());
    result.statistics.input_instance_count = bounded_count(input.instances.size());
    result.geometries.reserve(input.geometries.size());
    result.instances.reserve(input.instances.size());

    for (const auto& geometry : input.geometries) {
        if (geometry.geometry_id.empty()) {
            return invalid_result("geometry-empty-id", "Every geometry requires a stable geometry_id.");
        }
        if (geometry.geometry_id.size() > scene_raytracing_geometry_cache_max_text_bytes) {
            return invalid_result("geometry-id-too-long", "A geometry id exceeds the bounded text limit.");
        }
        if (!valid_geometry_source(geometry.source)) {
            return invalid_result("geometry-source-invalid", "The geometry source kind is not supported.");
        }
        if (geometry.positions.size() > limits.max_vertices_per_geometry) {
            return invalid_result("vertex-budget", "A geometry exceeds the vertex cache budget.");
        }
        if (geometry.indices.size() > limits.max_indices_per_geometry) {
            return invalid_result("index-budget", "A geometry exceeds the index cache budget.");
        }
        if (geometry.primitives.empty()) {
            return invalid_result("primitive-empty", "Every enabled or disabled geometry must declare primitive ranges.");
        }
        if (geometry.primitives.size() > limits.max_primitives) {
            return invalid_result("primitive-budget", "A geometry exceeds the primitive cache budget.");
        }
        if (result.statistics.input_primitive_count >
            std::numeric_limits<std::uint32_t>::max() - bounded_count(geometry.primitives.size())) {
            return invalid_result("primitive-count-overflow", "The aggregate primitive count exceeds the bounded contract.");
        }
        result.statistics.input_primitive_count += bounded_count(geometry.primitives.size());
        if (result.statistics.input_primitive_count > limits.max_primitives) {
            return invalid_result("primitive-budget", "The aggregate primitive count exceeds the cache budget.");
        }
        if (!geometry.enabled) ++result.statistics.excluded_disabled_geometry_count;
        result.statistics.source_position_bytes +=
            static_cast<std::uint64_t>(geometry.positions.size()) * 3ULL * sizeof(float);
        result.statistics.source_index_bytes +=
            static_cast<std::uint64_t>(geometry.indices.size()) * sizeof(std::uint32_t);

        for (const auto& position : geometry.positions) {
            if (!finite_position(position)) {
                return invalid_result("geometry-nonfinite-position", "A geometry position contains a non-finite value.");
            }
        }

        std::vector<const SceneRayTracingPrimitiveInput*> primitives;
        primitives.reserve(geometry.primitives.size());
        for (const auto& primitive : geometry.primitives) {
            if (primitive.primitive_id.empty()) {
                return invalid_result("primitive-empty-id", "Every primitive requires a stable primitive_id.");
            }
            if (primitive.primitive_id.size() > scene_raytracing_geometry_cache_max_text_bytes) {
                return invalid_result("primitive-id-too-long", "A primitive id exceeds the bounded text limit.");
            }
            if (!valid_alpha_mode(primitive.alpha_mode)) {
                return invalid_result("primitive-alpha-invalid", "The primitive alpha mode is not supported.");
            }
            if (primitive.index_count == 0U || primitive.index_count % 3U != 0U) {
                return invalid_result("primitive-range-not-triangles", "Primitive ranges must contain a non-zero multiple of three indices.");
            }
            const auto first_index = static_cast<std::size_t>(primitive.first_index);
            const auto index_count = static_cast<std::size_t>(primitive.index_count);
            if (first_index > geometry.indices.size() || index_count > geometry.indices.size() - first_index) {
                return invalid_result("primitive-range-out-of-bounds", "A primitive range exceeds its geometry index buffer.");
            }
            for (std::size_t index = first_index; index < first_index + index_count; ++index) {
                if (static_cast<std::size_t>(geometry.indices[index]) >= geometry.positions.size()) {
                    return invalid_result("index-out-of-bounds", "A primitive index does not reference a geometry position.");
                }
            }
            if (!primitive.enabled) ++result.statistics.excluded_disabled_primitive_count;
            primitives.push_back(&primitive);
        }
        std::sort(primitives.begin(), primitives.end(), [](const auto* left, const auto* right) {
            return left->primitive_id < right->primitive_id;
        });
        for (std::size_t index = 1U; index < primitives.size(); ++index) {
            if (primitives[index - 1U]->primitive_id == primitives[index]->primitive_id) {
                return invalid_result("primitive-duplicate-id", "Primitive ids must be unique within a geometry.");
            }
        }
        result.geometries.push_back(&geometry);
    }

    std::sort(result.geometries.begin(), result.geometries.end(), [](const auto* left, const auto* right) {
        return left->geometry_id < right->geometry_id;
    });
    for (std::size_t index = 1U; index < result.geometries.size(); ++index) {
        if (result.geometries[index - 1U]->geometry_id == result.geometries[index]->geometry_id) {
            return invalid_result("geometry-duplicate-id", "Geometry ids must be unique in a scene.");
        }
    }

    for (const auto& instance : input.instances) {
        if (instance.instance_id.empty()) {
            return invalid_result("instance-empty-id", "Every instance requires a stable instance_id.");
        }
        if (instance.instance_id.size() > scene_raytracing_geometry_cache_max_text_bytes) {
            return invalid_result("instance-id-too-long", "An instance id exceeds the bounded text limit.");
        }
        if (instance.geometry_id.empty()) {
            return invalid_result("instance-empty-geometry", "Every instance requires a geometry_id reference.");
        }
        if (instance.geometry_id.size() > scene_raytracing_geometry_cache_max_text_bytes) {
            return invalid_result("instance-geometry-id-too-long", "An instance geometry id exceeds the bounded text limit.");
        }
        if (!finite_transform(instance.transform)) {
            return invalid_result("instance-nonfinite-transform", "An instance transform contains a non-finite value.");
        }
        const auto geometry = std::lower_bound(
            result.geometries.begin(), result.geometries.end(), instance.geometry_id,
            [](const auto* candidate, const std::string& id) { return candidate->geometry_id < id; });
        if (geometry == result.geometries.end() || (*geometry)->geometry_id != instance.geometry_id) {
            return invalid_result("instance-geometry-missing", "An instance references an unknown geometry_id.");
        }
        if (!instance.enabled) ++result.statistics.excluded_disabled_instance_count;
        result.instances.push_back(&instance);
    }
    std::sort(result.instances.begin(), result.instances.end(), [](const auto* left, const auto* right) {
        return left->instance_id < right->instance_id;
    });
    for (std::size_t index = 1U; index < result.instances.size(); ++index) {
        if (result.instances[index - 1U]->instance_id == result.instances[index]->instance_id) {
            return invalid_result("instance-duplicate-id", "Instance ids must be unique in a scene.");
        }
    }

    std::uint64_t topology = fnv_offset_basis;
    std::uint64_t content = fnv_offset_basis;
    hash_string(topology, input.scene_id);
    hash_string(content, input.scene_id);
    hash_u64(topology, static_cast<std::uint64_t>(result.geometries.size()));
    hash_u64(content, static_cast<std::uint64_t>(result.geometries.size()));
    for (const auto* geometry : result.geometries) {
        hash_string(topology, geometry->geometry_id);
        hash_byte(topology, static_cast<std::uint8_t>(geometry->source));
        hash_bool(topology, geometry->enabled);
        hash_u64(topology, static_cast<std::uint64_t>(geometry->positions.size()));
        hash_u64(topology, static_cast<std::uint64_t>(geometry->indices.size()));
        hash_u64(topology, static_cast<std::uint64_t>(geometry->primitives.size()));
        hash_string(content, geometry->geometry_id);
        hash_byte(content, static_cast<std::uint8_t>(geometry->source));
        hash_bool(content, geometry->enabled);
        hash_u64(content, static_cast<std::uint64_t>(geometry->positions.size()));
        hash_u64(content, static_cast<std::uint64_t>(geometry->indices.size()));
        hash_u64(content, static_cast<std::uint64_t>(geometry->primitives.size()));
        for (const auto& position : geometry->positions) hash_float_array(content, position);
        for (const auto index : geometry->indices) {
            hash_u64(topology, index);
            hash_u64(content, index);
        }

        std::vector<const SceneRayTracingPrimitiveInput*> primitives;
        primitives.reserve(geometry->primitives.size());
        for (const auto& primitive : geometry->primitives) primitives.push_back(&primitive);
        std::sort(primitives.begin(), primitives.end(), [](const auto* left, const auto* right) {
            return left->primitive_id < right->primitive_id;
        });
        for (const auto* primitive : primitives) {
            hash_string(topology, primitive->primitive_id);
            hash_u64(topology, primitive->first_index);
            hash_u64(topology, primitive->index_count);
            hash_bool(topology, primitive->enabled);
            hash_byte(topology, static_cast<std::uint8_t>(primitive->alpha_mode));
            hash_bool(topology, primitive->skinned);
            hash_string(content, primitive->primitive_id);
            hash_u64(content, primitive->first_index);
            hash_u64(content, primitive->index_count);
            hash_bool(content, primitive->enabled);
            hash_byte(content, static_cast<std::uint8_t>(primitive->alpha_mode));
            hash_bool(content, primitive->skinned);
        }
    }
    hash_u64(topology, static_cast<std::uint64_t>(result.instances.size()));
    hash_u64(content, static_cast<std::uint64_t>(result.instances.size()));
    for (const auto* instance : result.instances) {
        hash_string(topology, instance->instance_id);
        hash_string(topology, instance->geometry_id);
        hash_bool(topology, instance->enabled);
        hash_string(content, instance->instance_id);
        hash_string(content, instance->geometry_id);
        hash_bool(content, instance->enabled);
        hash_matrix(content, instance->transform);
    }
    hash_bool(content, input.allow_update);
    // Keep the content identity distinct when an index/range topology change
    // occurs, while still allowing transform/position-only updates to retain
    // the same topology fingerprint.
    hash_u64(content, topology);
    result.topology_fingerprint = topology;
    result.content_fingerprint = content;
    return result;
}

struct BuildResult final {
    bool valid{};
    std::string code;
    std::string detail;
    SceneRayTracingGeometryCacheSnapshot snapshot;
};

void append_diagnostic(SceneRayTracingGeometryCacheSnapshot& snapshot,
                       std::string_view code,
                       std::string_view path,
                       std::string_view message) {
    if (snapshot.diagnostics.size() >= scene_raytracing_geometry_cache_max_diagnostics) return;
    snapshot.diagnostics.push_back(SceneRayTracingGeometryCacheDiagnostic{
        bounded_text(code), bounded_text(path), bounded_text(message)});
}

const SceneRayTracingGeometryInput* find_geometry(
    const std::vector<const SceneRayTracingGeometryInput*>& geometries,
    const std::string& id) noexcept {
    const auto found = std::lower_bound(
        geometries.begin(), geometries.end(), id,
        [](const auto* candidate, const std::string& value) {
            return candidate->geometry_id < value;
        });
    return found == geometries.end() || (*found)->geometry_id != id ? nullptr : *found;
}

BuildResult build_snapshot(const SceneRayTracingGeometryCacheInput& input,
                           const ValidationResult& validation,
                           const SceneRayTracingGeometryCacheLimits& limits) {
    BuildResult result;
    result.valid = true;
    result.snapshot.schema = std::string(scene_raytracing_geometry_cache_schema);
    result.snapshot.scene_id = input.scene_id;
    result.snapshot.allow_update = input.allow_update;
    result.snapshot.topology_fingerprint = validation.topology_fingerprint;
    result.snapshot.content_fingerprint = validation.content_fingerprint;
    result.snapshot.statistics = validation.statistics;
    result.snapshot.world_triangles.reserve(std::min<std::size_t>(limits.max_triangles, 256U));

    std::vector<std::string> unsupported_geometry_ids;
    for (const auto* instance : validation.instances) {
        if (!instance->enabled) continue;
        const auto* geometry = find_geometry(validation.geometries, instance->geometry_id);
        if (geometry == nullptr || !geometry->enabled) continue;

        std::vector<const SceneRayTracingPrimitiveInput*> primitives;
        primitives.reserve(geometry->primitives.size());
        for (const auto& primitive : geometry->primitives) primitives.push_back(&primitive);
        std::sort(primitives.begin(), primitives.end(), [](const auto* left, const auto* right) {
            return left->primitive_id < right->primitive_id;
        });

        const auto instance_first_triangle = result.snapshot.statistics.world_triangle_count;
        for (const auto* primitive : primitives) {
            if (!primitive->enabled) continue;
            if (!supported_for_as(*primitive)) {
                result.snapshot.statistics.excluded_unsupported_primitive_count =
                    result.snapshot.statistics.excluded_unsupported_primitive_count >=
                        std::numeric_limits<std::uint32_t>::max()
                    ? result.snapshot.statistics.excluded_unsupported_primitive_count
                    : result.snapshot.statistics.excluded_unsupported_primitive_count + 1U;
                result.snapshot.fallback_active = true;
                if (std::find(unsupported_geometry_ids.begin(), unsupported_geometry_ids.end(),
                              geometry->geometry_id) == unsupported_geometry_ids.end()) {
                    unsupported_geometry_ids.push_back(geometry->geometry_id);
                }
                continue;
            }

            const auto triangle_count = primitive->index_count / 3U;
            if (static_cast<std::size_t>(result.snapshot.statistics.world_triangle_count) +
                    static_cast<std::size_t>(triangle_count) > limits.max_triangles) {
                result.valid = false;
                result.code = "triangle-budget";
                result.detail = "The accepted world-space triangle count exceeds 65536 or the configured cache budget.";
                return result;
            }
            const auto range_first_triangle = result.snapshot.statistics.world_triangle_count;
            for (std::uint32_t triangle = 0U; triangle < triangle_count; ++triangle) {
                const auto source_index = primitive->first_index + triangle * 3U;
                SceneRayTracingGeometryCacheWorldTriangle world_triangle;
                world_triangle.instance_id = instance->instance_id;
                world_triangle.geometry_id = geometry->geometry_id;
                world_triangle.primitive_id = primitive->primitive_id;
                world_triangle.source_triangle_index = source_index / 3U;
                for (std::uint32_t vertex = 0U; vertex < 3U; ++vertex) {
                    const auto source_vertex = geometry->indices[source_index + vertex];
                    world_triangle.positions[vertex] =
                        transform_position(instance->transform, geometry->positions[source_vertex]);
                    if (!finite_position(world_triangle.positions[vertex])) {
                        result.valid = false;
                        result.code = "world-position-nonfinite";
                        result.detail = "An instance transform overflowed a world-space triangle position.";
                        return result;
                    }
                }
                result.snapshot.world_triangles.push_back(std::move(world_triangle));
                ++result.snapshot.statistics.world_triangle_count;
            }
            result.snapshot.primitive_ranges.push_back(SceneRayTracingGeometryCachePrimitiveRange{
                instance->instance_id,
                geometry->geometry_id,
                primitive->primitive_id,
                range_first_triangle,
                triangle_count,
                primitive->first_index,
                primitive->index_count,
            });
            ++result.snapshot.statistics.accepted_primitive_count;
        }

        const auto instance_triangle_count =
            result.snapshot.statistics.world_triangle_count - instance_first_triangle;
        if (instance_triangle_count != 0U) {
            result.snapshot.world_instances.push_back(SceneRayTracingGeometryCacheWorldInstance{
                instance->instance_id,
                geometry->geometry_id,
                instance->transform,
                instance_first_triangle,
                instance_triangle_count,
            });
            ++result.snapshot.statistics.accepted_instance_count;
            ++result.snapshot.statistics.accepted_geometry_count;
        }
    }

    // A geometry can be instantiated more than once.  accepted_geometry_count
    // above is therefore corrected to count unique stable geometry ids without
    // changing the deterministic instance order used by the triangle ranges.
    std::vector<std::string> accepted_geometry_ids;
    accepted_geometry_ids.reserve(result.snapshot.world_instances.size());
    for (const auto& instance : result.snapshot.world_instances) {
        accepted_geometry_ids.push_back(instance.geometry_id);
    }
    std::sort(accepted_geometry_ids.begin(), accepted_geometry_ids.end());
    accepted_geometry_ids.erase(
        std::unique(accepted_geometry_ids.begin(), accepted_geometry_ids.end()),
        accepted_geometry_ids.end());
    result.snapshot.statistics.accepted_geometry_count = 0U;
    result.snapshot.statistics.accepted_geometry_count = bounded_count(accepted_geometry_ids.size());

    result.snapshot.statistics.world_vertex_count =
        result.snapshot.statistics.world_triangle_count * 3U;
    result.snapshot.statistics.world_index_count =
        result.snapshot.statistics.world_triangle_count * 3U;
    result.snapshot.statistics.world_position_bytes =
        static_cast<std::uint64_t>(result.snapshot.statistics.world_vertex_count) *
        3ULL * sizeof(float);
    result.snapshot.statistics.world_index_bytes =
        static_cast<std::uint64_t>(result.snapshot.statistics.world_index_count) *
        sizeof(std::uint32_t);
    result.snapshot.statistics.world_triangle_bytes =
        static_cast<std::uint64_t>(result.snapshot.statistics.world_triangle_count) *
        sizeof(std::array<std::array<float, 3U>, 3U>);

    if (!unsupported_geometry_ids.empty()) {
        for (const auto& geometry_id : unsupported_geometry_ids) {
            append_diagnostic(result.snapshot, "primitive-excluded-unsupported",
                              geometry_id,
                              "Skinned, BLEND and MASK primitives are excluded from the AS-only cache.");
        }
    }
    if (result.snapshot.statistics.world_triangle_count == 0U) {
        result.snapshot.fallback_active = true;
        result.snapshot.fallback = SceneRayTracingGeometryCacheFallback{
            true,
            "no-supported-triangles",
            "No enabled opaque static triangle remained for the ray-tracing scene; use the explicit raster path.",
        };
        result.snapshot.state = SceneRayTracingGeometryCacheState::fallback;
    } else if (result.snapshot.fallback_active) {
        result.snapshot.fallback = SceneRayTracingGeometryCacheFallback{
            true,
            "partial-scene-fallback",
            "Unsupported or excluded primitives remain outside the AS-only triangle scene.",
        };
        result.snapshot.state = SceneRayTracingGeometryCacheState::ready;
    } else {
        result.snapshot.fallback = SceneRayTracingGeometryCacheFallback{};
        result.snapshot.state = SceneRayTracingGeometryCacheState::ready;
    }
    result.snapshot.accepted = true;
    return result;
}

SceneRayTracingGeometryCacheUpdate update_from_snapshot(
    const SceneRayTracingGeometryCacheSnapshot& snapshot,
    std::string_view code,
    std::string_view detail) {
    SceneRayTracingGeometryCacheUpdate result;
    result.schema = snapshot.schema;
    result.state = snapshot.state;
    result.code = bounded_text(code);
    result.detail = bounded_text(detail);
    result.accepted = snapshot.accepted;
    result.fallback_active = snapshot.fallback_active;
    result.topology_revision = snapshot.topology_revision;
    result.content_revision = snapshot.content_revision;
    result.topology_fingerprint = snapshot.topology_fingerprint;
    result.content_fingerprint = snapshot.content_fingerprint;
    result.statistics = snapshot.statistics;
    result.fallback = snapshot.fallback;
    return result;
}

} // namespace

std::string_view scene_raytracing_geometry_source_name(
    const SceneRayTracingGeometrySourceKind source) noexcept {
    switch (source) {
    case SceneRayTracingGeometrySourceKind::builtin:
        return "builtin";
    case SceneRayTracingGeometrySourceKind::imported:
        return "imported";
    }
    return "unknown";
}

std::string_view scene_raytracing_alpha_mode_name(
    const SceneRayTracingAlphaMode mode) noexcept {
    switch (mode) {
    case SceneRayTracingAlphaMode::opaque:
        return "opaque";
    case SceneRayTracingAlphaMode::mask:
        return "mask";
    case SceneRayTracingAlphaMode::blend:
        return "blend";
    }
    return "unknown";
}

std::string_view scene_raytracing_geometry_cache_state_name(
    const SceneRayTracingGeometryCacheState state) noexcept {
    switch (state) {
    case SceneRayTracingGeometryCacheState::empty:
        return "empty";
    case SceneRayTracingGeometryCacheState::ready:
        return "ready";
    case SceneRayTracingGeometryCacheState::fallback:
        return "fallback";
    case SceneRayTracingGeometryCacheState::rejected:
        return "rejected";
    }
    return "unknown";
}

struct SceneRayTracingGeometryCache::Impl final {
    explicit Impl(const SceneRayTracingGeometryCacheLimits input_limits)
        : limits(normalize_limits(input_limits)) {}

    SceneRayTracingGeometryCacheLimits limits;
    SceneRayTracingGeometryCacheSnapshot snapshot;
    bool has_snapshot{};
};

SceneRayTracingGeometryCache::SceneRayTracingGeometryCache(
    const SceneRayTracingGeometryCacheLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {}

SceneRayTracingGeometryCache::~SceneRayTracingGeometryCache() = default;

SceneRayTracingGeometryCacheUpdate SceneRayTracingGeometryCache::update(
    const SceneRayTracingGeometryCacheInput& input) {
    const auto validation = validate_input(input, impl_->limits);
    if (!validation.valid) {
        auto result = update_from_snapshot(
            impl_->snapshot, validation.code, validation.detail);
        result.state = SceneRayTracingGeometryCacheState::rejected;
        result.accepted = false;
        result.reused = false;
        result.content_changed = false;
        result.topology_changed = false;
        result.fallback_active = true;
        result.fallback = SceneRayTracingGeometryCacheFallback{
            true, "input-rejected", validation.detail};
        return result;
    }

    auto built = build_snapshot(input, validation, impl_->limits);
    if (!built.valid) {
        auto result = update_from_snapshot(impl_->snapshot, built.code, built.detail);
        result.state = SceneRayTracingGeometryCacheState::rejected;
        result.accepted = false;
        result.reused = false;
        result.content_changed = false;
        result.topology_changed = false;
        result.fallback_active = true;
        result.fallback = SceneRayTracingGeometryCacheFallback{
            true, built.code, built.detail};
        return result;
    }

    const bool topology_changed = !impl_->has_snapshot ||
        validation.topology_fingerprint != impl_->snapshot.topology_fingerprint;
    const bool content_changed = !impl_->has_snapshot ||
        validation.content_fingerprint != impl_->snapshot.content_fingerprint;
    if (impl_->has_snapshot && !topology_changed && !content_changed) {
        auto result = update_from_snapshot(
            impl_->snapshot, "cache-reused", "The stable geometry and instance input is unchanged.");
        result.reused = true;
        result.content_changed = false;
        result.topology_changed = false;
        return result;
    }

    built.snapshot.topology_revision = impl_->has_snapshot
        ? impl_->snapshot.topology_revision + (topology_changed ? 1U : 0U)
        : 1U;
    built.snapshot.content_revision = impl_->has_snapshot
        ? impl_->snapshot.content_revision + (content_changed ? 1U : 0U)
        : 1U;
    impl_->snapshot = std::move(built.snapshot);
    impl_->has_snapshot = true;

    const auto code = topology_changed ? "cache-topology-rebuilt"
        : (content_changed ? "cache-content-updated" : "cache-built");
    const auto detail = topology_changed
        ? "Geometry, primitive or instance topology changed; the AS transfer scene was rebuilt."
        : "Only geometry content, instance transforms or update policy changed; stable topology was retained.";
    auto result = update_from_snapshot(impl_->snapshot, code, detail);
    result.reused = false;
    result.content_changed = content_changed;
    result.topology_changed = topology_changed;
    return result;
}

const SceneRayTracingGeometryCacheSnapshot& SceneRayTracingGeometryCache::snapshot() const noexcept {
    return impl_->snapshot;
}

SceneRayTracingGeometryCacheUpdate SceneRayTracingGeometryCache::status() const {
    if (!impl_->has_snapshot) {
        SceneRayTracingGeometryCacheUpdate result;
        result.state = SceneRayTracingGeometryCacheState::empty;
        result.code = "cache-empty";
        result.detail = "No accepted source scene has been submitted.";
        return result;
    }
    return update_from_snapshot(impl_->snapshot, "cache-status", "Current derived world-space cache status.");
}

const SceneRayTracingGeometryCacheLimits& SceneRayTracingGeometryCache::limits() const noexcept {
    return impl_->limits;
}

void SceneRayTracingGeometryCache::clear() noexcept {
    impl_->snapshot = SceneRayTracingGeometryCacheSnapshot{};
    impl_->has_snapshot = false;
}

RayTracingContextSessionScene to_raytracing_context_session_scene(
    const SceneRayTracingGeometryCacheSnapshot& snapshot) {
    RayTracingContextSessionScene result;
    result.scene_id = snapshot.scene_id;
    result.topology_revision = snapshot.topology_revision;
    result.content_revision = snapshot.content_revision;
    result.allow_update = snapshot.allow_update;
    result.triangles.reserve(snapshot.world_triangles.size());
    for (const auto& world_triangle : snapshot.world_triangles) {
        result.triangles.push_back(RayTracingContextSessionTriangle{
            world_triangle.positions});
    }
    return result;
}

} // namespace noemancer
