#include "runtime/scene_raytracing_bridge.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

constexpr std::uint64_t kDefaultResourceBytes = 64ULL * 1024ULL;
constexpr std::uint64_t kDefaultScratchBytes = 128ULL * 1024ULL;
constexpr std::uint64_t kSbtBytes = 192ULL;
constexpr std::uint64_t kPixelStrideBytes = sizeof(std::uint32_t) * 4ULL;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U,
                                    std::min(value.size(),
                                             scene_raytracing_bridge_max_text_bytes)));
}

bool checked_mul(const std::uint64_t left, const std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    output = left * right;
    return true;
}

bool ascii_equal_insensitive(const std::string_view left,
                             const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const auto normalize = [](const char value) noexcept {
            const auto byte = static_cast<unsigned char>(value);
            return byte >= static_cast<unsigned char>('A') &&
                    byte <= static_cast<unsigned char>('Z')
                ? static_cast<unsigned char>(byte + ('a' - 'A'))
                : byte;
        };
        if (normalize(left[index]) != normalize(right[index])) return false;
    }
    return true;
}

std::optional<RayTracingContextSessionBackend> parse_backend(
    const std::string_view value) noexcept {
    if (ascii_equal_insensitive(value, "d3d12"))
        return RayTracingContextSessionBackend::d3d12;
    if (ascii_equal_insensitive(value, "vulkan"))
        return RayTracingContextSessionBackend::vulkan;
    return std::nullopt;
}

std::string canonical_backend(const std::string_view value) {
    if (ascii_equal_insensitive(value, "d3d12")) return "d3d12";
    if (ascii_equal_insensitive(value, "vulkan")) return "vulkan";
    return bounded_text(value);
}

bool valid_cache_snapshot(const SceneRayTracingGeometryCacheSnapshot& snapshot) noexcept {
    return snapshot.schema == scene_raytracing_geometry_cache_schema &&
        snapshot.state == SceneRayTracingGeometryCacheState::ready &&
        snapshot.accepted && !snapshot.scene_id.empty() &&
        snapshot.topology_revision != 0U && snapshot.content_revision != 0U &&
        !snapshot.world_triangles.empty() &&
        snapshot.world_triangles.size() <= raytracing_context_session_max_triangles;
}

std::uint64_t resource_bytes_for_triangles(const std::size_t triangle_count,
                                           const std::uint64_t minimum,
                                           const std::uint64_t per_triangle) noexcept {
    std::uint64_t scaled{};
    if (!checked_mul(static_cast<std::uint64_t>(triangle_count), per_triangle, scaled))
        return std::numeric_limits<std::uint64_t>::max();
    return std::max(minimum, scaled);
}

RayTracingRenderGraphResource make_resource(
    std::string id, const RayTracingRenderGraphResourceKind kind,
    const RayTracingRenderGraphResourceLifetime lifetime,
    const std::uint64_t bytes, const std::uint64_t scratch_bytes,
    const std::uint32_t width, const std::uint32_t height,
    const std::uint32_t history_length, const std::uint64_t generation,
    const bool dirty, const bool topology_changed) {
    return RayTracingRenderGraphResource{
        .id = std::move(id),
        .kind = kind,
        .lifetime = lifetime,
        .format = kind == RayTracingRenderGraphResourceKind::output ||
                          kind == RayTracingRenderGraphResourceKind::history
                      ? "rgba32uint"
                      : "buffer",
        .dimension = kind == RayTracingRenderGraphResourceKind::output ||
                             kind == RayTracingRenderGraphResourceKind::history
                         ? "2d"
                         : "buffer",
        .width = width,
        .height = height,
        .depth = 1U,
        .layers = 1U,
        .bytes = bytes,
        .scratch_bytes = scratch_bytes,
        .history_length = history_length,
        .generation = generation,
        .dirty = dirty,
        .topology_changed = topology_changed,
        .refit_requested = false,
    };
}

