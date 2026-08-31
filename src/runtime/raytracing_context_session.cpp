#include "runtime/raytracing_context_session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, raytracing_context_session_max_text_bytes));
}

bool valid_text(const std::string_view value) noexcept {
    if (value.empty() || value.size() > raytracing_context_session_max_text_bytes)
        return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == 0x7fU) return false;
    }
    return true;
}

bool valid_id(const std::string_view value) noexcept {
    if (!valid_text(value)) return false;
    for (const auto character : value) {
        if (character == '/' || character == '\\' || character == ' ' ||
            character == '\t')
            return false;
    }
    return true;
}

bool valid_resource_kind(const RayTracingRenderGraphResourceKind kind) noexcept {
    switch (kind) {
    case RayTracingRenderGraphResourceKind::blas:
    case RayTracingRenderGraphResourceKind::tlas:
    case RayTracingRenderGraphResourceKind::sbt:
    case RayTracingRenderGraphResourceKind::output:
    case RayTracingRenderGraphResourceKind::history:
        return true;
    }
    return false;
}

bool valid_lifetime(const RayTracingRenderGraphResourceLifetime lifetime) noexcept {
    switch (lifetime) {
    case RayTracingRenderGraphResourceLifetime::persistent:
    case RayTracingRenderGraphResourceLifetime::transient:
    case RayTracingRenderGraphResourceLifetime::history:
        return true;
    }
    return false;
}

bool valid_decision(const RayTracingRenderGraphBuildDecision decision) noexcept {
    switch (decision) {
    case RayTracingRenderGraphBuildDecision::none:
    case RayTracingRenderGraphBuildDecision::build:
    case RayTracingRenderGraphBuildDecision::update:
    case RayTracingRenderGraphBuildDecision::refit:
    case RayTracingRenderGraphBuildDecision::rebuild:
    case RayTracingRenderGraphBuildDecision::clear:
    case RayTracingRenderGraphBuildDecision::unsupported:
        return true;
    }
    return false;
}

bool valid_pass_kind(const RayTracingRenderGraphPassKind kind) noexcept {
    switch (kind) {
    case RayTracingRenderGraphPassKind::build_blas:
    case RayTracingRenderGraphPassKind::update_blas:
    case RayTracingRenderGraphPassKind::refit_blas:
    case RayTracingRenderGraphPassKind::build_tlas:
    case RayTracingRenderGraphPassKind::update_tlas:
    case RayTracingRenderGraphPassKind::refit_tlas:
    case RayTracingRenderGraphPassKind::build_sbt:
    case RayTracingRenderGraphPassKind::trace:
    case RayTracingRenderGraphPassKind::denoise:
    case RayTracingRenderGraphPassKind::resolve:
    case RayTracingRenderGraphPassKind::clear_history:
    case RayTracingRenderGraphPassKind::raster_fallback:
        return true;
    }
    return false;
}

bool is_build_pass(const RayTracingRenderGraphPassKind kind) noexcept {
    switch (kind) {
    case RayTracingRenderGraphPassKind::build_blas:
    case RayTracingRenderGraphPassKind::update_blas:
    case RayTracingRenderGraphPassKind::refit_blas:
    case RayTracingRenderGraphPassKind::build_tlas:
    case RayTracingRenderGraphPassKind::update_tlas:
    case RayTracingRenderGraphPassKind::refit_tlas:
        return true;
    default:
        return false;
    }
}

bool is_context_unmapped_pass(const RayTracingRenderGraphPassKind kind) noexcept {
    switch (kind) {
    case RayTracingRenderGraphPassKind::denoise:
    case RayTracingRenderGraphPassKind::resolve:
    case RayTracingRenderGraphPassKind::clear_history:
    case RayTracingRenderGraphPassKind::raster_fallback:
        return true;
    default:
        return false;
    }
}

void resolve_deferred_sbt_passes(
    RayTracingContextSessionReceipt& receipt,
    const RayTracingContextSessionStageReceipt& trace_stage) {
    for (auto& pass : receipt.passes) {
        if (!pass.selected || pass.kind != RayTracingRenderGraphPassKind::build_sbt ||
            pass.code != "session.pipeline-deferred-to-trace")
            continue;
        pass.attempted = true;
        pass.completed = trace_stage.completed;
        pass.native_executed = trace_stage.native_executed;
        pass.fallback_executed = trace_stage.fallback_executed;
        pass.unsupported = trace_stage.unsupported;
        pass.failed = trace_stage.failed;
        pass.code = trace_stage.completed
            ? "session.pipeline-and-sbt-ready"
            : trace_stage.code;
    }
}

struct PlanValidation final {
    bool valid{};
    std::string code;
    std::string detail;
};

PlanValidation plan_error(const std::string_view code,
                          const std::string_view detail) {
    return {false, std::string(code), std::string(detail)};
}

