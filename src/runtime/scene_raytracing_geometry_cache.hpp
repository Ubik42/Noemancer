#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// CPU-side, renderer-neutral preparation for an AS-only ray-tracing scene.
// The cache owns only a derived transfer snapshot; Scene/Render World remain
// the authority for source geometry and instances.  No API-specific handle or
// third-party type crosses this boundary.
inline constexpr std::string_view scene_raytracing_geometry_cache_schema =
    "noemancer.scene-raytracing-geometry-cache/0.1";
inline constexpr std::size_t scene_raytracing_geometry_cache_max_text_bytes = 256U;
inline constexpr std::size_t scene_raytracing_geometry_cache_hard_max_triangles = 65536U;
inline constexpr std::size_t scene_raytracing_geometry_cache_hard_max_geometries = 4096U;
inline constexpr std::size_t scene_raytracing_geometry_cache_hard_max_primitives = 16384U;
inline constexpr std::size_t scene_raytracing_geometry_cache_hard_max_instances = 16384U;
inline constexpr std::size_t scene_raytracing_geometry_cache_hard_max_vertices_per_geometry =
    1U << 20U;
inline constexpr std::size_t scene_raytracing_geometry_cache_hard_max_indices_per_geometry =
    3U << 20U;
inline constexpr std::size_t scene_raytracing_geometry_cache_max_diagnostics = 64U;

enum class SceneRayTracingGeometrySourceKind : std::uint8_t {
    builtin = 0U,
    imported = 1U,

    Builtin = builtin,
    Imported = imported,
};

enum class SceneRayTracingAlphaMode : std::uint8_t {
    opaque = 0U,
    mask = 1U,
    blend = 2U,

    Opaque = opaque,
    Mask = mask,
    Blend = blend,
};

enum class SceneRayTracingGeometryCacheState : std::uint8_t {
    empty = 0U,
    ready = 1U,
    fallback = 2U,
    rejected = 3U,

    Empty = empty,
    Ready = ready,
    Fallback = fallback,
    Rejected = rejected,
};

[[nodiscard]] std::string_view scene_raytracing_geometry_source_name(
    SceneRayTracingGeometrySourceKind source) noexcept;
[[nodiscard]] std::string_view scene_raytracing_alpha_mode_name(
    SceneRayTracingAlphaMode mode) noexcept;
[[nodiscard]] std::string_view scene_raytracing_geometry_cache_state_name(
    SceneRayTracingGeometryCacheState state) noexcept;

struct SceneRayTracingPrimitiveInput final {
    // Stable within geometry_id.  It is part of topology identity.
    std::string primitive_id;
    // Triangle-list range into the owning geometry's index array.
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    bool enabled{true};
    SceneRayTracingAlphaMode alpha_mode{SceneRayTracingAlphaMode::opaque};
    // Skinning is deliberately not evaluated by this first AS-only path.
    bool skinned{};
};

struct SceneRayTracingGeometryInput final {
    // Stable identity from the source Scene/Render World or an imported
    // cooked asset.  The source kind is metadata and never a native handle.
    std::string geometry_id;
    SceneRayTracingGeometrySourceKind source{SceneRayTracingGeometrySourceKind::builtin};
    std::vector<std::array<float, 3U>> positions;
    std::vector<std::uint32_t> indices;
    std::vector<SceneRayTracingPrimitiveInput> primitives;
    bool enabled{true};
};

