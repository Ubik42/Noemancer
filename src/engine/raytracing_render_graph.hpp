#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Logical, renderer-neutral contract for a persistent ray-tracing graph.  The
// Runtime may translate these records to D3D12 or Vulkan objects, but no
// device, resource, descriptor, allocator or command-buffer handle crosses
// this boundary.
inline constexpr std::string_view raytracing_render_graph_schema =
    "noemancer.raytracing-render-graph/0.1";
inline constexpr std::size_t raytracing_render_graph_max_resources = 128U;
inline constexpr std::size_t raytracing_render_graph_max_passes = 128U;
inline constexpr std::size_t raytracing_render_graph_max_diagnostics = 64U;
inline constexpr std::size_t raytracing_render_graph_max_text_bytes = 512U;
inline constexpr std::uint32_t raytracing_render_graph_max_extent = 16384U;
inline constexpr std::uint32_t raytracing_render_graph_max_layers = 2048U;
inline constexpr std::uint32_t raytracing_render_graph_max_history_length = 8U;
inline constexpr std::uint64_t raytracing_render_graph_max_resource_bytes = 1ULL << 40U;

enum class RayTracingRenderGraphResourceKind : std::uint8_t {
    blas = 0U,
    tlas = 1U,
    sbt = 2U,
    output = 3U,
    history = 4U,

    Blas = blas,
    Tlas = tlas,
    Sbt = sbt,
    Output = output,
    History = history,
};

enum class RayTracingRenderGraphResourceLifetime : std::uint8_t {
    persistent = 0U,
    transient = 1U,
    history = 2U,

    Persistent = persistent,
    Transient = transient,
    History = history,
};

enum class RayTracingRenderGraphPassKind : std::uint8_t {
    build_blas = 0U,
    update_blas = 1U,
    refit_blas = 2U,
    build_tlas = 3U,
    update_tlas = 4U,
    refit_tlas = 5U,
    build_sbt = 6U,
    trace = 7U,
    denoise = 8U,
    resolve = 9U,
    clear_history = 10U,
    raster_fallback = 11U,

    BuildBlas = build_blas,
    UpdateBlas = update_blas,
    RefitBlas = refit_blas,
    BuildTlas = build_tlas,
    UpdateTlas = update_tlas,
    RefitTlas = refit_tlas,
    BuildSbt = build_sbt,
    Trace = trace,
    Denoise = denoise,
    Resolve = resolve,
    ClearHistory = clear_history,
    RasterFallback = raster_fallback,
};

enum class RayTracingRenderGraphBuildDecision : std::uint8_t {
    none = 0U,
    build = 1U,
    update = 2U,
    refit = 3U,
    rebuild = 4U,
    clear = 5U,
    unsupported = 6U,

    None = none,
    Build = build,
    Update = update,
    Refit = refit,
    Rebuild = rebuild,
    Clear = clear,
    Unsupported = unsupported,
};

enum class RayTracingRenderGraphMode : std::uint8_t {
    ray_tracing = 0U,
    raster_fallback = 1U,
    unsupported = 2U,
    error = 3U,

    RayTracing = ray_tracing,
    RasterFallback = raster_fallback,
    Unsupported = unsupported,
    Error = error,
};

[[nodiscard]] std::string_view raytracing_render_graph_resource_kind_name(
    RayTracingRenderGraphResourceKind kind) noexcept;
[[nodiscard]] std::string_view raytracing_render_graph_resource_lifetime_name(
    RayTracingRenderGraphResourceLifetime lifetime) noexcept;
[[nodiscard]] std::string_view raytracing_render_graph_pass_kind_name(
    RayTracingRenderGraphPassKind kind) noexcept;
[[nodiscard]] std::string_view raytracing_render_graph_build_decision_name(
    RayTracingRenderGraphBuildDecision decision) noexcept;
[[nodiscard]] std::string_view raytracing_render_graph_mode_name(
    RayTracingRenderGraphMode mode) noexcept;

struct RayTracingRenderGraphResource final {
    // Stable logical identity.  This is not a native resource name or handle.
    std::string id;
    RayTracingRenderGraphResourceKind kind{
        RayTracingRenderGraphResourceKind::output};
    RayTracingRenderGraphResourceLifetime lifetime{
        RayTracingRenderGraphResourceLifetime::transient};
    std::string format{"unknown"};
    std::string dimension{"2d"};
    std::uint32_t width{1U};
    std::uint32_t height{1U};
    std::uint32_t depth{1U};
    std::uint32_t layers{1U};
    // Resident result bytes for one logical resource instance.  History
    // resources multiply this by history_length in the budget report.
    std::uint64_t bytes{};
    std::uint64_t scratch_bytes{};
    std::uint32_t history_length{1U};
    // Generation is the content/topology identity observed by this frame.
    // It is monotonic for a stable logical ID and is never a pointer value.
    std::uint64_t generation{1U};
    bool dirty{};
    bool topology_changed{};
    bool refit_requested{};
};

struct RayTracingRenderGraphPass final {
    std::string id;
    RayTracingRenderGraphPassKind kind{
        RayTracingRenderGraphPassKind::trace};
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<std::string> depends_on;
    // A pass may read and write the same logical resource only when this is
    // explicit.  This prevents accidental history/output aliasing in a plan.
    bool read_modify_write{};
    bool enabled{true};
};

