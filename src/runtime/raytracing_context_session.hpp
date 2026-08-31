#pragma once

#include "engine/native_raytracing_shading.hpp"
#include "engine/native_raytracing_view.hpp"
#include "engine/raytracing_render_graph.hpp"
#include "runtime/native_d3d12_raytracing_context.hpp"
#include "runtime/native_vulkan_raytracing_context.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Runtime-private orchestration boundary for one renderer-neutral graph plan.
// The graph plan and all receipts are plain data.  Native devices, command
// queues, acceleration structures, shader tables and output resources stay
// owned by the selected Native*RayTracingContext implementation.
inline constexpr std::string_view raytracing_context_session_schema =
    "noemancer.raytracing-context-session/0.1";
inline constexpr std::size_t raytracing_context_session_max_text_bytes = 512U;
inline constexpr std::size_t raytracing_context_session_max_triangles =
    native_vulkan_raytracing_context_hard_max_triangles;
inline constexpr std::size_t raytracing_context_session_max_grouped_geometries =
    native_d3d12_raytracing_context_max_geometry_count;
inline constexpr std::size_t raytracing_context_session_max_stages = 8U;

enum class RayTracingContextSessionBackend : std::uint8_t {
    d3d12 = 0U,
    vulkan = 1U,

    D3D12 = d3d12,
    Vulkan = vulkan,
};

enum class RayTracingContextSessionOutcome : std::uint8_t {
    executed = 0U,
    native_ready = 1U,
    fallback = 2U,
    unsupported = 3U,
    failure = 4U,

    Executed = executed,
    NativeReady = native_ready,
    Fallback = fallback,
    Unsupported = unsupported,
    Failure = failure,
};

enum class RayTracingContextSessionStageKind : std::uint8_t {
    initialize = 0U,
    ensure_scene = 1U,
    build = 2U,
    trace = 3U,
    readback = 4U,
    shutdown = 5U,

    Initialize = initialize,
    EnsureScene = ensure_scene,
    Build = build,
    Trace = trace,
    Readback = readback,
    Shutdown = shutdown,
};

[[nodiscard]] std::string_view raytracing_context_session_backend_name(
    RayTracingContextSessionBackend backend) noexcept;
[[nodiscard]] std::string_view raytracing_context_session_outcome_name(
    RayTracingContextSessionOutcome outcome) noexcept;
[[nodiscard]] std::string_view raytracing_context_session_stage_name(
    RayTracingContextSessionStageKind stage) noexcept;

// Stable backend geometry identity for one world-space instance/primitive
// range.  The cache and the session use this same helper so a D3D material
// record can be resolved without positional guessing.  The encoded id is
// bounded, path-safe and deterministic; the human-readable source identities
// remain present alongside it in RayTracingContextSessionGeometryGroup.
[[nodiscard]] inline std::string raytracing_context_session_group_geometry_id(
    const std::string_view instance_id, const std::string_view primitive_id) {
    // FNV-1a over length-prefixed components is small, deterministic and
    // independent of platform string concatenation rules.  The source ids
    // remain in the grouped record for diagnostics; this compact backend key
    // keeps native geometry/material tables bounded and path-safe.
    constexpr std::uint64_t offset_basis = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset_basis;
    const auto add_byte = [&hash](const std::uint8_t value) {
        hash ^= value;
        hash *= prime;
    };
    const auto add_string = [&add_byte](const std::string_view value) {
        auto length = static_cast<std::uint64_t>(value.size());
        for (std::uint32_t index = 0U; index < 8U; ++index)
            add_byte(static_cast<std::uint8_t>((length >> (index * 8U)) & 0xffU));
        for (const auto character : value)
            add_byte(static_cast<std::uint8_t>(character));
    };
    add_string(instance_id);
    add_string(primitive_id);

    constexpr char hex[] = "0123456789abcdef";
    std::string result{"rt.group."};
    result.reserve(result.size() + 16U);
    for (std::int32_t shift = 60; shift >= 0; shift -= 4)
        result.push_back(hex[(hash >> static_cast<std::uint32_t>(shift)) & 0x0fU]);
    return result;
}