RayTracingRenderGraphPass make_pass(
    std::string id, const RayTracingRenderGraphPassKind kind,
    std::vector<std::string> reads, std::vector<std::string> writes,
    std::vector<std::string> depends_on = {}, const bool read_modify_write = false) {
    return RayTracingRenderGraphPass{
        .id = std::move(id),
        .kind = kind,
        .reads = std::move(reads),
        .writes = std::move(writes),
        .depends_on = std::move(depends_on),
        .read_modify_write = read_modify_write,
        .enabled = true,
    };
}

RayTracingRenderGraphDescription make_description(
    const SceneRayTracingBridgeOptions& options,
    const SceneRayTracingGeometryCacheSnapshot& snapshot,
    const bool enabled,
    const bool topology_changed,
    const bool content_changed) {
    RayTracingRenderGraphDescription description;
    description.graph_id = "render.graph.scene-raytracing";
    description.graph_generation = options.graph_generation;
    description.policy.enabled = enabled;
    description.policy.allow_raster_fallback = options.allow_fallback;
    description.policy.require_history = true;
    description.budget.max_resident_bytes = raytracing_render_graph_max_resource_bytes;
    description.budget.max_scratch_bytes = raytracing_render_graph_max_resource_bytes;
    description.budget.max_history_bytes = raytracing_render_graph_max_resource_bytes;
    description.budget.max_output_bytes = raytracing_render_graph_max_resource_bytes;

    const auto triangle_count = snapshot.world_triangles.size();
    const auto blas_bytes = resource_bytes_for_triangles(
        triangle_count, kDefaultResourceBytes, 128ULL);
    const auto tlas_bytes = resource_bytes_for_triangles(
        triangle_count, kDefaultResourceBytes, 128ULL);
    const auto blas_scratch = resource_bytes_for_triangles(
        triangle_count, kDefaultScratchBytes, 256ULL);
    const auto tlas_scratch = resource_bytes_for_triangles(
        triangle_count, kDefaultScratchBytes, 256ULL);
    std::uint64_t output_bytes{};
    if (!checked_mul(static_cast<std::uint64_t>(options.output_width),
                    static_cast<std::uint64_t>(options.output_height), output_bytes) ||
        !checked_mul(output_bytes, kPixelStrideBytes, output_bytes)) {
        output_bytes = std::numeric_limits<std::uint64_t>::max();
    }

    description.resources = {
        make_resource("as.blas.scene", RayTracingRenderGraphResourceKind::blas,
                      RayTracingRenderGraphResourceLifetime::persistent,
                      blas_bytes, blas_scratch, 1U, 1U, 1U,
                      snapshot.content_revision, content_changed || topology_changed,
                      topology_changed),
        make_resource("as.tlas.scene", RayTracingRenderGraphResourceKind::tlas,
                      RayTracingRenderGraphResourceLifetime::persistent,
                      tlas_bytes, tlas_scratch, 1U, 1U, 1U,
                      snapshot.content_revision, content_changed || topology_changed,
                      topology_changed),
        make_resource("rt.sbt.scene", RayTracingRenderGraphResourceKind::sbt,
                      RayTracingRenderGraphResourceLifetime::persistent,
                      kSbtBytes, 0U, 1U, 1U, 1U,
                      snapshot.topology_revision, topology_changed, topology_changed),
        make_resource("rt.output", RayTracingRenderGraphResourceKind::output,
                      RayTracingRenderGraphResourceLifetime::transient,
                      output_bytes, 0U, options.output_width, options.output_height,
                      1U, 1U, false, false),
        make_resource("rt.history", RayTracingRenderGraphResourceKind::history,
                      RayTracingRenderGraphResourceLifetime::history,
                      output_bytes, 0U, options.output_width, options.output_height,
                      2U, 1U, false, false),
    };
    description.passes = {
        make_pass("01.build-blas", RayTracingRenderGraphPassKind::build_blas,
                  {}, {"as.blas.scene"}),
        make_pass("02.build-tlas", RayTracingRenderGraphPassKind::build_tlas,
                  {"as.blas.scene"}, {"as.tlas.scene"}, {"01.build-blas"}),
        make_pass("03.build-sbt", RayTracingRenderGraphPassKind::build_sbt,
                  {}, {"rt.sbt.scene"}),
        make_pass("04.trace", RayTracingRenderGraphPassKind::trace,
                  {"as.tlas.scene", "rt.sbt.scene", "rt.history"},
                  {"rt.output"}, {"02.build-tlas", "03.build-sbt"}),
        make_pass("05.raster-fallback", RayTracingRenderGraphPassKind::raster_fallback,
                  {}, {"rt.output"}, {"04.trace"}),
    };
    return description;
}