struct RayTracingRenderGraphBudget final {
    std::uint64_t max_resident_bytes{1ULL << 30U};
    std::uint64_t max_scratch_bytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t max_history_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t max_output_bytes{256ULL * 1024ULL * 1024ULL};
};

struct RayTracingRenderGraphPolicy final {
    bool enabled{true};
    bool allow_raster_fallback{true};
    bool require_history{true};
};

// These are capability facts, not adapter objects.  A backend adapter fills
// them only after probing its device; the graph contract can then fail closed
// or select its explicit raster path without knowing the API in use.
struct RayTracingRenderGraphCapabilities final {
    bool device_supported{true};
    bool acceleration_structure_supported{true};
    bool ray_tracing_pipeline_supported{true};
    bool update_supported{true};
    bool refit_supported{true};
};

struct RayTracingRenderGraphPreviousResource final {
    std::string id;
    RayTracingRenderGraphResourceKind kind{
        RayTracingRenderGraphResourceKind::output};
    std::uint64_t generation{};
    bool ready{};
};

struct RayTracingRenderGraphFrameState final {
    // frame_generation identifies the frame boundary; graph_generation
    // identifies the graph topology/pipeline version across frames.
    std::uint64_t frame_generation{1U};
    std::uint64_t graph_generation{1U};
    std::uint64_t previous_graph_generation{1U};
    bool previous_frame_valid{};
    bool camera_cut{};
    bool extent_changed{};
    bool history_valid{};
    std::vector<RayTracingRenderGraphPreviousResource> previous_resources;
};

struct RayTracingRenderGraphDescription final {
    std::string schema{std::string(raytracing_render_graph_schema)};
    std::string graph_id{"render.graph.raytracing"};
    std::uint64_t graph_generation{1U};
    RayTracingRenderGraphPolicy policy;
    RayTracingRenderGraphBudget budget;
    RayTracingRenderGraphCapabilities capabilities;
    std::vector<RayTracingRenderGraphResource> resources;
    std::vector<RayTracingRenderGraphPass> passes;
};

struct RayTracingRenderGraphDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct RayTracingRenderGraphBudgetReport final {
    std::uint64_t required_resident_bytes{};
    std::uint64_t required_scratch_bytes{};
    std::uint64_t required_history_bytes{};
    std::uint64_t required_output_bytes{};
    std::uint64_t available_resident_bytes{};
    std::uint64_t available_scratch_bytes{};
    std::uint64_t available_history_bytes{};
    std::uint64_t available_output_bytes{};
    bool fits{};
};

struct RayTracingRenderGraphFallback final {
    bool active{};
    std::string mode{"raster-pbr"};
    std::string reason{"none"};
};

struct RayTracingRenderGraphResourcePlan final {
    std::string id;
    RayTracingRenderGraphResourceKind kind{
        RayTracingRenderGraphResourceKind::output};
    RayTracingRenderGraphResourceLifetime lifetime{
        RayTracingRenderGraphResourceLifetime::transient};
    std::uint64_t generation{};
    std::uint64_t previous_generation{};
    RayTracingRenderGraphBuildDecision decision{
        RayTracingRenderGraphBuildDecision::none};
    bool preserve_history{};
    bool reset_history{};
};

struct RayTracingRenderGraphPassPlan final {
    std::string id;
    RayTracingRenderGraphPassKind kind{
        RayTracingRenderGraphPassKind::trace};
    std::size_t execution_index{};
    bool selected{};
    bool enabled{};
};

struct RayTracingRenderGraphPlan final {
    std::string schema{std::string(raytracing_render_graph_schema)};
    std::string graph_id;
    std::uint64_t frame_generation{};
    std::uint64_t graph_generation{};
    RayTracingRenderGraphMode mode{RayTracingRenderGraphMode::error};
    bool valid{};
    bool supported{};
    std::string code;
    std::string detail;
    RayTracingRenderGraphBudgetReport budget;
    RayTracingRenderGraphFallback fallback;
    std::vector<std::string> execution_order;
    std::vector<RayTracingRenderGraphResourcePlan> resources;
    std::vector<RayTracingRenderGraphPassPlan> passes;
    std::vector<RayTracingRenderGraphDiagnostic> diagnostics;
};

[[nodiscard]] std::vector<RayTracingRenderGraphDiagnostic>
validate_raytracing_render_graph(
    const RayTracingRenderGraphDescription& description);

// Build a renderer-neutral frame plan.  previous_resources are matched by
// stable ID; no native lifetime is inferred from a missing/changed ID.
[[nodiscard]] RayTracingRenderGraphPlan build_raytracing_render_graph_plan(
    const RayTracingRenderGraphDescription& description,
    const RayTracingRenderGraphFrameState& frame = {});

[[nodiscard]] std::string raytracing_render_graph_canonical_description(
    const RayTracingRenderGraphDescription& description);
[[nodiscard]] std::string raytracing_render_graph_canonical_evidence(
    const RayTracingRenderGraphPlan& plan);
[[nodiscard]] std::string raytracing_render_graph_observation_json(
    const RayTracingRenderGraphPlan& plan);
[[nodiscard]] std::string raytracing_render_graph_fingerprint(
    const RayTracingRenderGraphPlan& plan);

} // namespace noemancer