PlanValidation validate_plan(const RayTracingRenderGraphPlan& plan) {
    if (plan.schema != raytracing_render_graph_schema)
        return plan_error("session.plan-schema-mismatch",
                          "The session accepts only the current engine ray-tracing graph schema.");
    if (!plan.valid)
        return plan_error("session.plan-invalid",
                          "The engine graph plan is invalid and the backend was not touched.");
    if (!valid_id(plan.graph_id))
        return plan_error("session.plan-graph-id-invalid",
                          "The graph ID must be a bounded path-safe stable ID.");
    if (plan.resources.size() > raytracing_render_graph_max_resources ||
        plan.passes.size() > raytracing_render_graph_max_passes ||
        plan.execution_order.size() > raytracing_render_graph_max_passes)
        return plan_error("session.plan-size-exceeded",
                          "The graph plan exceeds the bounded session contract.");
    if (plan.execution_order.empty())
        return plan_error("session.plan-no-passes",
                          "A backend session requires at least one graph pass.");
    if (plan.passes.size() != plan.execution_order.size())
        return plan_error("session.plan-pass-order-invalid",
                          "The explicit execution order must cover every graph pass exactly once.");

    std::vector<std::string> resource_ids;
    resource_ids.reserve(plan.resources.size());
    for (const auto& resource : plan.resources) {
        if (!valid_id(resource.id))
            return plan_error("session.plan-resource-id-invalid",
                              "A graph resource ID is not a bounded stable ID.");
        if (!valid_resource_kind(resource.kind) || !valid_lifetime(resource.lifetime) ||
            !valid_decision(resource.decision))
            return plan_error("session.plan-resource-vocabulary-invalid",
                              "A graph resource contains an unknown kind, lifetime or decision.");
        if (std::find(resource_ids.begin(), resource_ids.end(), resource.id) !=
            resource_ids.end())
            return plan_error("session.plan-resource-duplicate",
                              "Graph resource IDs must be unique in a session plan.");
        resource_ids.push_back(resource.id);
    }

    std::vector<std::string> pass_ids;
    pass_ids.reserve(plan.passes.size());
    std::vector<bool> execution_indices(plan.passes.size(), false);
    for (const auto& pass : plan.passes) {
        if (!valid_id(pass.id) || !valid_pass_kind(pass.kind))
            return plan_error("session.plan-pass-invalid",
                              "A graph pass ID or kind is outside the stable contract.");
        if (pass.execution_index >= plan.passes.size() ||
            execution_indices[pass.execution_index])
            return plan_error("session.plan-pass-order-invalid",
                              "Graph pass execution indices must be contiguous and unique.");
        if (pass.selected && !pass.enabled)
            return plan_error("session.plan-pass-selection-invalid",
                              "A disabled pass cannot be selected for backend translation.");
        if (std::find(pass_ids.begin(), pass_ids.end(), pass.id) != pass_ids.end())
            return plan_error("session.plan-pass-duplicate",
                              "Graph pass IDs must be unique in a session plan.");
        execution_indices[pass.execution_index] = true;
        pass_ids.push_back(pass.id);
    }
    for (std::size_t index = 0U; index < execution_indices.size(); ++index) {
        if (!execution_indices[index])
            return plan_error("session.plan-pass-order-invalid",
                              "Graph pass execution indices must cover the complete order.");
        const auto ordered = std::find_if(
            plan.passes.begin(), plan.passes.end(),
            [&](const auto& pass) { return pass.execution_index == index; });
        if (ordered == plan.passes.end() || plan.execution_order[index] != ordered->id)
            return plan_error("session.plan-execution-order-mismatch",
                              "The explicit execution order does not match pass indices.");
    }

    switch (plan.mode) {
    case RayTracingRenderGraphMode::ray_tracing:
        if (!plan.supported || plan.fallback.active)
            return plan_error("session.plan-mode-inconsistent",
                              "A ray-tracing mode plan must be supported without active fallback.");
        break;
    case RayTracingRenderGraphMode::raster_fallback:
        if (plan.supported || !plan.fallback.active)
            return plan_error("session.plan-mode-inconsistent",
                              "A raster fallback plan must be unsupported with explicit fallback.");
        break;
    case RayTracingRenderGraphMode::unsupported:
        if (plan.supported || plan.fallback.active)
            return plan_error("session.plan-mode-inconsistent",
                              "An unsupported plan cannot also claim support or active fallback.");
        break;
    case RayTracingRenderGraphMode::error:
        return plan_error("session.plan-invalid",
                          "An error-mode graph plan cannot enter a native context.");
    default:
        return plan_error("session.plan-mode-invalid",
                          "The graph plan mode is outside the stable session vocabulary.");
    }
    return {true, {}, {}};
}

const RayTracingRenderGraphPassPlan* pass_at_execution_index(
    const RayTracingRenderGraphPlan& plan, const std::size_t execution_index) {
    for (const auto& pass : plan.passes)
        if (pass.execution_index == execution_index) return &pass;
    return nullptr;
}

const RayTracingRenderGraphPassPlan* find_pass(
    const RayTracingRenderGraphPlan& plan, const std::string_view id) {
    for (const auto& pass : plan.passes)
        if (pass.id == id) return &pass;
    return nullptr;
}

RayTracingContextSessionPassReceipt* find_receipt_pass(
    RayTracingContextSessionReceipt& receipt, const std::string_view id) {
    for (auto& pass : receipt.passes)
        if (pass.id == id) return &pass;
    return nullptr;
}

RayTracingContextSessionStageReceipt make_stage(
    const RayTracingContextSessionStageKind kind,
    const std::string_view code = {}, const std::string_view detail = {}) {
    RayTracingContextSessionStageReceipt result;
    result.stage = kind;
    result.code = bounded_text(code);
    result.detail = bounded_text(detail);
    return result;
}

void copy_native_text(RayTracingContextSessionStageReceipt& stage,
                      const std::string& code, const std::string& detail) {
    stage.code = bounded_text(code);
    stage.detail = bounded_text(detail);
}