RayTracingRenderGraphFrameState make_frame_state(
    const std::uint64_t frame_generation, const std::uint64_t graph_generation,
    const std::vector<RayTracingRenderGraphPreviousResource>& previous_resources,
    const bool previous_frame_valid) {
    RayTracingRenderGraphFrameState frame;
    frame.frame_generation = frame_generation;
    frame.graph_generation = graph_generation;
    frame.previous_graph_generation = graph_generation;
    frame.previous_frame_valid = previous_frame_valid;
    frame.history_valid = previous_frame_valid;
    frame.previous_resources = previous_resources;
    return frame;
}

const RayTracingContextSessionStageReceipt* find_stage(
    const RayTracingContextSessionReceipt& receipt,
    const RayTracingContextSessionStageKind stage) noexcept {
    const auto found = std::find_if(
        receipt.stages.begin(), receipt.stages.end(),
        [stage](const auto& value) { return value.stage == stage; });
    return found == receipt.stages.end() ? nullptr : &*found;
}

bool stage_native_completed(const RayTracingContextSessionReceipt& receipt,
                            const RayTracingContextSessionStageKind stage) noexcept {
    const auto* value = find_stage(receipt, stage);
    return value != nullptr && value->native_executed && value->completed &&
        !value->failed && !value->unsupported;
}

void set_fallback(SceneRayTracingBridgeReceipt& result,
                  const std::string_view code, const std::string_view detail) {
    result.fallback_code = bounded_text(code);
    result.fallback_detail = bounded_text(detail);
    result.fallback_active = result.fallback_code != "none";
}

} // namespace

struct SceneRayTracingBridge::Impl final {
    explicit Impl(SceneRayTracingBridgeOptions input)
        : options(std::move(input)) {
        if (options.output_width == 0U) options.output_width = 1U;
        if (options.output_height == 0U) options.output_height = 1U;
        // Both native contexts currently clamp their retained output to a
        // 4096-square surface.  Mirror that bound here so the plan and the
        // selected backend describe the same output dimensions.
        options.output_width = std::min(options.output_width, 4096U);
        options.output_height = std::min(options.output_height, 4096U);
        if (options.graph_generation == 0U) options.graph_generation = 1U;
        last.schema = std::string(scene_raytracing_bridge_schema);
        last.code = "bridge.not-executed";
        last.detail = "No scene geometry cache snapshot has been submitted.";
    }

    SceneRayTracingBridgeOptions options;
    std::unique_ptr<RayTracingContextSession> session;
    std::string backend;
    std::vector<RayTracingRenderGraphPreviousResource> previous_resources;
    std::uint64_t frame_generation{};
    std::uint64_t last_topology_revision{};
    std::uint64_t last_content_revision{};
    std::uint64_t last_topology_fingerprint{};
    std::uint64_t last_content_fingerprint{};
    bool has_frame{};
    bool closed{};
    SceneRayTracingBridgeReceipt last;
};