struct SceneRayTracingInstanceInput final {
    // Stable instance identity, independent of array order.
    std::string instance_id;
    std::string geometry_id;
    // Column-major affine matrix, matching the engine's existing matrix
    // convention: x' = m0*x + m4*y + m8*z + m12.
    std::array<float, 16U> transform{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    bool enabled{true};
};

struct SceneRayTracingGeometryCacheInput final {
    std::string scene_id;
    std::vector<SceneRayTracingGeometryInput> geometries;
    std::vector<SceneRayTracingInstanceInput> instances;
    bool allow_update{true};
};

struct SceneRayTracingGeometryCacheLimits final {
    std::size_t max_triangles{scene_raytracing_geometry_cache_hard_max_triangles};
    std::size_t max_geometries{scene_raytracing_geometry_cache_hard_max_geometries};
    std::size_t max_primitives{scene_raytracing_geometry_cache_hard_max_primitives};
    std::size_t max_instances{scene_raytracing_geometry_cache_hard_max_instances};
    std::size_t max_vertices_per_geometry{
        scene_raytracing_geometry_cache_hard_max_vertices_per_geometry};
    std::size_t max_indices_per_geometry{
        scene_raytracing_geometry_cache_hard_max_indices_per_geometry};
};

struct SceneRayTracingGeometryCacheDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct SceneRayTracingGeometryCacheFallback final {
    bool active{};
    std::string code{"none"};
    std::string detail;
};

struct SceneRayTracingGeometryCacheStatistics final {
    std::uint32_t input_geometry_count{};
    std::uint32_t input_primitive_count{};
    std::uint32_t input_instance_count{};
    std::uint32_t accepted_geometry_count{};
    std::uint32_t accepted_primitive_count{};
    std::uint32_t accepted_instance_count{};
    std::uint32_t excluded_disabled_geometry_count{};
    std::uint32_t excluded_disabled_primitive_count{};
    std::uint32_t excluded_disabled_instance_count{};
    std::uint32_t excluded_unsupported_primitive_count{};
    std::uint32_t world_triangle_count{};
    std::uint32_t world_vertex_count{};
    std::uint32_t world_index_count{};
    std::uint64_t source_position_bytes{};
    std::uint64_t source_index_bytes{};
    std::uint64_t world_position_bytes{};
    std::uint64_t world_index_bytes{};
    std::uint64_t world_triangle_bytes{};
};

struct SceneRayTracingGeometryCacheWorldTriangle final {
    std::array<std::array<float, 3U>, 3U> positions{};
    std::string instance_id;
    std::string geometry_id;
    std::string primitive_id;
    std::uint32_t source_triangle_index{};
};

struct SceneRayTracingGeometryCachePrimitiveRange final {
    std::string instance_id;
    std::string geometry_id;
    std::string primitive_id;
    std::uint32_t first_triangle{};
    std::uint32_t triangle_count{};
    std::uint32_t source_first_index{};
    std::uint32_t source_index_count{};
};

struct SceneRayTracingGeometryCacheWorldInstance final {
    std::string instance_id;
    std::string geometry_id;
    std::array<float, 16U> transform{};
    std::uint32_t first_triangle{};
    std::uint32_t triangle_count{};
};

// Renderer-neutral grouping metadata for one accepted instance/primitive
// range.  The cache keeps world_triangles as the only position authority and
// records this range table instead of duplicating vertices.  geometry_id is a
// stable backend key generated from instance_id + primitive_id by the shared
// RayTracingContextSession helper; the source identity fields remain useful
// for material lookup and Agent-facing diagnostics.
struct SceneRayTracingGeometryCacheGroupedGeometry final {
    std::string geometry_id;
    std::string source_geometry_id;
    std::string instance_id;
    std::string primitive_id;
    std::uint32_t first_triangle{};
    std::uint32_t triangle_count{};
};

// Derived world-space snapshot.  It is intentionally shaped as a direct
// transfer to RayTracingContextSessionScene: scene_id, revisions, update
// policy and the position-only triangle list are all available here.  It is
// not persisted and is never a second Scene authority.
struct SceneRayTracingGeometryCacheSnapshot final {
    std::string schema{std::string(scene_raytracing_geometry_cache_schema)};
    SceneRayTracingGeometryCacheState state{SceneRayTracingGeometryCacheState::empty};
    std::string scene_id;
    std::uint64_t topology_revision{};
    std::uint64_t content_revision{};
    std::uint64_t topology_fingerprint{};
    std::uint64_t content_fingerprint{};
    bool allow_update{true};
    bool accepted{};
    bool fallback_active{};
    SceneRayTracingGeometryCacheStatistics statistics;
    SceneRayTracingGeometryCacheFallback fallback;
    std::vector<SceneRayTracingGeometryCacheWorldTriangle> world_triangles;
    std::vector<SceneRayTracingGeometryCachePrimitiveRange> primitive_ranges;
    std::vector<SceneRayTracingGeometryCacheWorldInstance> world_instances;
    // One entry per accepted AS primitive range.  Empty means no supported
    // opaque static range survived the cache build.
    std::vector<SceneRayTracingGeometryCacheGroupedGeometry> grouped_geometries;
    std::vector<SceneRayTracingGeometryCacheDiagnostic> diagnostics;
};

struct SceneRayTracingGeometryCacheUpdate final {
    std::string schema{std::string(scene_raytracing_geometry_cache_schema)};
    SceneRayTracingGeometryCacheState state{SceneRayTracingGeometryCacheState::empty};
    std::string code;
    std::string detail;
    bool accepted{};
    bool reused{};
    bool content_changed{};
    bool topology_changed{};
    bool fallback_active{};
    std::uint64_t topology_revision{};
    std::uint64_t content_revision{};
    std::uint64_t topology_fingerprint{};
    std::uint64_t content_fingerprint{};
    SceneRayTracingGeometryCacheStatistics statistics;
    SceneRayTracingGeometryCacheFallback fallback;
};

// Forward declaration keeps the cache header independent of the runtime
// session implementation.  The one-way adapter is defined in the .cpp.
struct RayTracingContextSessionScene;

[[nodiscard]] RayTracingContextSessionScene
to_raytracing_context_session_scene(
    const SceneRayTracingGeometryCacheSnapshot& snapshot);

class SceneRayTracingGeometryCache final {
public:
    explicit SceneRayTracingGeometryCache(
        SceneRayTracingGeometryCacheLimits limits = {});
    ~SceneRayTracingGeometryCache();

    SceneRayTracingGeometryCache(const SceneRayTracingGeometryCache&) = delete;
    SceneRayTracingGeometryCache& operator=(const SceneRayTracingGeometryCache&) = delete;

    // Validate and derive one deterministic world-space snapshot.  A failed
    // update is transactional: the last accepted snapshot remains intact.
    [[nodiscard]] SceneRayTracingGeometryCacheUpdate update(
        const SceneRayTracingGeometryCacheInput& input);

    [[nodiscard]] const SceneRayTracingGeometryCacheSnapshot& snapshot() const noexcept;
    [[nodiscard]] SceneRayTracingGeometryCacheUpdate status() const;
    [[nodiscard]] const SceneRayTracingGeometryCacheLimits& limits() const noexcept;

    // Clear only the derived cache.  The caller remains responsible for the
    // authoritative Scene/Render World input.
    void clear() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace noemancer