RayTracingContextSessionStageReceipt map_d3d_stage(
    const RayTracingContextSessionStageKind kind,
    const NativeD3D12RayTracingContextReceipt& native) {
    auto result = make_stage(kind);
    result.attempted = true;
    copy_native_text(result, native.code, native.detail);
    result.failed = native.state == NativeD3D12RayTracingContextState::failed;
    result.unsupported = native.state == NativeD3D12RayTracingContextState::unsupported &&
        !native.fallback_active;
    result.fallback_executed = native.fallback_active;

    switch (kind) {
    case RayTracingContextSessionStageKind::initialize:
        result.accepted = native.initialized || native.fallback_active ||
            native.state == NativeD3D12RayTracingContextState::ready;
        result.completed = result.accepted && !result.failed;
        result.native_executed = native.state == NativeD3D12RayTracingContextState::ready &&
            native.device_ready && native.command_queue_ready && native.fence_ready;
        break;
    case RayTracingContextSessionStageKind::ensure_scene:
        result.accepted = native.scene_received && !result.failed &&
            native.state != NativeD3D12RayTracingContextState::shutdown;
        result.completed = result.accepted;
        result.native_executed = result.accepted &&
            native.state == NativeD3D12RayTracingContextState::ready;
        break;
    case RayTracingContextSessionStageKind::build:
        result.accepted = native.scene_received && !result.failed &&
            native.state != NativeD3D12RayTracingContextState::shutdown;
        result.completed = native.blas_ready && native.tlas_ready;
        result.native_executed = result.completed &&
            native.state == NativeD3D12RayTracingContextState::ready &&
            !native.fallback_active;
        break;
    case RayTracingContextSessionStageKind::trace:
        result.accepted = native.scene_received && !result.failed &&
            native.state != NativeD3D12RayTracingContextState::shutdown;
        result.completed = native.trace_completed;
        result.native_executed = native.trace_submitted && native.trace_completed &&
            native.blas_ready && native.tlas_ready && native.shader_pipeline_ready &&
            native.shader_table_ready && native.output_resource_ready &&
            !native.fallback_active;
        break;
    case RayTracingContextSessionStageKind::readback:
        result.accepted = native.trace_completed && !result.failed &&
            native.state != NativeD3D12RayTracingContextState::shutdown;
        result.completed = native.readback_completed;
        result.native_executed = native.readback_completed &&
            native.output_resource_ready && !native.fallback_active;
        break;
    case RayTracingContextSessionStageKind::shutdown:
        result.accepted = native.shutdown_completed ||
            native.state == NativeD3D12RayTracingContextState::shutdown;
        result.completed = native.shutdown_completed;
        break;
    }
    return result;
}

RayTracingContextSessionStageReceipt map_vulkan_stage(
    const RayTracingContextSessionStageKind kind,
    const NativeVulkanRayTracingContextReceipt& native) {
    auto result = make_stage(kind);
    result.attempted = true;
    copy_native_text(result, native.code, native.detail);
    result.failed = native.state == NativeVulkanRayTracingContextState::error;
    result.unsupported = native.state == NativeVulkanRayTracingContextState::unsupported &&
        !native.fallback_active;
    result.fallback_executed = native.fallback_active ||
        native.state == NativeVulkanRayTracingContextState::fallback;

    switch (kind) {
    case RayTracingContextSessionStageKind::initialize:
        result.accepted = native.initialized || result.fallback_executed;
        result.completed = (native.state == NativeVulkanRayTracingContextState::ready ||
                            native.state == NativeVulkanRayTracingContextState::fallback ||
                            native.state == NativeVulkanRayTracingContextState::unsupported) &&
            !result.failed;
        result.native_executed = native.state == NativeVulkanRayTracingContextState::ready &&
            native.persistent_backend;
        break;
    case RayTracingContextSessionStageKind::ensure_scene:
        result.accepted = native.scene_ready && !result.failed && !native.shutdown;
        result.completed = result.accepted;
        result.native_executed = result.accepted && native.persistent_backend &&
            native.state == NativeVulkanRayTracingContextState::ready;
        break;
    case RayTracingContextSessionStageKind::build:
        result.accepted = native.scene_ready && !result.failed && !native.shutdown;
        result.completed = native.build_completed ||
            native.code == "native-vulkan-rt.context-native-build-cached";
        result.native_executed = result.completed && native.persistent_backend &&
            native.state == NativeVulkanRayTracingContextState::ready &&
            !native.fallback_active;
        break;
    case RayTracingContextSessionStageKind::trace:
        result.accepted = native.scene_ready && !result.failed && !native.shutdown;
        result.completed = native.trace_completed;
        result.native_executed = native.trace_submitted && native.trace_completed &&
            native.persistent_backend && !native.fallback_active;
        break;
    case RayTracingContextSessionStageKind::readback:
        result.accepted = native.trace_completed && !result.failed && !native.shutdown;
        result.completed = native.readback_completed;
        result.native_executed = native.readback_completed && native.persistent_backend &&
            !native.fallback_active;
        break;
    case RayTracingContextSessionStageKind::shutdown:
        result.accepted = native.shutdown ||
            native.state == NativeVulkanRayTracingContextState::shutdown;
        result.completed = native.shutdown;
        break;
    }
    return result;
}

void append_stage(RayTracingContextSessionReceipt& receipt,
                  RayTracingContextSessionStageReceipt stage) {
    if (receipt.stages.size() < raytracing_context_session_max_stages)
        receipt.stages.push_back(std::move(stage));
}

void initialize_receipt_from_request(
    RayTracingContextSessionReceipt& receipt,
    const RayTracingContextSessionRequest& request,
    const RayTracingContextSessionOptions& options) {
    receipt.session_id = bounded_text(request.session_id);
    receipt.backend = std::string(
        raytracing_context_session_backend_name(options.backend));
    receipt.frame_generation = request.plan.frame_generation;
    receipt.graph_generation = request.plan.graph_generation;
    receipt.plan_fingerprint = raytracing_render_graph_fingerprint(request.plan);
    receipt.execution_order.reserve(request.plan.execution_order.size());
    for (const auto& id : request.plan.execution_order)
        receipt.execution_order.push_back(bounded_text(id));
    receipt.resources.reserve(request.plan.resources.size());
    for (const auto& resource : request.plan.resources) {
        receipt.resources.push_back(RayTracingContextSessionResourceReceipt{
            .id = bounded_text(resource.id),
            .kind = resource.kind,
            .lifetime = resource.lifetime,
            .generation = resource.generation,
            .previous_generation = resource.previous_generation,
            .decision = resource.decision,
            .preserve_history = resource.preserve_history,
            .reset_history = resource.reset_history,
        });
    }
    std::sort(receipt.resources.begin(), receipt.resources.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });
    receipt.passes.reserve(request.plan.passes.size());
    for (std::size_t index = 0U; index < request.plan.execution_order.size(); ++index) {
        const auto* pass = pass_at_execution_index(request.plan, index);
        if (pass == nullptr) continue;
        receipt.passes.push_back(RayTracingContextSessionPassReceipt{
            .id = bounded_text(pass->id),
            .kind = pass->kind,
            .execution_index = pass->execution_index,
            .selected = pass->selected,
            .translated = false,
            .attempted = false,
            .completed = false,
            .native_executed = false,
            .fallback_executed = false,
            .unsupported = false,
            .failed = false,
            .code = {},
        });
    }
    receipt.native_handles_exposed = false;
}