struct RayTracingContextSessionTriangle final {
    std::array<std::array<float, 3U>, 3U> positions{};
};

// One canonical world-space triangle range.  `triangles` remains the sole
// position storage for compatibility; this structure is only grouping and
// identity metadata consumed by native adapters.  The backend id is derived
// from instance_id + primitive_id by the stable helper above, while the
// source geometry identity remains observable for diagnostics/material
// lookup.  Ranges are required to partition the triangle vector exactly when
// this metadata is supplied.
struct RayTracingContextSessionGeometryGroup final {
    std::string geometry_id;
    std::string source_geometry_id;
    std::string instance_id;
    std::string primitive_id;
    std::uint32_t first_triangle{};
    std::uint32_t triangle_count{};
};

struct RayTracingContextSessionScene final {
    // This is a bounded transfer snapshot supplied by the existing Render
    // World/Scene authority.  The session never edits, persists or publishes
    // it as a second Scene database.
    std::string scene_id{"rt.session.scene"};
    std::uint64_t topology_revision{1U};
    std::uint64_t content_revision{1U};
    bool allow_update{true};
    std::vector<RayTracingContextSessionTriangle> triangles;
    // Optional production grouping metadata.  Empty means the legacy single
    // geometry transfer path; non-empty data is canonicalized into one D3D
    // geometry per range and flattened for Vulkan's current adapter.
    std::vector<RayTracingContextSessionGeometryGroup> grouped_geometries;
};

struct RayTracingContextSessionTraceRequest final {
    std::array<float, 3U> origin{0.0F, 0.0F, -1.0F};
    std::array<float, 3U> direction{0.0F, 0.0F, 1.0F};
    float minimum_distance{0.0F};
    float maximum_distance{1.0e6F};
};

struct RayTracingContextSessionOptions final {
    RayTracingContextSessionBackend backend{
        RayTracingContextSessionBackend::vulkan};
    // This gate applies to the session projection.  Vulkan forwards it to its
    // deterministic CPU fallback.  D3D12 reports a native-context fallback
    // as unsupported when this is false; the D3D12 context itself remains the
    // source of truth for its capability state.
    bool allow_fallback{true};
    NativeD3D12RayTracingContextOptions d3d12_options{};
    NativeVulkanRayTracingContextOptions vulkan_options{};
};

struct RayTracingContextSessionRequest final {
    std::string session_id{"rt.context-session"};
    RayTracingRenderGraphPlan plan;
    RayTracingContextSessionScene scene;
    RayTracingContextSessionTraceRequest trace{};
    // Optional renderer-neutral primary-ray view.  When present, the session
    // validates and translates this plain-data plan into the selected native
    // backend; legacy marker probes can omit it and retain the historical ray.
    std::optional<NativeRayTracingViewPlan> view;
    // Build and scene submission always follow the selected plan.  These two
    // switches only gate optional proof stages, so a caller can safely stop
    // at a bounded build/trace boundary while shaders or readback are absent.
    bool run_trace{true};
    bool run_readback{true};
    // Optional engine-owned PBR input/plan.  It is observed on Vulkan (which
    // does not yet consume this table) and translated to D3D12 after the
    // canonical scene has been accepted, before AS build/update.
    std::optional<NativeRayTracingShadingPlan> shading;
};

struct RayTracingContextSessionStageReceipt final {
    RayTracingContextSessionStageKind stage{
        RayTracingContextSessionStageKind::initialize};
    bool attempted{};
    bool accepted{};
    bool completed{};
    // Native execution means the selected backend receipt proved that this
    // stage used persistent native resources.  It is never inferred from a
    // device probe alone and never set for a CPU fallback.
    bool native_executed{};
    bool fallback_executed{};
    bool unsupported{};
    bool failed{};
    std::string code;
    std::string detail;
};