SceneRayTracingBridge::SceneRayTracingBridge(SceneRayTracingBridgeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

SceneRayTracingBridge::~SceneRayTracingBridge() {
    static_cast<void>(shutdown());
}

SceneRayTracingBridgeReceipt SceneRayTracingBridge::update(
    const SceneRayTracingBridgeRequest& request,
    const SceneRayTracingGeometryCacheSnapshot& snapshot) {
    SceneRayTracingBridgeReceipt result;
    result.schema = std::string(scene_raytracing_bridge_schema);
    result.backend = canonical_backend(request.backend);
    result.enabled = request.enabled;
    result.trace_requested = request.request_trace;
    result.readback_requested = request.request_readback;
    result.requested = request.enabled && request.request_trace;
    result.cache_state = bounded_text(
        scene_raytracing_geometry_cache_state_name(snapshot.state));
    result.topology_revision = snapshot.topology_revision;
    result.content_revision = snapshot.content_revision;
    result.triangle_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        snapshot.world_triangles.size(), std::numeric_limits<std::uint32_t>::max()));
    result.scene_accepted = valid_cache_snapshot(snapshot);
    result.visual_path = result.scene_accepted ? "ssgi-raster-fallback" : "raster-pbr";

    if (impl_->closed) {
        result.failed = true;
        set_fallback(result, "bridge.already-shutdown",
                     "The bridge was shut down and cannot accept another snapshot.");
        result.code = "bridge.already-shutdown";
        result.detail = "A scene ray-tracing bridge cannot execute after shutdown.";
        impl_->last = result;
        return result;
    }

    const auto parsed_backend = parse_backend(request.backend);
    if (!parsed_backend) {
        result.failed = true;
        set_fallback(result, "bridge.backend-unsupported",
                     "Only the d3d12 and vulkan runtime backends are supported by this bridge.");
        result.code = "bridge.backend-unsupported";
        result.detail = "The requested backend is outside the bridge's explicit vocabulary.";
        impl_->last = result;
        return result;
    }
    if (!result.scene_accepted) {
        result.failed = true;
        const auto code = snapshot.fallback.code.empty()
            ? std::string_view{"bridge.cache-not-ready"}
            : std::string_view{snapshot.fallback.code};
        const auto detail = snapshot.fallback.detail.empty()
            ? std::string_view{"The geometry cache snapshot is not a ready accepted triangle scene."}
            : std::string_view{snapshot.fallback.detail};
        set_fallback(result, code, detail);
        result.code = bounded_text(code);
        result.detail = bounded_text(detail);
        impl_->last = result;
        return result;
    }

    if (!impl_->backend.empty() && impl_->backend != result.backend) {
        result.failed = true;
        set_fallback(result, "bridge.backend-switch-requires-shutdown",
                     "A long-lived bridge keeps one backend session; shut it down before selecting another backend.");
        result.code = "bridge.backend-switch-requires-shutdown";
        result.detail = "Changing the backend would invalidate retained native resources.";
        impl_->last = result;
        return result;
    }

    if (!request.enabled) {
        const bool topology_changed = !impl_->has_frame ||
            snapshot.topology_fingerprint != impl_->last_topology_fingerprint ||
            snapshot.topology_revision != impl_->last_topology_revision;
        const bool content_changed = !impl_->has_frame ||
            snapshot.content_fingerprint != impl_->last_content_fingerprint ||
            snapshot.content_revision != impl_->last_content_revision;
        const auto disabled_frame_generation = impl_->frame_generation ==
            std::numeric_limits<std::uint64_t>::max()
            ? impl_->frame_generation
            : impl_->frame_generation + 1U;
        const auto disabled_frame = make_frame_state(
            disabled_frame_generation, impl_->options.graph_generation,
            impl_->previous_resources, impl_->has_frame);
        const auto disabled_description = make_description(
            impl_->options, snapshot, false, topology_changed, content_changed);
        const auto disabled_plan = build_raytracing_render_graph_plan(
            disabled_description, disabled_frame);
        result.plan_valid = disabled_plan.valid;
        result.plan_supported = disabled_plan.supported;
        result.frame_generation = disabled_plan.frame_generation;
        result.graph_generation = disabled_plan.graph_generation;
        result.plan_fingerprint = bounded_text(
            raytracing_render_graph_fingerprint(disabled_plan));
        if (!disabled_plan.valid) {
            result.failed = true;
            set_fallback(result, disabled_plan.code, disabled_plan.detail);
            result.code = bounded_text(disabled_plan.code);
            result.detail = bounded_text(disabled_plan.detail);
            impl_->last = result;
            return result;
        }
        set_fallback(result, "bridge.disabled",
                     "Ray tracing was disabled by the caller; the scene remains on the explicit raster path.");
        result.code = "bridge.disabled";
        result.detail = "No native context work was requested while the bridge was disabled.";
        impl_->last = result;
        return result;
    }

    const bool topology_changed = !impl_->has_frame ||
        snapshot.topology_fingerprint != impl_->last_topology_fingerprint ||
        snapshot.topology_revision != impl_->last_topology_revision;
    const bool content_changed = !impl_->has_frame ||
        snapshot.content_fingerprint != impl_->last_content_fingerprint ||
        snapshot.content_revision != impl_->last_content_revision;
    const auto next_frame_generation = impl_->frame_generation ==
        std::numeric_limits<std::uint64_t>::max()
        ? impl_->frame_generation
        : impl_->frame_generation + 1U;
    const auto frame = make_frame_state(
        next_frame_generation, impl_->options.graph_generation,
        impl_->previous_resources, impl_->has_frame);
    auto description = make_description(impl_->options, snapshot, request.enabled,
                                        topology_changed, content_changed);
    const auto plan = build_raytracing_render_graph_plan(description, frame);
    result.plan_valid = plan.valid;
    result.plan_supported = plan.supported;
    result.frame_generation = plan.frame_generation;
    result.graph_generation = plan.graph_generation;
    result.plan_fingerprint = bounded_text(raytracing_render_graph_fingerprint(plan));
    result.content_updated = impl_->has_frame && content_changed && !topology_changed;
    result.topology_rebuilt = impl_->has_frame && topology_changed;
    if (!plan.valid) {
        result.failed = true;
        set_fallback(result, plan.code, plan.detail);
        result.code = bounded_text(plan.code);
        result.detail = bounded_text(plan.detail);
        impl_->last = result;
        return result;
    }

    if (!impl_->session) {
        RayTracingContextSessionOptions session_options;
        session_options.backend = *parsed_backend;
        session_options.allow_fallback = impl_->options.allow_fallback;
        session_options.vulkan_options.allow_fallback = impl_->options.allow_fallback;
        session_options.vulkan_options.maximum_triangles =
            native_vulkan_raytracing_context_hard_max_triangles;
        session_options.vulkan_options.output_width = impl_->options.output_width;
        session_options.vulkan_options.output_height = impl_->options.output_height;
        session_options.d3d12_options.output_width = impl_->options.output_width;
        session_options.d3d12_options.output_height = impl_->options.output_height;
        impl_->session = std::make_unique<RayTracingContextSession>(session_options);
        impl_->backend = result.backend;
    }

    RayTracingContextSessionRequest session_request;
    session_request.session_id = "scene-raytracing-bridge";
    session_request.plan = plan;
    session_request.scene = to_raytracing_context_session_scene(snapshot);
    session_request.run_trace = request.request_trace;
    session_request.run_readback = request.request_readback;
    const auto session_receipt = impl_->session->execute(session_request);
    result.session_executed = session_receipt.executed;
    result.fallback_active = session_receipt.fallback_active;
    result.failed = session_receipt.failed;
    result.native_as_ready = stage_native_completed(
        session_receipt, RayTracingContextSessionStageKind::build);
    result.native_trace_ready = stage_native_completed(
        session_receipt, RayTracingContextSessionStageKind::trace);
    if (session_receipt.failed) {
        const auto code = session_receipt.code.empty()
            ? std::string_view{"bridge.backend-failed"}
            : std::string_view{session_receipt.code};
        const auto detail = session_receipt.detail.empty()
            ? std::string_view{"The selected backend failed after the trace request; the visible path remains raster fallback."}
            : std::string_view{session_receipt.detail};
        set_fallback(result, code, detail);
        result.code = bounded_text(code);
        result.detail = bounded_text(detail);
    } else if (result.native_trace_ready) {
        // The native output is currently private to the RT context.  It is
        // intentionally not advertised as visible RTGI until a real SDL_GPU
        // texture/import synchronization contract exists.
        result.visual_path = "ssgi-raster-fallback";
        set_fallback(result, "native-output-not-shared",
                     "Native ray-tracing completed, but its output is not yet shared with SDL_GPU; the visible path remains SSGI raster fallback.");
        result.code = "bridge.native-trace-ready-output-not-shared";
        result.detail = "The persistent backend traced the scene; presentation remains on the explicit raster fallback path.";
    } else if (session_receipt.unsupported || session_receipt.fallback_active) {
        const auto code = session_receipt.code.empty()
            ? std::string_view{"bridge.backend-fallback"}
            : std::string_view{session_receipt.code};
        const auto detail = session_receipt.detail.empty()
            ? std::string_view{"The selected backend did not produce a native trace; the visible path remains raster fallback."}
            : std::string_view{session_receipt.detail};
        set_fallback(result, code, detail);
        result.code = bounded_text(session_receipt.code);
        result.detail = bounded_text(session_receipt.detail);
    } else if (!request.request_trace) {
        set_fallback(result, "bridge.trace-not-requested",
                     "The bridge built or retained the scene but the caller did not request a trace pass.");
        result.code = "bridge.trace-not-requested";
        result.detail = "No trace stage was submitted for this frame.";
    } else {
        set_fallback(result, "bridge.native-trace-not-ready",
                     "The session executed without native trace proof; the visible path remains raster fallback.");
        result.code = bounded_text(session_receipt.code);
        result.detail = bounded_text(session_receipt.detail);
    }
    if (result.code.empty()) result.code = "bridge.session-complete";
    if (result.detail.empty()) result.detail = "The scene snapshot was submitted to the persistent ray-tracing session.";

    impl_->frame_generation = next_frame_generation;
    impl_->last_topology_revision = snapshot.topology_revision;
    impl_->last_content_revision = snapshot.content_revision;
    impl_->last_topology_fingerprint = snapshot.topology_fingerprint;
    impl_->last_content_fingerprint = snapshot.content_fingerprint;
    impl_->has_frame = true;
    impl_->previous_resources.clear();
    impl_->previous_resources.reserve(plan.resources.size());
    for (const auto& resource : plan.resources) {
        bool ready = true;
        if (resource.kind == RayTracingRenderGraphResourceKind::blas ||
            resource.kind == RayTracingRenderGraphResourceKind::tlas)
            ready = result.native_as_ready;
        else if (resource.kind == RayTracingRenderGraphResourceKind::sbt ||
                 resource.kind == RayTracingRenderGraphResourceKind::output)
            ready = result.native_trace_ready;
        impl_->previous_resources.push_back(
            RayTracingRenderGraphPreviousResource{
                resource.id, resource.kind, resource.generation, ready});
    }
    result.fallback_active = result.fallback_active ||
        result.fallback_code != "none";
    impl_->last = result;
    return result;
}