void mark_pass_from_stage(RayTracingContextSessionPassReceipt& pass,
                          const RayTracingContextSessionStageReceipt& stage,
                          const std::string_view code = {}) {
    pass.translated = true;
    pass.attempted = stage.attempted;
    pass.completed = stage.completed;
    pass.native_executed = stage.native_executed;
    pass.fallback_executed = stage.fallback_executed;
    pass.unsupported = stage.unsupported;
    pass.failed = stage.failed;
    pass.code = bounded_text(code.empty() ? stage.code : code);
}

const RayTracingContextSessionStageReceipt* find_stage(
    const RayTracingContextSessionReceipt& receipt,
    const RayTracingContextSessionStageKind kind) {
    for (const auto& stage : receipt.stages)
        if (stage.stage == kind) return &stage;
    return nullptr;
}

void finalize_receipt(RayTracingContextSessionReceipt& receipt,
                      const RayTracingContextSessionOptions& options,
                      const bool has_selected_trace,
                      const bool has_unmapped_pass) {
    for (const auto& stage : receipt.stages) {
        receipt.executed |= stage.attempted &&
            (stage.accepted || stage.completed || stage.fallback_executed);
        receipt.failed |= stage.failed;
        receipt.unsupported |= stage.unsupported;
        receipt.fallback_active |= stage.fallback_executed;
    }
    for (const auto& pass : receipt.passes) {
        receipt.executed |= pass.attempted &&
            (pass.completed || pass.fallback_executed);
        receipt.failed |= pass.failed;
        receipt.unsupported |= pass.unsupported;
        receipt.fallback_active |= pass.fallback_executed;
    }

    const auto* initialize = find_stage(receipt,
                                         RayTracingContextSessionStageKind::initialize);
    const auto* scene = find_stage(receipt,
                                   RayTracingContextSessionStageKind::ensure_scene);
    const auto* build = find_stage(receipt, RayTracingContextSessionStageKind::build);
    const auto* trace = find_stage(receipt, RayTracingContextSessionStageKind::trace);
    const auto* readback = find_stage(receipt,
                                      RayTracingContextSessionStageKind::readback);
    receipt.native_ready = has_selected_trace && initialize != nullptr &&
        initialize->native_executed && scene != nullptr && scene->native_executed &&
        (!build || build->native_executed) && trace != nullptr &&
        trace->native_executed && readback != nullptr && readback->native_executed &&
        !has_unmapped_pass && !receipt.fallback_active && !receipt.unsupported &&
        !receipt.failed;

    if (receipt.failed) {
        receipt.outcome = RayTracingContextSessionOutcome::failure;
        receipt.code = "session.backend-failed";
        receipt.detail = "The selected backend returned a failure; no later stage was promoted.";
    } else if (receipt.fallback_active && !options.allow_fallback) {
        receipt.unsupported = true;
        receipt.outcome = RayTracingContextSessionOutcome::unsupported;
        receipt.code = "session.fallback-disallowed";
        receipt.detail = "The backend exposed fallback evidence but this session disallows fallback.";
    } else if (receipt.native_ready) {
        receipt.outcome = RayTracingContextSessionOutcome::native_ready;
        receipt.code = "session.native-ready";
        receipt.detail = "The plan reached native trace and readback proof without fallback.";
    } else if (receipt.fallback_active) {
        receipt.outcome = RayTracingContextSessionOutcome::fallback;
        receipt.code = has_unmapped_pass ? "session.fallback-before-unmapped-pass"
                                         : "session.backend-fallback";
        receipt.detail = has_unmapped_pass
            ? "Backend fallback completed the reachable prefix; a later graph pass has no context mapping."
            : "The backend completed an explicit fallback path; native readiness is false.";
    } else if (receipt.unsupported || has_unmapped_pass) {
        receipt.unsupported = true;
        receipt.outcome = RayTracingContextSessionOutcome::unsupported;
        receipt.code = has_unmapped_pass ? "session.pass-unsupported"
                                         : "session.backend-unsupported";
        receipt.detail = has_unmapped_pass
            ? "The graph contains a pass outside this bounded context adapter."
            : "The selected backend cannot execute this plan and no fallback was accepted.";
    } else if (receipt.executed) {
        receipt.outcome = RayTracingContextSessionOutcome::executed;
        receipt.code = "session.executed";
        receipt.detail = "The reachable context stages executed without native-ready proof.";
    } else {
        receipt.outcome = RayTracingContextSessionOutcome::failure;
        receipt.failed = true;
        receipt.code = "session.not-executed";
        receipt.detail = "No backend stage reached an accepted execution boundary.";
    }
    receipt.code = bounded_text(receipt.code);
    receipt.detail = bounded_text(receipt.detail);
}

void mark_unreached_selected_passes(RayTracingContextSessionReceipt& receipt,
                                    const std::size_t from_index) {
    for (auto& pass : receipt.passes) {
        if (!pass.selected || pass.execution_index < from_index || pass.attempted ||
            !pass.code.empty())
            continue;
        pass.code = "session.not-reached";
    }
}

