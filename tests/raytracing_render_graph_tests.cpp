#include "engine/raytracing_render_graph.hpp"

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
        std::cerr << "raytracing_render_graph_tests: " << message << '\n';
    return condition;
}

RayTracingRenderGraphResource resource(
    const std::string& id, const RayTracingRenderGraphResourceKind kind,
    const RayTracingRenderGraphResourceLifetime lifetime,
    const std::uint64_t bytes, const std::uint64_t scratch_bytes = 0U,
    const std::uint32_t history_length = 1U,
    const std::uint64_t generation = 1U) {
    return RayTracingRenderGraphResource{
        .id = id,
        .kind = kind,
        .lifetime = lifetime,
        .format = kind == RayTracingRenderGraphResourceKind::history ||
                          kind == RayTracingRenderGraphResourceKind::output
                      ? "rgba16f"
                      : "buffer",
        .dimension = kind == RayTracingRenderGraphResourceKind::history ||
                             kind == RayTracingRenderGraphResourceKind::output
                         ? "2d"
                         : "buffer",
        .width = 128U,
        .height = 128U,
        .depth = 1U,
        .layers = 1U,
        .bytes = bytes,
        .scratch_bytes = scratch_bytes,
        .history_length = history_length,
        .generation = generation,
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

RayTracingRenderGraphDescription complete_graph() {
    RayTracingRenderGraphDescription description;
    description.graph_id = "render.graph.persistent-rt";
    description.resources = {
        resource("as.blas.mesh", RayTracingRenderGraphResourceKind::blas,
                 RayTracingRenderGraphResourceLifetime::persistent,
                 64U * 1024U, 128U * 1024U),
        resource("as.tlas.scene", RayTracingRenderGraphResourceKind::tlas,
                 RayTracingRenderGraphResourceLifetime::persistent,
                 64U * 1024U, 128U * 1024U),
        resource("rt.sbt.main", RayTracingRenderGraphResourceKind::sbt,
                 RayTracingRenderGraphResourceLifetime::persistent, 1024U),
        resource("rt.output", RayTracingRenderGraphResourceKind::output,
                 RayTracingRenderGraphResourceLifetime::transient,
                 128U * 128U * 8U),
        resource("rt.history", RayTracingRenderGraphResourceKind::history,
                 RayTracingRenderGraphResourceLifetime::history,
                 128U * 128U * 8U, 0U, 2U),
    };
    description.passes = {
        pass("01.build-blas", RayTracingRenderGraphPassKind::build_blas, {},
             {"as.blas.mesh"}),
        pass("02.build-tlas", RayTracingRenderGraphPassKind::build_tlas,
             {"as.blas.mesh"}, {"as.tlas.scene"}, {"01.build-blas"}),
        pass("03.build-sbt", RayTracingRenderGraphPassKind::build_sbt, {},
             {"rt.sbt.main"}),
        pass("04.trace", RayTracingRenderGraphPassKind::trace,
             {"as.tlas.scene", "rt.sbt.main"}, {"rt.output"},
             {"02.build-tlas", "03.build-sbt"}),
        pass("05.denoise", RayTracingRenderGraphPassKind::denoise,
             {"rt.output", "rt.history"}, {"rt.output", "rt.history"},
             {"04.trace"}, true),
    };
    description.policy.require_history = true;
    return description;
}

RayTracingRenderGraphFrameState ready_previous() {
    RayTracingRenderGraphFrameState frame;
    frame.frame_generation = 2U;
    frame.graph_generation = 1U;
    frame.previous_graph_generation = 1U;
    frame.previous_frame_valid = true;
    frame.history_valid = true;
    frame.previous_resources = {
        {"as.blas.mesh", RayTracingRenderGraphResourceKind::blas, 1U, true},
        {"as.tlas.scene", RayTracingRenderGraphResourceKind::tlas, 1U, true},
        {"rt.sbt.main", RayTracingRenderGraphResourceKind::sbt, 1U, true},
        {"rt.output", RayTracingRenderGraphResourceKind::output, 1U, true},
        {"rt.history", RayTracingRenderGraphResourceKind::history, 1U, true},
    };
    return frame;
}

bool test_vocabulary_and_initial_plan() {
    const auto description = complete_graph();
    if (!check(raytracing_render_graph_schema ==
                   "noemancer.raytracing-render-graph/0.1",
               "schema drifted"))
        return false;
    if (!check(raytracing_render_graph_resource_kind_name(
                   RayTracingRenderGraphResourceKind::tlas) == "tlas" &&
                   raytracing_render_graph_pass_kind_name(
                       RayTracingRenderGraphPassKind::raster_fallback) ==
                       "raster-fallback" &&
                   raytracing_render_graph_build_decision_name(
                       RayTracingRenderGraphBuildDecision::refit) == "refit",
               "vocabulary drifted"))
        return false;

    const auto diagnostics = validate_raytracing_render_graph(description);
    if (!check(diagnostics.empty(), "complete graph was rejected")) return false;
    RayTracingRenderGraphFrameState initial;
    initial.frame_generation = 1U;
    initial.graph_generation = 1U;
    initial.previous_graph_generation = 1U;
    const auto plan = build_raytracing_render_graph_plan(description, initial);
    if (!check(plan.valid && plan.supported &&
                   plan.mode == RayTracingRenderGraphMode::ray_tracing &&
                   plan.budget.fits,
               "initial graph did not produce a supported plan"))
        return false;
    const auto find_resource = [&](const std::string_view id)
        -> const RayTracingRenderGraphResourcePlan* {
        const auto found = std::find_if(plan.resources.begin(), plan.resources.end(),
                                        [&](const auto& value) { return value.id == id; });
        return found == plan.resources.end() ? nullptr : &*found;
    };
    const auto* blas = find_resource("as.blas.mesh");
    const auto* tlas = find_resource("as.tlas.scene");
    const auto* sbt = find_resource("rt.sbt.main");
    const auto* history = find_resource("rt.history");
    return check(blas != nullptr && tlas != nullptr && sbt != nullptr &&
                     history != nullptr &&
                     blas->decision == RayTracingRenderGraphBuildDecision::build &&
                     tlas->decision == RayTracingRenderGraphBuildDecision::build &&
                     sbt->decision == RayTracingRenderGraphBuildDecision::build &&
                     history->decision == RayTracingRenderGraphBuildDecision::clear &&
                     history->reset_history && plan.execution_order.size() == 5U,
                 "initial build/clear decisions or topological order were incorrect");
}

bool test_deterministic_evidence() {
    auto first_description = complete_graph();
    auto second_description = complete_graph();
    std::reverse(second_description.resources.begin(), second_description.resources.end());
    std::reverse(second_description.passes.begin(), second_description.passes.end());
    for (auto& current : second_description.passes) {
        std::reverse(current.reads.begin(), current.reads.end());
        std::reverse(current.writes.begin(), current.writes.end());
        std::reverse(current.depends_on.begin(), current.depends_on.end());
    }
    RayTracingRenderGraphFrameState frame;
    frame.frame_generation = 1U;
    frame.graph_generation = 1U;
    frame.previous_graph_generation = 1U;
    const auto first = build_raytracing_render_graph_plan(first_description, frame);
    const auto second = build_raytracing_render_graph_plan(second_description, frame);
    const auto first_description_json =
        raytracing_render_graph_canonical_description(first_description);
    const auto second_description_json =
        raytracing_render_graph_canonical_description(second_description);
    const auto first_evidence = raytracing_render_graph_canonical_evidence(first);
    const auto second_evidence = raytracing_render_graph_canonical_evidence(second);
    return check(first_description_json == second_description_json &&
                     first_evidence == second_evidence &&
                     raytracing_render_graph_fingerprint(first) ==
                         raytracing_render_graph_fingerprint(second),
                 "source ordering changed canonical graph evidence");
}

bool test_generation_lifecycle_and_capability_fallback() {
    auto description = complete_graph();
    auto frame = ready_previous();
    for (auto& resource : description.resources) {
        if (resource.id == "as.blas.mesh") {
            resource.generation = 2U;
            resource.dirty = true;
            resource.refit_requested = true;
        } else if (resource.id == "as.tlas.scene") {
            resource.generation = 2U;
            resource.dirty = true;
        } else if (resource.id == "rt.sbt.main") {
            resource.generation = 2U;
            resource.dirty = true;
        }
    }
    const auto plan = build_raytracing_render_graph_plan(description, frame);
    const auto find_resource = [&](const RayTracingRenderGraphPlan& value,
                                   const std::string_view id)
        -> const RayTracingRenderGraphResourcePlan* {
        const auto found = std::find_if(value.resources.begin(), value.resources.end(),
                                        [&](const auto& item) { return item.id == id; });
        return found == value.resources.end() ? nullptr : &*found;
    };
    if (!check(plan.supported &&
                   find_resource(plan, "as.blas.mesh")->decision ==
                       RayTracingRenderGraphBuildDecision::refit &&
                   find_resource(plan, "as.tlas.scene")->decision ==
                       RayTracingRenderGraphBuildDecision::update &&
                   find_resource(plan, "rt.sbt.main")->decision ==
                       RayTracingRenderGraphBuildDecision::rebuild &&
                   find_resource(plan, "rt.history")->preserve_history,
               "cross-frame build/update/refit decisions were incorrect"))
        return false;

    frame.camera_cut = true;
    const auto reset_plan = build_raytracing_render_graph_plan(description, frame);
    if (!check(find_resource(reset_plan, "rt.history")->reset_history &&
                   !find_resource(reset_plan, "rt.history")->preserve_history,
               "camera cut did not reset history"))
        return false;

    description.capabilities.refit_supported = false;
    const auto fallback = build_raytracing_render_graph_plan(description, ready_previous());
    return check(fallback.valid && !fallback.supported && fallback.fallback.active &&
                     fallback.mode == RayTracingRenderGraphMode::raster_fallback &&
                     fallback.code == "refit-unsupported" &&
                     find_resource(fallback, "as.blas.mesh")->decision ==
                         RayTracingRenderGraphBuildDecision::unsupported,
                 "unsupported refit did not select an explicit raster fallback");
}

bool test_invalid_lifecycle_and_fallback_modes() {
    auto invalid = complete_graph();
    invalid.resources.back().lifetime = RayTracingRenderGraphResourceLifetime::persistent;
    invalid.passes[1].depends_on = {"04.trace"};
    invalid.passes[3].depends_on = {"02.build-tlas"};
    const auto diagnostics = validate_raytracing_render_graph(invalid);
    if (!check(!diagnostics.empty(), "invalid history/cycle was accepted")) return false;
    const auto invalid_plan = build_raytracing_render_graph_plan(invalid);
    if (!check(!invalid_plan.valid && invalid_plan.mode == RayTracingRenderGraphMode::error,
               "invalid graph did not fail closed"))
        return false;

    auto unsupported = complete_graph();
    unsupported.capabilities.device_supported = false;
    const auto unsupported_plan = build_raytracing_render_graph_plan(unsupported);
    if (!check(unsupported_plan.valid && !unsupported_plan.supported &&
                   unsupported_plan.fallback.active &&
                   unsupported_plan.code == "device-unsupported",
               "unsupported device did not activate fallback"))
        return false;

    unsupported.policy.allow_raster_fallback = false;
    const auto no_fallback = build_raytracing_render_graph_plan(unsupported);
    if (!check(no_fallback.mode == RayTracingRenderGraphMode::unsupported &&
                   !no_fallback.fallback.active,
               "fallback-disabled policy was ignored"))
        return false;

    auto regressed = complete_graph();
    auto frame = ready_previous();
    frame.previous_resources[0].generation = 3U;
    const auto regressed_plan = build_raytracing_render_graph_plan(regressed, frame);
    return check(!regressed_plan.valid && regressed_plan.mode == RayTracingRenderGraphMode::error &&
                     regressed_plan.code == "render-graph.resource-generation-regressed",
                 "generation regression did not fail closed");
}

bool test_budget_boundary_and_observation() {
    auto description = complete_graph();
    const auto initial = build_raytracing_render_graph_plan(description);
    description.budget.max_resident_bytes = initial.budget.required_resident_bytes;
    description.budget.max_scratch_bytes = initial.budget.required_scratch_bytes;
    description.budget.max_history_bytes = initial.budget.required_history_bytes;
    description.budget.max_output_bytes = initial.budget.required_output_bytes;
    const auto exact = build_raytracing_render_graph_plan(description);
    if (!check(exact.budget.fits && exact.supported,
               "exact budget boundary was rejected"))
        return false;

    --description.budget.max_resident_bytes;
    const auto exceeded = build_raytracing_render_graph_plan(description);
    if (!check(!exceeded.budget.fits && exceeded.code == "budget-exceeded" &&
                   exceeded.fallback.active,
               "one-byte budget overflow did not select fallback"))
        return false;

    const auto observation = nlohmann::json::parse(
        raytracing_render_graph_observation_json(exact));
    return check(observation.at("observation") ==
                     "persistent-raytracing-render-graph" &&
                     observation.at("nativeHandlesExposed") == false &&
                     observation.at("resources").size() == 5U &&
                     observation.at("resources").at(0).contains("generation") &&
                     observation.at("resources").at(0).contains("decision"),
                 "observation JSON did not expose bounded lifecycle evidence");
}

} // namespace

int main() {
    if (!test_vocabulary_and_initial_plan()) return 1;
    if (!test_deterministic_evidence()) return 2;
    if (!test_generation_lifecycle_and_capability_fallback()) return 3;
    if (!test_invalid_lifecycle_and_fallback_modes()) return 4;
    if (!test_budget_boundary_and_observation()) return 5;
    std::cout << "raytracing_render_graph_tests: ok\n";
    return 0;
}
