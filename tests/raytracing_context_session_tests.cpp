#include "engine/raytracing_render_graph.hpp"
#include "runtime/raytracing_context_session.hpp"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "raytracing_context_session_tests: " << message << '\n';
    return condition;
}

RayTracingRenderGraphResource resource(
    const std::string& id, const RayTracingRenderGraphResourceKind kind,
    const RayTracingRenderGraphResourceLifetime lifetime,
    const std::uint64_t bytes, const std::uint64_t scratch_bytes = 0U,
    const std::uint32_t history_length = 1U) {
    return RayTracingRenderGraphResource{
        .id = id,
        .kind = kind,
        .lifetime = lifetime,
        .format = kind == RayTracingRenderGraphResourceKind::history
                      ? "rgba16f"
                      : (kind == RayTracingRenderGraphResourceKind::output
                             ? "rgba16f"
                             : "buffer"),
        .dimension = kind == RayTracingRenderGraphResourceKind::history ||
                             kind == RayTracingRenderGraphResourceKind::output
                         ? "2d"
                         : "buffer",
        .width = 4U,
        .height = 4U,
        .depth = 1U,
        .layers = 1U,
        .bytes = bytes,
        .scratch_bytes = scratch_bytes,
        .history_length = history_length,
        .generation = 1U,
        .dirty = false,
        .topology_changed = false,
        .refit_requested = false,
    };
}

RayTracingRenderGraphPass pass(
    const std::string& id, const RayTracingRenderGraphPassKind kind,
    std::vector<std::string> reads, std::vector<std::string> writes,
    std::vector<std::string> depends_on = {}, const bool read_modify_write = false) {
    return RayTracingRenderGraphPass{
        .id = id,
        .kind = kind,
        .reads = std::move(reads),
        .writes = std::move(writes),
        .depends_on = std::move(depends_on),
        .read_modify_write = read_modify_write,
        .enabled = true,
    };
}

RayTracingRenderGraphDescription graph_description() {
    RayTracingRenderGraphDescription description;
    description.graph_id = "render.graph.session-fixture";
    description.resources = {
        resource("as.blas.main", RayTracingRenderGraphResourceKind::blas,
                 RayTracingRenderGraphResourceLifetime::persistent,
                 64U * 1024U, 128U * 1024U),
        resource("as.tlas.main", RayTracingRenderGraphResourceKind::tlas,
                 RayTracingRenderGraphResourceLifetime::persistent,
                 64U * 1024U, 128U * 1024U),
        resource("rt.sbt.main", RayTracingRenderGraphResourceKind::sbt,
                 RayTracingRenderGraphResourceLifetime::persistent, 1024U),
        resource("rt.output", RayTracingRenderGraphResourceKind::output,
                 RayTracingRenderGraphResourceLifetime::transient, 4U * 4U * 8U),
        resource("rt.history", RayTracingRenderGraphResourceKind::history,
                 RayTracingRenderGraphResourceLifetime::history,
                 4U * 4U * 8U, 0U, 2U),
    };
    description.passes = {
        pass("01.build-blas", RayTracingRenderGraphPassKind::build_blas, {},
             {"as.blas.main"}),
        pass("02.build-tlas", RayTracingRenderGraphPassKind::build_tlas,
             {"as.blas.main"}, {"as.tlas.main"}, {"01.build-blas"}),
        pass("03.build-sbt", RayTracingRenderGraphPassKind::build_sbt, {},
             {"rt.sbt.main"}),
        pass("04.trace", RayTracingRenderGraphPassKind::trace,
             {"as.tlas.main", "rt.sbt.main"}, {"rt.output"},
             {"02.build-tlas", "03.build-sbt"}),
    };
    return description;
}

RayTracingContextSessionScene triangle_scene() {
    RayTracingContextSessionScene scene;
    scene.scene_id = "session.fixture.scene";
    scene.triangles.push_back(RayTracingContextSessionTriangle{
        .positions = {{{-1.0F, -1.0F, 0.0F},
                       {1.0F, -1.0F, 0.0F},
                       {0.0F, 1.0F, 0.0F}}}});
    return scene;
}

RayTracingContextSessionRequest request_for(
    const RayTracingRenderGraphPlan& plan) {
    RayTracingContextSessionRequest request;
    request.session_id = "session.fixture";
    request.plan = plan;
    request.scene = triangle_scene();
    return request;
}

