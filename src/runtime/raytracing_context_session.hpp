#pragma once

#include "engine/raytracing_render_graph.hpp"
#include "runtime/native_d3d12_raytracing_context.hpp"
#include "runtime/native_vulkan_raytracing_context.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

struct RayTracingContextSessionTriangle final {
    std::array<std::array<float, 3U>, 3U> positions{};
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
    // Build and scene submission always follow the selected plan.  These two
    // switches only gate optional proof stages, so a caller can safely stop
    // at a bounded build/trace boundary while shaders or readback are absent.
    bool run_trace{true};
    bool run_readback{true};
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
    std::uint64_t output_resource_generation{};
    std::string output_format;

    std::uint64_t frame_generation{};
    std::uint64_t graph_generation{};
    std::string plan_fingerprint;
    std::vector<std::string> execution_order;
    std::vector<RayTracingContextSessionResourceReceipt> resources;
    std::vector<RayTracingContextSessionPassReceipt> passes;
    std::vector<RayTracingContextSessionStageReceipt> stages;
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