bool valid_scene(const RayTracingContextSessionScene& scene,
                 std::string& code, std::string& detail) {
    if (!valid_id(scene.scene_id)) {
        code = "session.scene-id-invalid";
        detail = "The session scene ID must be a bounded path-safe stable ID.";
        return false;
    }
    if (scene.topology_revision == 0U || scene.content_revision == 0U) {
        code = "session.scene-revision-invalid";
        detail = "Scene topology and content revisions must be greater than zero.";
        return false;
    }
    if (scene.triangles.empty() ||
        scene.triangles.size() > raytracing_context_session_max_triangles) {
        code = "session.scene-size-invalid";
        detail = "The scene must contain at least one triangle within the bounded budget.";
        return false;
    }
    for (const auto& triangle : scene.triangles) {
        for (const auto& position : triangle.positions) {
            for (const auto coordinate : position) {
                if (!std::isfinite(coordinate) || std::abs(coordinate) > 1.0e6F) {
                    code = "session.scene-coordinate-invalid";
                    detail = "Scene coordinates must be finite and within the bounded range.";
                    return false;
                }
            }
        }
    }
    return true;
}

NativeD3D12RayTracingScene make_d3d_scene(
    const RayTracingContextSessionScene& source) {
    NativeD3D12RayTracingScene scene;
    scene.scene_id = source.scene_id;
    scene.revision = std::max(source.topology_revision, source.content_revision);
    scene.allow_update = source.allow_update;
    NativeD3D12RayTracingGeometry geometry;
    geometry.geometry_id = "rt.session.geometry";
    geometry.allow_update = source.allow_update;
    geometry.position_xyz.reserve(source.triangles.size() * 9U);
    geometry.indices.reserve(source.triangles.size() * 3U);
    for (std::size_t index = 0U; index < source.triangles.size(); ++index) {
        for (const auto& position : source.triangles[index].positions)
            geometry.position_xyz.insert(geometry.position_xyz.end(),
                                         position.begin(), position.end());
        const auto base = static_cast<std::uint32_t>(index * 3U);
        geometry.indices.insert(geometry.indices.end(), {base, base + 1U, base + 2U});
    }
    scene.geometries.push_back(std::move(geometry));
    return scene;
}

std::vector<NativeVulkanRayTracingTriangle> make_vulkan_triangles(
    const RayTracingContextSessionScene& source) {
    std::vector<NativeVulkanRayTracingTriangle> result;
    result.reserve(source.triangles.size());
    for (const auto& triangle : source.triangles)
        result.push_back(NativeVulkanRayTracingTriangle{triangle.positions});
    return result;
}

Json stage_json(const RayTracingContextSessionStageReceipt& stage) {
    return Json{
        {"stage", raytracing_context_session_stage_name(stage.stage)},
        {"attempted", stage.attempted},
        {"accepted", stage.accepted},
        {"completed", stage.completed},
        {"nativeExecuted", stage.native_executed},
        {"fallbackExecuted", stage.fallback_executed},
        {"unsupported", stage.unsupported},
        {"failed", stage.failed},
        {"code", bounded_text(stage.code)},
        {"detail", bounded_text(stage.detail)},
    };
}

} // namespace

struct RayTracingContextSession::Impl final {
    RayTracingContextSessionOptions options;
    std::unique_ptr<NativeD3D12RayTracingContext> d3d12;
    std::unique_ptr<NativeVulkanRayTracingContext> vulkan;
    RayTracingContextSessionReceipt last;
    bool closed{};

    explicit Impl(RayTracingContextSessionOptions input)
        : options(std::move(input)) {
        last.backend = std::string(
            raytracing_context_session_backend_name(options.backend));
        last.code = "session.not-executed";
        last.detail = "No graph plan has been submitted to this context session.";
        last.native_handles_exposed = false;
        if (options.backend == RayTracingContextSessionBackend::d3d12) {
            d3d12 = std::make_unique<NativeD3D12RayTracingContext>(
                options.d3d12_options);
        } else if (options.backend == RayTracingContextSessionBackend::vulkan) {
            options.vulkan_options.allow_fallback = options.vulkan_options.allow_fallback &&
                options.allow_fallback;
            vulkan = std::make_unique<NativeVulkanRayTracingContext>(
                options.vulkan_options);
        }
    }
};

std::string_view raytracing_context_session_backend_name(
    const RayTracingContextSessionBackend backend) noexcept {
    switch (backend) {
    case RayTracingContextSessionBackend::d3d12: return "d3d12";
    case RayTracingContextSessionBackend::vulkan: return "vulkan";
    }
    return "unknown";
}

std::string_view raytracing_context_session_outcome_name(
    const RayTracingContextSessionOutcome outcome) noexcept {
    switch (outcome) {
    case RayTracingContextSessionOutcome::executed: return "executed";
    case RayTracingContextSessionOutcome::native_ready: return "native-ready";
    case RayTracingContextSessionOutcome::fallback: return "fallback";
    case RayTracingContextSessionOutcome::unsupported: return "unsupported";
    case RayTracingContextSessionOutcome::failure: return "failure";
    }
    return "failure";
}

std::string_view raytracing_context_session_stage_name(
    const RayTracingContextSessionStageKind stage) noexcept {
    switch (stage) {
    case RayTracingContextSessionStageKind::initialize: return "initialize";
    case RayTracingContextSessionStageKind::ensure_scene: return "ensure-scene";
    case RayTracingContextSessionStageKind::build: return "build";
    case RayTracingContextSessionStageKind::trace: return "trace";
    case RayTracingContextSessionStageKind::readback: return "readback";
    case RayTracingContextSessionStageKind::shutdown: return "shutdown";
    }
    return "unknown";
}