bool test_plan_to_fallback_and_bounded_observation() {
    const auto plan = build_raytracing_render_graph_plan(graph_description());
    if (!check(plan.valid && plan.supported &&
                   plan.mode == RayTracingRenderGraphMode::ray_tracing,
               "fixture graph did not produce a supported plan"))
        return false;

    RayTracingContextSessionOptions options;
    options.backend = RayTracingContextSessionBackend::vulkan;
    RayTracingContextSession session(options);
    const auto receipt = session.execute(request_for(plan));
    // A machine with a Vulkan-capable device may reach the native AS boundary
    // before the RT pipeline/SBT slice is available; another machine may use
    // the deterministic CPU fallback.  Both are valid here, but neither may
    // be promoted to native-ready without complete trace/readback proof.
    if (!check(receipt.plan_consumed && receipt.scene_consumed && receipt.executed &&
                   !receipt.native_handles_exposed &&
                   (receipt.outcome == RayTracingContextSessionOutcome::fallback ||
                    receipt.outcome == RayTracingContextSessionOutcome::unsupported ||
                    receipt.outcome == RayTracingContextSessionOutcome::native_ready),
               "the Vulkan context boundary was not projected explicitly"))
        return false;
    if (!check(receipt.outcome == RayTracingContextSessionOutcome::native_ready
                   ? receipt.native_ready
                   : !receipt.native_ready,
               "native-ready was inferred from an incomplete context receipt"))
        return false;
    if (!check(receipt.execution_order == plan.execution_order &&
                   receipt.resources.size() == plan.resources.size() &&
                   receipt.passes.size() == plan.passes.size() &&
                   receipt.stages.size() >= 3U && receipt.stages.size() <= 5U &&
                   (receipt.stages.size() != 3U ||
                    std::ranges::any_of(receipt.passes, [](const auto& pass) {
                        return pass.kind == RayTracingRenderGraphPassKind::build_sbt &&
                            pass.unsupported;
                    })) &&
                   (receipt.stages.size() != 4U ||
                    (receipt.stages.back().stage ==
                         RayTracingContextSessionStageKind::trace &&
                     receipt.stages.back().unsupported)),
               "the session did not retain bounded plan order and stage evidence"))
        return false;

    const auto observation = nlohmann::json::parse(
        raytracing_context_session_observation_json(receipt));
    if (!check(observation.at("observation") == "raytracing-context-session" &&
                   observation.at("outcome").get<std::string>() ==
                       raytracing_context_session_outcome_name(receipt.outcome) &&
                   observation.at("nativeReady") == receipt.native_ready &&
                   observation.at("nativeHandlesExposed") == false &&
                   observation.at("executionOrder").size() == plan.execution_order.size() &&
                   observation.at("stages").size() == receipt.stages.size(),
               "session observation was not stable plain data"))
        return false;

    const auto second = session.execute(request_for(plan));
    const bool reused = second.outcome == receipt.outcome &&
        second.plan_fingerprint == receipt.plan_fingerprint &&
        second.execution_order == receipt.execution_order &&
        second.resources.size() == receipt.resources.size();
    if (!reused) {
        std::cerr << "first=" << raytracing_context_session_outcome_name(receipt.outcome)
                  << " second=" << raytracing_context_session_outcome_name(second.outcome)
                  << " firstCode=" << receipt.code << " secondCode=" << second.code << '\n';
    }
    return check(reused, "reusing a context session changed the plan projection");
}

bool test_explicit_plan_fallback_and_unsupported() {
    auto description = graph_description();
    description.capabilities.device_supported = false;
    auto plan = build_raytracing_render_graph_plan(description);
    if (!check(plan.mode == RayTracingRenderGraphMode::raster_fallback &&
                   plan.fallback.active,
               "device capability failure did not select graph fallback"))
        return false;
    RayTracingContextSession session;
    const auto fallback = session.execute(request_for(plan));
    if (!check(fallback.outcome == RayTracingContextSessionOutcome::fallback &&
                   fallback.fallback_active && !fallback.executed &&
                   fallback.stages.empty(),
               "explicit raster fallback touched a native context"))
        return false;

    description.policy.allow_raster_fallback = false;
    plan = build_raytracing_render_graph_plan(description);
    if (!check(plan.mode == RayTracingRenderGraphMode::unsupported &&
                   !plan.fallback.active,
               "fallback-disabled graph did not become unsupported"))
        return false;
    const auto unsupported = session.execute(request_for(plan));
    return check(unsupported.outcome == RayTracingContextSessionOutcome::unsupported &&
                     unsupported.unsupported && !unsupported.executed &&
                     unsupported.stages.empty(),
                 "explicit unsupported plan was not fail-closed");
}

bool test_invalid_plan_and_unmapped_prefix() {
    const auto plan = build_raytracing_render_graph_plan(graph_description());
    RayTracingContextSession session;
    auto invalid_request = request_for(plan);
    invalid_request.plan.schema = "noemancer.legacy-raytracing/0.0";
    const auto invalid = session.execute(invalid_request);
    if (!check(invalid.outcome == RayTracingContextSessionOutcome::failure &&
                   invalid.failed && !invalid.plan_consumed && invalid.stages.empty(),
               "schema mismatch was allowed to reach a backend"))
        return false;

    auto unmapped_description = graph_description();
    unmapped_description.passes.push_back(pass(
        "05.denoise", RayTracingRenderGraphPassKind::denoise,
        {"rt.output", "rt.history"}, {"rt.output", "rt.history"},
        {"04.trace"}, true));
    const auto unmapped_plan = build_raytracing_render_graph_plan(unmapped_description);
    if (!check(unmapped_plan.valid && unmapped_plan.supported,
               "unmapped pass fixture was not a valid engine plan"))
        return false;
    const auto partial = session.execute(request_for(unmapped_plan));
    const auto denoise = std::find_if(
        partial.passes.begin(), partial.passes.end(),
        [](const auto& pass) { return pass.id == "05.denoise"; });
    return check((partial.outcome == RayTracingContextSessionOutcome::fallback ||
                  partial.outcome == RayTracingContextSessionOutcome::unsupported) &&
                     partial.unsupported &&
                     !partial.native_ready && partial.stages.size() >= 3U &&
                     partial.stages.size() <= 5U && denoise != partial.passes.end() &&
                     (denoise->code == "session.pass-unsupported" ||
                      denoise->code == "session.not-reached"),
                 "adapter did not stop at the farthest safely mapped prefix");
}

} // namespace

int main() {
    if (!test_plan_to_fallback_and_bounded_observation()) return 1;
    if (!test_explicit_plan_fallback_and_unsupported()) return 2;
    if (!test_invalid_plan_and_unmapped_prefix()) return 3;
    std::cout << "raytracing_context_session_tests: ok\n";
    return 0;
}