struct RayTracingContextSessionPassReceipt final {
    std::string id;
    RayTracingRenderGraphPassKind kind{
        RayTracingRenderGraphPassKind::trace};
    std::size_t execution_index{};
    bool selected{};
    bool translated{};
    bool attempted{};
    bool completed{};
    bool native_executed{};
    bool fallback_executed{};
    bool unsupported{};
    bool failed{};
    std::string code;
};

struct RayTracingContextSessionResourceReceipt final {
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

// A bounded, deterministic projection of one plan-to-context session.  The
// receipt deliberately carries decisions and pass order so an Agent can
// inspect what the adapter consumed without opening a native debugger.  It
// does not carry any backend handle or third-party enum.
struct RayTracingContextSessionReceipt final {
    std::string schema{std::string(raytracing_context_session_schema)};
    std::string session_id;
    std::string backend;
    RayTracingContextSessionOutcome outcome{
        RayTracingContextSessionOutcome::failure};
    std::string code;
    std::string detail;

    bool plan_consumed{};
    bool scene_consumed{};
    bool executed{};
    bool native_ready{};
    bool fallback_active{};
    bool unsupported{};
    bool failed{};
    bool native_handles_exposed{};
    bool shared_device{};
    bool shared_queue{};
    bool output_resource_live{};
    bool output_trace_written{};
    bool output_transfer_candidate{};
    bool full_frame_shader_ready{};
    bool shading_requested{};
    bool shading_valid{};
    bool shading_resources_ready{};
    bool linear_radiance_shader_consumed{};
    bool claims_rtgi{};
    std::string shading_schema;
    std::uint64_t shading_fingerprint{};
    std::uint32_t shading_material_count{};
    bool output_radiance_valid{};
    bool camera_requested{};
    bool camera_valid{};
    bool camera_shader_consumed{};
    std::string camera_id;
    std::string camera_projection;
    std::uint64_t camera_fingerprint{};
    std::uint64_t output_resource_generation{};
    std::string output_format;
    std::string shader_contract;

    std::uint64_t frame_generation{};
    std::uint64_t graph_generation{};
    std::string plan_fingerprint;
    std::vector<std::string> execution_order;
    std::vector<RayTracingContextSessionResourceReceipt> resources;
    std::vector<RayTracingContextSessionPassReceipt> passes;
    std::vector<RayTracingContextSessionStageReceipt> stages;
};

struct RayTracingContextSessionOutputTransferReceipt final {
    std::string schema{"noemancer.raytracing-output-transfer/0.1"};
    std::string backend;
    std::string code{"session.output-transfer-not-attempted"};
    std::string detail;
    bool attempted{};
    bool completed{};
    bool unsupported{};
    bool failed{};
    bool native_handles_exposed{};
    std::uint64_t resource_generation{};
};

// Translate one engine plan into one long-lived native backend context.  The
// session does not construct a World/Scene authority and does not rebuild a
// plan from native state; it consumes the supplied plan exactly as authored.
class RayTracingContextSession final {
public:
    explicit RayTracingContextSession(
        RayTracingContextSessionOptions options = {});
    ~RayTracingContextSession();
    RayTracingContextSession(const RayTracingContextSession&) = delete;
    RayTracingContextSession& operator=(const RayTracingContextSession&) = delete;

    [[nodiscard]] RayTracingContextSessionReceipt execute(
        const RayTracingContextSessionRequest& request);
    // Runtime-private destination input, plain-data receipt output. The
    // destination is never retained or serialized by the session.
    [[nodiscard]] RayTracingContextSessionOutputTransferReceipt transfer_output_to(
        void* destination_resource);
    [[nodiscard]] RayTracingContextSessionReceipt shutdown();
    [[nodiscard]] RayTracingContextSessionReceipt status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Stable, bounded JSON for Agent/CLI observation.  This is a projection of
// the plain receipt only; it never serializes native context internals.
[[nodiscard]] std::string raytracing_context_session_observation_json(
    const RayTracingContextSessionReceipt& receipt);

} // namespace noemancer