RayTracingContextSession::RayTracingContextSession(
    RayTracingContextSessionOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

RayTracingContextSession::~RayTracingContextSession() = default;

RayTracingContextSessionOutputTransferReceipt
RayTracingContextSession::transfer_output_to(void* destination_resource) {
    RayTracingContextSessionOutputTransferReceipt result;
    result.backend = std::string(
        raytracing_context_session_backend_name(impl_->options.backend));
    if (impl_->closed) {
        result.failed = true;
        result.code = "session.output-transfer-after-shutdown";
        result.detail = "A closed native session cannot transfer an output resource.";
        return result;
    }
    if (destination_resource == nullptr) {
        result.failed = true;
        result.code = "session.output-transfer-destination-null";
        result.detail = "The runtime-private output destination is null.";
        return result;
    }
    if (impl_->options.backend != RayTracingContextSessionBackend::d3d12 || !impl_->d3d12) {
        result.unsupported = true;
        result.code = "session.output-transfer-backend-unsupported";
        result.detail = "The current Vulkan context has no completed SDL image ownership transfer path.";
        return result;
    }
    const auto view = impl_->d3d12->private_output_surface_view();
    result.resource_generation = view.metadata.resource_generation;
    result.attempted = true;
    const auto native = impl_->d3d12->copy_output_to(view, destination_resource);
    result.completed = native.output_copy_submitted && native.output_copy_completed &&
        native.state == NativeD3D12RayTracingContextState::ready;
    result.failed = !result.completed;
    result.code = bounded_text(native.code);
    result.detail = bounded_text(native.detail);
    return result;
}

RayTracingContextSessionReceipt RayTracingContextSession::execute(
    const RayTracingContextSessionRequest& request) {
    RayTracingContextSessionReceipt receipt;
    receipt.session_id = bounded_text(request.session_id);
    receipt.backend = std::string(
        raytracing_context_session_backend_name(impl_->options.backend));
    receipt.native_handles_exposed = false;
    if (impl_->closed) {
        receipt.failed = true;
        receipt.code = "session.already-shutdown";
        receipt.detail = "A session cannot accept a plan after shutdown.";
        receipt.outcome = RayTracingContextSessionOutcome::failure;
        impl_->last = receipt;
        return receipt;
    }
    if (!valid_id(request.session_id)) {
        receipt.failed = true;
        receipt.code = "session.id-invalid";
        receipt.detail = "The session ID must be a bounded path-safe stable ID.";
        receipt.outcome = RayTracingContextSessionOutcome::failure;
        impl_->last = receipt;
        return receipt;
    }
    if (impl_->options.backend != RayTracingContextSessionBackend::d3d12 &&
        impl_->options.backend != RayTracingContextSessionBackend::vulkan) {
        receipt.failed = true;
        receipt.code = "session.backend-invalid";
        receipt.detail = "The session backend selector is outside the supported vocabulary.";
        receipt.outcome = RayTracingContextSessionOutcome::failure;
        impl_->last = receipt;
        return receipt;
    }

    const auto validation = validate_plan(request.plan);
    if (!validation.valid) {
        receipt.failed = true;
        receipt.code = bounded_text(validation.code);
        receipt.detail = bounded_text(validation.detail);
        receipt.outcome = RayTracingContextSessionOutcome::failure;
        impl_->last = receipt;
        return receipt;
    }
    // Copy and fingerprint only after the bounded plan validation above.  A
    // malformed caller-supplied vector must not force an unbounded receipt
    // allocation or a large canonicalization pass.
    initialize_receipt_from_request(receipt, request, impl_->options);
    receipt.plan_consumed = true;

    if (request.plan.mode == RayTracingRenderGraphMode::raster_fallback) {
        receipt.fallback_active = true;
        receipt.outcome = RayTracingContextSessionOutcome::fallback;
        receipt.code = "session.plan-raster-fallback";
        receipt.detail = "The engine plan selected raster fallback; no native RT context was touched.";
        for (auto& pass : receipt.passes) {
            if (pass.selected && pass.kind == RayTracingRenderGraphPassKind::raster_fallback) {
                pass.translated = true;
                pass.fallback_executed = true;
                pass.code = "session.plan-raster-fallback";
            } else if (!pass.selected) {
                pass.code = "session.pass-not-selected";
            }
        }
        impl_->last = receipt;
        return receipt;
    }
    if (request.plan.mode == RayTracingRenderGraphMode::unsupported) {
        receipt.unsupported = true;
        receipt.outcome = RayTracingContextSessionOutcome::unsupported;
        receipt.code = "session.plan-unsupported";
        receipt.detail = "The engine plan explicitly disabled native execution and fallback.";
        for (auto& pass : receipt.passes)
            if (!pass.selected) pass.code = "session.pass-not-selected";
        impl_->last = receipt;
        return receipt;
    }

    std::string scene_code;
    std::string scene_detail;
    if (!valid_scene(request.scene, scene_code, scene_detail)) {
        receipt.failed = true;
        receipt.code = scene_code;
        receipt.detail = scene_detail;
        receipt.outcome = RayTracingContextSessionOutcome::failure;
        impl_->last = receipt;
        return receipt;
    }

    bool has_selected_trace = false;
    for (const auto& pass : receipt.passes) {
        if (pass.selected && pass.kind == RayTracingRenderGraphPassKind::trace)
            has_selected_trace = true;
    }
    bool has_unmapped_pass = false;
    bool stopped = false;
    std::size_t stop_index = request.plan.execution_order.size();

    if (impl_->options.backend == RayTracingContextSessionBackend::d3d12) {
        auto init = impl_->d3d12->initialize();
        append_stage(receipt, map_d3d_stage(
            RayTracingContextSessionStageKind::initialize, init));
        if (receipt.stages.back().failed || receipt.stages.back().unsupported) {
            stopped = true;
            stop_index = 0U;
        } else {
            auto ensure = impl_->d3d12->ensure_scene(make_d3d_scene(request.scene));
            receipt.scene_consumed = ensure.scene_received;
            append_stage(receipt, map_d3d_stage(
                RayTracingContextSessionStageKind::ensure_scene, ensure));
            if (receipt.stages.back().failed || receipt.stages.back().unsupported) {
                stopped = true;
                stop_index = 0U;
            }
        }

        bool build_called = false;
        for (std::size_t index = 0U;
             index < request.plan.execution_order.size() && !stopped; ++index) {
            const auto* planned = find_pass(request.plan,
                                             request.plan.execution_order[index]);
            auto* pass = find_receipt_pass(receipt, request.plan.execution_order[index]);
            if (planned == nullptr || pass == nullptr || !planned->selected) continue;
            if (is_build_pass(planned->kind)) {
                if (!build_called) {
                    const auto native = impl_->d3d12->build_or_update();
                    append_stage(receipt, map_d3d_stage(
                        RayTracingContextSessionStageKind::build, native));
                    const auto& stage = receipt.stages.back();
                    mark_pass_from_stage(*pass, stage);
                    build_called = true;
                    if (stage.failed || stage.unsupported) {
                        stopped = true;
                        stop_index = index;
                    }
                } else {
                    pass->translated = true;
                    pass->completed = receipt.stages.back().completed;
                    pass->native_executed = receipt.stages.back().native_executed;
                    pass->fallback_executed = receipt.stages.back().fallback_executed;
                    pass->unsupported = receipt.stages.back().unsupported;
                    pass->failed = receipt.stages.back().failed;
                    pass->code = "session.shared-build-operation";
                }
                continue;
            }
            if (planned->kind == RayTracingRenderGraphPassKind::trace) {
                if (!request.run_trace) {
                    pass->translated = true;
                    pass->code = "session.trace-disabled";
                    continue;
                }
                const auto native = impl_->d3d12->trace();
                append_stage(receipt, map_d3d_stage(
                    RayTracingContextSessionStageKind::trace, native));
                mark_pass_from_stage(*pass, receipt.stages.back());
                resolve_deferred_sbt_passes(receipt, receipt.stages.back());
                if (receipt.stages.back().failed || receipt.stages.back().unsupported) {
                    stopped = true;
                    stop_index = index;
                }
                continue;
            }
            if (planned->kind == RayTracingRenderGraphPassKind::build_sbt) {
                pass->translated = true;
                pass->code = "session.pipeline-deferred-to-trace";
                continue;
            }
            if (is_context_unmapped_pass(planned->kind)) {
                pass->translated = false;
                pass->unsupported = true;
                pass->code = "session.pass-unsupported";
                has_unmapped_pass = true;
                stopped = true;
                stop_index = index;
                break;
            }
        }
        const auto* trace_stage = find_stage(
            receipt, RayTracingContextSessionStageKind::trace);
        if (request.run_readback && has_selected_trace && trace_stage != nullptr &&
            !trace_stage->failed && !trace_stage->unsupported) {
            const auto native = impl_->d3d12->readback();
            append_stage(receipt, map_d3d_stage(
                RayTracingContextSessionStageKind::readback, native));
        }
    } else {
        auto init = impl_->vulkan->initialize();
        append_stage(receipt, map_vulkan_stage(
            RayTracingContextSessionStageKind::initialize, init));
        if (receipt.stages.back().failed || receipt.stages.back().unsupported) {
            stopped = true;
            stop_index = 0U;
        } else {
            auto triangles = make_vulkan_triangles(request.scene);
            const NativeVulkanRayTracingScene scene{
                .triangles = std::span<const NativeVulkanRayTracingTriangle>(triangles),
                .topology_revision = request.scene.topology_revision,
                .content_revision = request.scene.content_revision,
            };
            const auto ensure = impl_->vulkan->ensure_scene(scene);
            receipt.scene_consumed = ensure.scene_ready;
            append_stage(receipt, map_vulkan_stage(
                RayTracingContextSessionStageKind::ensure_scene, ensure));
            if (receipt.stages.back().failed || receipt.stages.back().unsupported) {
                stopped = true;
                stop_index = 0U;
            }
        }

        bool build_called = false;
        for (std::size_t index = 0U;
             index < request.plan.execution_order.size() && !stopped; ++index) {
            const auto* planned = find_pass(request.plan,
                                             request.plan.execution_order[index]);
            auto* pass = find_receipt_pass(receipt, request.plan.execution_order[index]);
            if (planned == nullptr || pass == nullptr || !planned->selected) continue;
            if (is_build_pass(planned->kind)) {
                if (!build_called) {
                    const auto native = impl_->vulkan->build_or_update();
                    append_stage(receipt, map_vulkan_stage(
                        RayTracingContextSessionStageKind::build, native));
                    const auto& stage = receipt.stages.back();
                    mark_pass_from_stage(*pass, stage);
                    build_called = true;
                    if (stage.failed || stage.unsupported) {
                        stopped = true;
                        stop_index = index;
                    }
                } else {
                    pass->translated = true;
                    pass->completed = receipt.stages.back().completed;
                    pass->native_executed = receipt.stages.back().native_executed;
                    pass->fallback_executed = receipt.stages.back().fallback_executed;
                    pass->unsupported = receipt.stages.back().unsupported;
                    pass->failed = receipt.stages.back().failed;
                    pass->code = "session.shared-build-operation";
                }
                continue;
            }
            if (planned->kind == RayTracingRenderGraphPassKind::trace) {
                if (!request.run_trace) {
                    pass->translated = true;
                    pass->code = "session.trace-disabled";
                    continue;
                }
                NativeVulkanRayTracingTraceRequest trace;
                trace.origin = request.trace.origin;
                trace.direction = request.trace.direction;
                trace.minimum_distance = request.trace.minimum_distance;
                trace.maximum_distance = request.trace.maximum_distance;
                const auto native = impl_->vulkan->trace(trace);
                append_stage(receipt, map_vulkan_stage(
                    RayTracingContextSessionStageKind::trace, native));
                mark_pass_from_stage(*pass, receipt.stages.back());
                resolve_deferred_sbt_passes(receipt, receipt.stages.back());
                if (receipt.stages.back().failed || receipt.stages.back().unsupported) {
                    stopped = true;
                    stop_index = index;
                }
                continue;
            }
            if (planned->kind == RayTracingRenderGraphPassKind::build_sbt) {
                pass->translated = true;
                pass->code = "session.pipeline-deferred-to-trace";
                continue;
            }
            if (is_context_unmapped_pass(planned->kind)) {
                pass->translated = false;
                pass->unsupported = true;
                pass->code = "session.pass-unsupported";
                has_unmapped_pass = true;
                stopped = true;
                stop_index = index;
                break;
            }
        }
        const auto* trace_stage = find_stage(
            receipt, RayTracingContextSessionStageKind::trace);
        if (request.run_readback && has_selected_trace && trace_stage != nullptr &&
            !trace_stage->failed && !trace_stage->unsupported) {
            const auto native = impl_->vulkan->readback();
            append_stage(receipt, map_vulkan_stage(
                RayTracingContextSessionStageKind::readback, native));
        }
    }

    if (stopped) mark_unreached_selected_passes(receipt, stop_index);
    if (impl_->options.backend == RayTracingContextSessionBackend::d3d12) {
        const auto native = impl_->d3d12->status();
        receipt.shared_device = native.shared_device;
        receipt.shared_queue = native.shared_command_queue;
        receipt.output_resource_live = native.output_surface.resource_ready;
        receipt.output_trace_written = native.output_surface.gpu_write_complete;
        receipt.output_transfer_candidate = native.output_surface.valid &&
            native.output_surface.direct_sdl_gpu_import_supported;
        receipt.output_resource_generation = native.output_surface.resource_generation;
        receipt.output_format = native.output_surface.format;
    } else {
        // Vulkan stage receipts remain the authority until its storage-image
        // output is wired into the session. Borrowed-handle presence alone is
        // deliberately insufficient to promote an interop candidate.
        receipt.shared_device = false;
        receipt.shared_queue = false;
    }
    for (auto& pass : receipt.passes)
        if (!pass.selected && pass.code.empty()) pass.code = "session.pass-not-selected";
    finalize_receipt(receipt, impl_->options, has_selected_trace, has_unmapped_pass);
    impl_->last = receipt;
    return receipt;
}

RayTracingContextSessionReceipt RayTracingContextSession::shutdown() {
    if (impl_->closed) return impl_->last;
    auto receipt = impl_->last;
    receipt.native_handles_exposed = false;
    if (impl_->options.backend == RayTracingContextSessionBackend::d3d12 && impl_->d3d12) {
        const auto native = impl_->d3d12->shutdown();
        append_stage(receipt, map_d3d_stage(
            RayTracingContextSessionStageKind::shutdown, native));
    } else if (impl_->options.backend == RayTracingContextSessionBackend::vulkan &&
               impl_->vulkan) {
        const auto native = impl_->vulkan->shutdown();
        append_stage(receipt, map_vulkan_stage(
            RayTracingContextSessionStageKind::shutdown, native));
    }
    impl_->closed = true;
    receipt.code = "session.shutdown-complete";
    receipt.detail = "The backend context was shut down; later plan submissions are rejected.";
    receipt.outcome = RayTracingContextSessionOutcome::executed;
    receipt.failed = false;
    receipt.unsupported = false;
    receipt.fallback_active = false;
    impl_->last = receipt;
    return receipt;
}

RayTracingContextSessionReceipt RayTracingContextSession::status() const {
    return impl_->last;
}

std::string raytracing_context_session_observation_json(
    const RayTracingContextSessionReceipt& receipt) {
    Json resources = Json::array();
    for (const auto& resource : receipt.resources) {
        resources.push_back(Json{
            {"id", bounded_text(resource.id)},
            {"kind", raytracing_render_graph_resource_kind_name(resource.kind)},
            {"lifetime", raytracing_render_graph_resource_lifetime_name(resource.lifetime)},
            {"generation", resource.generation},
            {"previousGeneration", resource.previous_generation},
            {"decision", raytracing_render_graph_build_decision_name(resource.decision)},
            {"preserveHistory", resource.preserve_history},
            {"resetHistory", resource.reset_history},
        });
    }
    Json passes = Json::array();
    for (const auto& pass : receipt.passes) {
        passes.push_back(Json{
            {"id", bounded_text(pass.id)},
            {"kind", raytracing_render_graph_pass_kind_name(pass.kind)},
            {"executionIndex", pass.execution_index},
            {"selected", pass.selected},
            {"translated", pass.translated},
            {"attempted", pass.attempted},
            {"completed", pass.completed},
            {"nativeExecuted", pass.native_executed},
            {"fallbackExecuted", pass.fallback_executed},
            {"unsupported", pass.unsupported},
            {"failed", pass.failed},
            {"code", bounded_text(pass.code)},
        });
    }
    Json execution_order = Json::array();
    for (const auto& id : receipt.execution_order)
        execution_order.push_back(bounded_text(id));
    Json stages = Json::array();
    for (const auto& stage : receipt.stages) stages.push_back(stage_json(stage));

    return Json{
        {"observation", "raytracing-context-session"},
        {"observationVersion", "0.1"},
        {"schema", bounded_text(receipt.schema)},
        {"sessionId", bounded_text(receipt.session_id)},
        {"backend", bounded_text(receipt.backend)},
        {"outcome", raytracing_context_session_outcome_name(receipt.outcome)},
        {"code", bounded_text(receipt.code)},
        {"detail", bounded_text(receipt.detail)},
        {"planConsumed", receipt.plan_consumed},
        {"sceneConsumed", receipt.scene_consumed},
        {"executed", receipt.executed},
        {"nativeReady", receipt.native_ready},
        {"sharedDevice", receipt.shared_device},
        {"sharedQueue", receipt.shared_queue},
        {"outputResourceLive", receipt.output_resource_live},
        {"outputTraceWritten", receipt.output_trace_written},
        {"outputTransferCandidate", receipt.output_transfer_candidate},
        {"outputResourceGeneration", receipt.output_resource_generation},
        {"outputFormat", receipt.output_format},
        {"fallbackActive", receipt.fallback_active},
        {"unsupported", receipt.unsupported},
        {"failed", receipt.failed},
        {"nativeHandlesExposed", false},
        {"frameGeneration", receipt.frame_generation},
        {"graphGeneration", receipt.graph_generation},
        {"planFingerprint", bounded_text(receipt.plan_fingerprint)},
        {"executionOrder", std::move(execution_order)},
        {"resources", std::move(resources)},
        {"passes", std::move(passes)},
        {"stages", std::move(stages)},
    }.dump();
}

} // namespace noemancer