SceneRayTracingBridgeReceipt SceneRayTracingBridge::update(
    const std::string_view backend,
    const SceneRayTracingGeometryCacheSnapshot& snapshot,
    const bool enabled,
    const bool request_trace,
    const bool request_readback) {
    SceneRayTracingBridgeRequest request;
    request.backend = std::string(backend);
    request.enabled = enabled;
    request.request_trace = request_trace;
    request.request_readback = request_readback;
    return update(request, snapshot);
}

SceneRayTracingBridgeReceipt SceneRayTracingBridge::execute(
    const SceneRayTracingBridgeRequest& request,
    const SceneRayTracingGeometryCacheSnapshot& snapshot) {
    return update(request, snapshot);
}

SceneRayTracingBridgeReceipt SceneRayTracingBridge::shutdown() {
    if (impl_->closed) {
        auto result = impl_->last;
        result.shutdown_completed = true;
        result.code = "bridge.shutdown-idempotent";
        result.detail = "The long-lived scene ray-tracing bridge was already shut down.";
        impl_->last = result;
        return result;
    }
    auto result = impl_->last;
    if (impl_->session) {
        const auto session_receipt = impl_->session->shutdown();
        result.session_executed = session_receipt.executed;
        result.failed = session_receipt.failed;
    }
    impl_->closed = true;
    result.shutdown_completed = true;
    result.code = "bridge.shutdown-complete";
    result.detail = "The persistent ray-tracing session and bridge-owned frame state were shut down.";
    result.fallback_active = false;
    result.fallback_code = "none";
    result.fallback_detail.clear();
    impl_->last = result;
    return result;
}

SceneRayTracingBridgeReceipt SceneRayTracingBridge::status() const {
    return impl_->last;
}

SceneRayTracingBridgeReceipt SceneRayTracingBridge::observation() const {
    return impl_->last;
}

} // namespace noemancer
