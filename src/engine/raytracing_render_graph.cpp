#include "engine/raytracing_render_graph.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string bounded_text(const std::string_view value) {
    return std::string(value.substr(0U, raytracing_render_graph_max_text_bytes));
}

bool text_valid(const std::string_view value) {
    if (value.empty() || value.size() > raytracing_render_graph_max_text_bytes)
        return false;
    for (const auto character : value) {
        if (static_cast<unsigned char>(character) < 0x20U ||
            static_cast<unsigned char>(character) == 0x7fU)
            return false;
    }
    return true;
}

bool stable_id_valid(const std::string_view value) {
    if (!text_valid(value)) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '/' || byte == '\\' || byte == ' ' || byte == '\t')
            return false;
    }
    return true;
}

void add_diagnostic(std::vector<RayTracingRenderGraphDiagnostic>& diagnostics,
                    std::string code, std::string path, std::string message) {
    if (diagnostics.size() >= raytracing_render_graph_max_diagnostics)
        return;
    diagnostics.push_back(RayTracingRenderGraphDiagnostic{
        .code = bounded_text(code),
        .path = bounded_text(path),
        .message = bounded_text(message),
    });
}

void sort_diagnostics(
    std::vector<RayTracingRenderGraphDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const RayTracingRenderGraphDiagnostic& left,
                 const RayTracingRenderGraphDiagnostic& right) {
                  if (left.code != right.code) return left.code < right.code;
                  if (left.path != right.path) return left.path < right.path;
                  return left.message < right.message;
              });
}

bool checked_add(const std::uint64_t left, const std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    output = left + right;
    return true;
}

bool checked_mul(const std::uint64_t left, const std::uint64_t right,
                 std::uint64_t& output) noexcept {
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    output = left * right;
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

std::string path_for(const std::string_view root, const std::size_t index,
                     const std::string_view field) {
    return std::string(root) + "[" + std::to_string(index) + "]." +
           std::string(field);
}

Json policy_json(const RayTracingRenderGraphPolicy& policy) {
    return Json{
        {"enabled", policy.enabled},
        {"allowRasterFallback", policy.allow_raster_fallback},
        {"requireHistory", policy.require_history},
    };
}

Json budget_json(const RayTracingRenderGraphBudget& budget) {
    return Json{
        {"maxResidentBytes", budget.max_resident_bytes},
        {"maxScratchBytes", budget.max_scratch_bytes},
        {"maxHistoryBytes", budget.max_history_bytes},
        {"maxOutputBytes", budget.max_output_bytes},
    };
}

Json capabilities_json(const RayTracingRenderGraphCapabilities& capabilities) {
    return Json{
        {"deviceSupported", capabilities.device_supported},
        {"accelerationStructureSupported",
         capabilities.acceleration_structure_supported},
        {"rayTracingPipelineSupported",
         capabilities.ray_tracing_pipeline_supported},
        {"updateSupported", capabilities.update_supported},
        {"refitSupported", capabilities.refit_supported},
    };
}

Json resource_json(const RayTracingRenderGraphResource& resource) {
    return Json{
        {"id", bounded_text(resource.id)},
        {"kind", raytracing_render_graph_resource_kind_name(resource.kind)},
        {"lifetime",
         raytracing_render_graph_resource_lifetime_name(resource.lifetime)},
        {"format", bounded_text(resource.format)},
        {"dimension", bounded_text(resource.dimension)},
        {"width", resource.width},
        {"height", resource.height},
        {"depth", resource.depth},
        {"layers", resource.layers},
        {"bytes", resource.bytes},
        {"scratchBytes", resource.scratch_bytes},
        {"historyLength", resource.history_length},
        {"generation", resource.generation},
        {"dirty", resource.dirty},
        {"topologyChanged", resource.topology_changed},
        {"refitRequested", resource.refit_requested},
    };
}

Json sorted_string_array(const std::vector<std::string>& values) {
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    Json result = Json::array();
    for (const auto& value : sorted)
        result.push_back(bounded_text(value));
    return result;
}

Json pass_json(const RayTracingRenderGraphPass& pass) {
    return Json{
        {"id", bounded_text(pass.id)},
        {"kind", raytracing_render_graph_pass_kind_name(pass.kind)},
        {"reads", sorted_string_array(pass.reads)},
        {"writes", sorted_string_array(pass.writes)},
        {"dependsOn", sorted_string_array(pass.depends_on)},
        {"readModifyWrite", pass.read_modify_write},
        {"enabled", pass.enabled},
    };
}

Json diagnostic_json(const RayTracingRenderGraphDiagnostic& diagnostic) {
    return Json{
        {"code", bounded_text(diagnostic.code)},
        {"path", bounded_text(diagnostic.path)},
        {"message", bounded_text(diagnostic.message)},
    };
}

Json budget_report_json(const RayTracingRenderGraphBudgetReport& report) {
    return Json{
        {"requiredResidentBytes", report.required_resident_bytes},
        {"requiredScratchBytes", report.required_scratch_bytes},
        {"requiredHistoryBytes", report.required_history_bytes},
        {"requiredOutputBytes", report.required_output_bytes},
        {"availableResidentBytes", report.available_resident_bytes},
        {"availableScratchBytes", report.available_scratch_bytes},
        {"availableHistoryBytes", report.available_history_bytes},
        {"availableOutputBytes", report.available_output_bytes},
        {"fits", report.fits},
    };
}

Json fallback_json(const RayTracingRenderGraphFallback& fallback) {
    return Json{
        {"active", fallback.active},
        {"mode", bounded_text(fallback.mode)},
        {"reason", bounded_text(fallback.reason)},
    };
}

Json resource_plan_json(const RayTracingRenderGraphResourcePlan& resource) {
    return Json{
        {"id", bounded_text(resource.id)},
        {"kind", raytracing_render_graph_resource_kind_name(resource.kind)},
        {"lifetime",
         raytracing_render_graph_resource_lifetime_name(resource.lifetime)},
        {"generation", resource.generation},
        {"previousGeneration", resource.previous_generation},
        {"decision",
         raytracing_render_graph_build_decision_name(resource.decision)},
        {"preserveHistory", resource.preserve_history},
        {"resetHistory", resource.reset_history},
    };
}

Json pass_plan_json(const RayTracingRenderGraphPassPlan& pass) {
    return Json{
        {"id", bounded_text(pass.id)},
        {"kind", raytracing_render_graph_pass_kind_name(pass.kind)},
        {"executionIndex", pass.execution_index},
        {"selected", pass.selected},
        {"enabled", pass.enabled},
    };
}

std::uint64_t fnv1a(const std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16U, '0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned int>(
            (result.size() - 1U - index) * 4U);
        result[index] = digits[(value >> shift) & 0x0fU];
    }
    return result;
}

bool is_blas_pass(const RayTracingRenderGraphPassKind kind) noexcept {
    return kind == RayTracingRenderGraphPassKind::build_blas ||
           kind == RayTracingRenderGraphPassKind::update_blas ||
           kind == RayTracingRenderGraphPassKind::refit_blas;
}

bool is_tlas_pass(const RayTracingRenderGraphPassKind kind) noexcept {
    return kind == RayTracingRenderGraphPassKind::build_tlas ||
           kind == RayTracingRenderGraphPassKind::update_tlas ||
           kind == RayTracingRenderGraphPassKind::refit_tlas;
}

bool is_history_reset_required(const RayTracingRenderGraphResource& resource,
                               const RayTracingRenderGraphFrameState& frame,
                               const RayTracingRenderGraphPreviousResource*
                                   previous) noexcept {
    if (resource.kind != RayTracingRenderGraphResourceKind::history)
        return false;
    if (previous == nullptr || !previous->ready)
        return true;
    if (!frame.previous_frame_valid || !frame.history_valid || frame.camera_cut ||
        frame.extent_changed)
        return true;
    if (frame.graph_generation != frame.previous_graph_generation)
        return true;
    return resource.generation != previous->generation || resource.dirty ||
           resource.topology_changed;
}

std::vector<std::size_t> topological_order(
    const RayTracingRenderGraphDescription& description,
    const std::unordered_map<std::string, std::size_t>& pass_indices) {
    std::vector<std::vector<std::size_t>> outgoing(description.passes.size());
    std::vector<std::size_t> indegree(description.passes.size(), 0U);
    for (std::size_t index = 0U; index < description.passes.size(); ++index) {
        for (const auto& dependency : description.passes[index].depends_on) {
            const auto dependency_it = pass_indices.find(dependency);
            if (dependency_it == pass_indices.end()) continue;
            outgoing[dependency_it->second].push_back(index);
            ++indegree[index];
        }
    }

    // A set makes independent passes deterministic by stable ID, regardless
    // of how the author ordered the source records.
    std::set<std::pair<std::string, std::size_t>> ready;
    for (std::size_t index = 0U; index < indegree.size(); ++index)
        if (indegree[index] == 0U)
            ready.emplace(description.passes[index].id, index);

    std::vector<std::size_t> result;
    result.reserve(description.passes.size());
    while (!ready.empty()) {
        const auto [id, index] = *ready.begin();
        (void)id;
        ready.erase(ready.begin());
        result.push_back(index);
        for (const auto successor : outgoing[index]) {
            if (--indegree[successor] == 0U)
                ready.emplace(description.passes[successor].id, successor);
        }
    }
    return result;
}

const RayTracingRenderGraphPreviousResource* previous_for(
    const std::unordered_map<std::string, RayTracingRenderGraphPreviousResource>&
        previous_resources,
    const std::string_view id) {
    const auto found = previous_resources.find(std::string(id));
    return found == previous_resources.end() ? nullptr : &found->second;
}

void sort_resource_plans(std::vector<RayTracingRenderGraphResourcePlan>& plans) {
    std::sort(plans.begin(), plans.end(),
              [](const auto& left, const auto& right) {
                  return left.id < right.id;
              });
}

void sort_pass_plans(std::vector<RayTracingRenderGraphPassPlan>& plans) {
    std::sort(plans.begin(), plans.end(),
              [](const auto& left, const auto& right) {
                  if (left.execution_index != right.execution_index)
                      return left.execution_index < right.execution_index;
                  return left.id < right.id;
              });
}

} // namespace

std::string_view raytracing_render_graph_resource_kind_name(
    const RayTracingRenderGraphResourceKind kind) noexcept {
    switch (kind) {
    case RayTracingRenderGraphResourceKind::blas: return "blas";
    case RayTracingRenderGraphResourceKind::tlas: return "tlas";
    case RayTracingRenderGraphResourceKind::sbt: return "sbt";
    case RayTracingRenderGraphResourceKind::output: return "output";
    case RayTracingRenderGraphResourceKind::history: return "history";
    }
    return "unknown";
}

std::string_view raytracing_render_graph_resource_lifetime_name(
    const RayTracingRenderGraphResourceLifetime lifetime) noexcept {
    switch (lifetime) {
    case RayTracingRenderGraphResourceLifetime::persistent: return "persistent";
    case RayTracingRenderGraphResourceLifetime::transient: return "transient";
    case RayTracingRenderGraphResourceLifetime::history: return "history";
    }
    return "unknown";
}

std::string_view raytracing_render_graph_pass_kind_name(
    const RayTracingRenderGraphPassKind kind) noexcept {
    switch (kind) {
    case RayTracingRenderGraphPassKind::build_blas: return "build-blas";
    case RayTracingRenderGraphPassKind::update_blas: return "update-blas";
    case RayTracingRenderGraphPassKind::refit_blas: return "refit-blas";
    case RayTracingRenderGraphPassKind::build_tlas: return "build-tlas";
    case RayTracingRenderGraphPassKind::update_tlas: return "update-tlas";
    case RayTracingRenderGraphPassKind::refit_tlas: return "refit-tlas";
    case RayTracingRenderGraphPassKind::build_sbt: return "build-sbt";
    case RayTracingRenderGraphPassKind::trace: return "trace";
    case RayTracingRenderGraphPassKind::denoise: return "denoise";
    case RayTracingRenderGraphPassKind::resolve: return "resolve";
    case RayTracingRenderGraphPassKind::clear_history: return "clear-history";
    case RayTracingRenderGraphPassKind::raster_fallback:
        return "raster-fallback";
    }
    return "unknown";
}

std::string_view raytracing_render_graph_build_decision_name(
    const RayTracingRenderGraphBuildDecision decision) noexcept {
    switch (decision) {
    case RayTracingRenderGraphBuildDecision::none: return "none";
    case RayTracingRenderGraphBuildDecision::build: return "build";
    case RayTracingRenderGraphBuildDecision::update: return "update";
    case RayTracingRenderGraphBuildDecision::refit: return "refit";
    case RayTracingRenderGraphBuildDecision::rebuild: return "rebuild";
    case RayTracingRenderGraphBuildDecision::clear: return "clear";
    case RayTracingRenderGraphBuildDecision::unsupported: return "unsupported";
    }
    return "unknown";
}

std::string_view raytracing_render_graph_mode_name(
    const RayTracingRenderGraphMode mode) noexcept {
    switch (mode) {
    case RayTracingRenderGraphMode::ray_tracing: return "ray-tracing";
    case RayTracingRenderGraphMode::raster_fallback: return "raster-fallback";
    case RayTracingRenderGraphMode::unsupported: return "unsupported";
    case RayTracingRenderGraphMode::error: return "error";
    }
    return "unknown";
}

std::vector<RayTracingRenderGraphDiagnostic> validate_raytracing_render_graph(
    const RayTracingRenderGraphDescription& description) {
    std::vector<RayTracingRenderGraphDiagnostic> diagnostics;
    if (description.schema != raytracing_render_graph_schema) {
        add_diagnostic(diagnostics, "render-graph.schema", "schema",
                       "render graph schema does not match the supported contract");
    }
    if (!stable_id_valid(description.graph_id)) {
        add_diagnostic(diagnostics, "render-graph.identity", "graphId",
                       "graphId must be a non-empty printable stable ID");
    }
    if (description.graph_generation == 0U) {
        add_diagnostic(diagnostics, "render-graph.generation", "graphGeneration",
                       "graphGeneration must be greater than zero");
    }
    const auto check_budget = [&](const std::uint64_t value,
                                  const std::string_view field) {
        if (value > raytracing_render_graph_max_resource_bytes) {
            add_diagnostic(diagnostics, "render-graph.budget", std::string(field),
                           "budget exceeds the contract safety bound");
        }
    };
    check_budget(description.budget.max_resident_bytes, "budget.maxResidentBytes");
    check_budget(description.budget.max_scratch_bytes, "budget.maxScratchBytes");
    check_budget(description.budget.max_history_bytes, "budget.maxHistoryBytes");
    check_budget(description.budget.max_output_bytes, "budget.maxOutputBytes");

    if (description.resources.size() > raytracing_render_graph_max_resources) {
        add_diagnostic(diagnostics, "render-graph.count", "resources",
                       "resource count exceeds the contract safety bound");
    }
    if (description.passes.size() > raytracing_render_graph_max_passes) {
        add_diagnostic(diagnostics, "render-graph.count", "passes",
                       "pass count exceeds the contract safety bound");
    }

    std::unordered_map<std::string, std::size_t> resource_indices;
    resource_indices.reserve(description.resources.size());
    std::size_t history_count = 0U;
    for (std::size_t index = 0U; index < description.resources.size(); ++index) {
        const auto& resource = description.resources[index];
        const auto path = [&](const std::string_view field) {
            return path_for("resources", index, field);
        };
        if (!stable_id_valid(resource.id)) {
            add_diagnostic(diagnostics, "render-graph.resource-identity",
                           path("id"),
                           "resource ID must be a non-empty printable stable ID");
        } else if (!resource_indices.emplace(resource.id, index).second) {
            add_diagnostic(diagnostics, "render-graph.resource-duplicate", path("id"),
                           "resource IDs must be unique");
        }
        if (!valid_resource_kind(resource.kind)) {
            add_diagnostic(diagnostics, "render-graph.resource-kind", path("kind"),
                           "resource kind is not supported by this contract");
        }
        if (!valid_lifetime(resource.lifetime)) {
            add_diagnostic(diagnostics, "render-graph.resource-lifetime",
                           path("lifetime"),
                           "resource lifetime is not supported by this contract");
        }
        if (!text_valid(resource.format))
            add_diagnostic(diagnostics, "render-graph.resource-format", path("format"),
                           "resource format must be printable and bounded");
        if (!text_valid(resource.dimension))
            add_diagnostic(diagnostics, "render-graph.resource-dimension",
                           path("dimension"),
                           "resource dimension must be printable and bounded");
        if (resource.width == 0U || resource.width > raytracing_render_graph_max_extent ||
            resource.height == 0U ||
                resource.height > raytracing_render_graph_max_extent ||
            resource.depth == 0U || resource.depth > raytracing_render_graph_max_extent ||
            resource.layers == 0U || resource.layers > raytracing_render_graph_max_layers) {
            add_diagnostic(diagnostics, "render-graph.resource-extent", path("extent"),
                           "resource extent is outside the bounded contract range");
        }
        if (resource.bytes > raytracing_render_graph_max_resource_bytes)
            add_diagnostic(diagnostics, "render-graph.resource-budget", path("bytes"),
                           "resource bytes exceed the contract safety bound");
        if (resource.scratch_bytes > raytracing_render_graph_max_resource_bytes)
            add_diagnostic(diagnostics, "render-graph.resource-budget",
                           path("scratchBytes"),
                           "resource scratch bytes exceed the contract safety bound");
        if (resource.generation == 0U)
            add_diagnostic(diagnostics, "render-graph.resource-generation",
                           path("generation"),
                           "resource generation must be greater than zero");
        if (resource.kind == RayTracingRenderGraphResourceKind::history) {
            ++history_count;
            if (resource.lifetime != RayTracingRenderGraphResourceLifetime::history)
                add_diagnostic(diagnostics, "render-graph.history-lifetime",
                               path("lifetime"),
                               "history resources must use history lifetime");
            if (resource.history_length < 2U ||
                resource.history_length > raytracing_render_graph_max_history_length)
                add_diagnostic(diagnostics, "render-graph.history-length",
                               path("historyLength"),
                               "historyLength must be between two and the safety bound");
        } else {
            if (resource.lifetime == RayTracingRenderGraphResourceLifetime::history)
                add_diagnostic(diagnostics, "render-graph.history-kind",
                               path("kind"),
                               "only history resources may use history lifetime");
            if (resource.history_length != 1U)
                add_diagnostic(diagnostics, "render-graph.history-length",
                               path("historyLength"),
                               "non-history resources must use historyLength one");
        }
        if ((resource.kind == RayTracingRenderGraphResourceKind::blas ||
             resource.kind == RayTracingRenderGraphResourceKind::tlas ||
             resource.kind == RayTracingRenderGraphResourceKind::sbt) &&
            resource.lifetime != RayTracingRenderGraphResourceLifetime::persistent) {
            add_diagnostic(diagnostics, "render-graph.acceleration-lifetime",
                           path("lifetime"),
                           "BLAS, TLAS and SBT resources must be persistent");
        }
    }
    if (description.policy.require_history && history_count == 0U) {
        add_diagnostic(diagnostics, "render-graph.history-required", "policy.requireHistory",
                       "policy requires at least one history resource");
    }

    std::unordered_map<std::string, std::size_t> pass_indices;
    pass_indices.reserve(description.passes.size());
    // Register every pass identity before checking dependencies.  Dependency
    // order is not required to match authoring order; a reversed graph must
    // validate exactly like the original ordering.
    for (std::size_t index = 0U; index < description.passes.size(); ++index) {
        const auto& pass = description.passes[index];
        const auto path = path_for("passes", index, "id");
        if (!stable_id_valid(pass.id)) {
            add_diagnostic(diagnostics, "render-graph.pass-identity", path,
                           "pass ID must be a non-empty printable stable ID");
        } else if (!pass_indices.emplace(pass.id, index).second) {
            add_diagnostic(diagnostics, "render-graph.pass-duplicate", path,
                           "pass IDs must be unique");
        }
    }
    for (std::size_t index = 0U; index < description.passes.size(); ++index) {
        const auto& pass = description.passes[index];
        const auto path = [&](const std::string_view field) {
            return path_for("passes", index, field);
        };
        if (!valid_pass_kind(pass.kind))
            add_diagnostic(diagnostics, "render-graph.pass-kind", path("kind"),
                           "pass kind is not supported by this contract");

        const auto check_refs = [&](const std::vector<std::string>& refs,
                                    const std::string_view field) {
            std::unordered_set<std::string> seen;
            for (std::size_t ref_index = 0U; ref_index < refs.size(); ++ref_index) {
                const auto& ref = refs[ref_index];
                if (resource_indices.find(ref) == resource_indices.end()) {
                    add_diagnostic(diagnostics, "render-graph.resource-reference",
                                   path_for(path(field), ref_index, "id"),
                                   "pass references an unknown resource ID");
                }
                if (!seen.emplace(ref).second)
                    add_diagnostic(diagnostics, "render-graph.resource-reference-duplicate",
                                   path_for(path(field), ref_index, "id"),
                                   "a pass reference list cannot contain duplicates");
            }
        };
        check_refs(pass.reads, "reads");
        check_refs(pass.writes, "writes");
        if (!pass.read_modify_write) {
            for (const auto& read : pass.reads) {
                if (std::find(pass.writes.begin(), pass.writes.end(), read) !=
                    pass.writes.end()) {
                    add_diagnostic(diagnostics, "render-graph.read-write-alias",
                                   path("writes"),
                                   "a resource read and written by a pass requires readModifyWrite");
                    break;
                }
            }
        }
        std::unordered_set<std::string> dependency_set;
        for (std::size_t dependency_index = 0U;
             dependency_index < pass.depends_on.size(); ++dependency_index) {
            const auto& dependency = pass.depends_on[dependency_index];
            if (!dependency_set.emplace(dependency).second)
                add_diagnostic(diagnostics, "render-graph.pass-dependency-duplicate",
                               path_for(path("dependsOn"), dependency_index, "id"),
                               "a pass dependency list cannot contain duplicates");
            if (pass_indices.find(dependency) == pass_indices.end())
                add_diagnostic(diagnostics, "render-graph.pass-dependency",
                               path_for(path("dependsOn"), dependency_index, "id"),
                               "pass depends on an unknown pass ID");
            if (dependency == pass.id)
                add_diagnostic(diagnostics, "render-graph.pass-cycle", path("dependsOn"),
                               "a pass cannot depend on itself");
        }

        if (!pass.enabled) continue;
        const auto resource_of = [&](const std::string& id)
            -> const RayTracingRenderGraphResource* {
            const auto found = resource_indices.find(id);
            return found == resource_indices.end()
                       ? nullptr
                       : &description.resources[found->second];
        };
        const auto has_kind = [&](const std::vector<std::string>& refs,
                                  const RayTracingRenderGraphResourceKind kind) {
            return std::any_of(refs.begin(), refs.end(), [&](const auto& id) {
                const auto* resource = resource_of(id);
                return resource != nullptr && resource->kind == kind;
            });
        };
        const auto all_kind = [&](const std::vector<std::string>& refs,
                                  const RayTracingRenderGraphResourceKind kind) {
            return !refs.empty() && std::all_of(refs.begin(), refs.end(), [&](const auto& id) {
                const auto* resource = resource_of(id);
                return resource != nullptr && resource->kind == kind;
            });
        };
        const auto require_writes = [&](const std::string_view code,
                                        const std::string_view message) {
            if (pass.writes.empty())
                add_diagnostic(diagnostics, std::string(code), path("writes"),
                               std::string(message));
        };
        if (is_blas_pass(pass.kind)) {
            require_writes("render-graph.pass-shape", "BLAS pass must write a BLAS resource");
            if (!all_kind(pass.writes, RayTracingRenderGraphResourceKind::blas))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "BLAS pass writes must all be BLAS resources");
        } else if (is_tlas_pass(pass.kind)) {
            require_writes("render-graph.pass-shape", "TLAS pass must write a TLAS resource");
            if (!all_kind(pass.writes, RayTracingRenderGraphResourceKind::tlas))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "TLAS pass writes must all be TLAS resources");
        } else if (pass.kind == RayTracingRenderGraphPassKind::build_sbt) {
            require_writes("render-graph.pass-shape", "SBT pass must write an SBT resource");
            if (!all_kind(pass.writes, RayTracingRenderGraphResourceKind::sbt))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "SBT pass writes must all be SBT resources");
        } else if (pass.kind == RayTracingRenderGraphPassKind::trace) {
            if (!has_kind(pass.reads, RayTracingRenderGraphResourceKind::tlas) ||
                !has_kind(pass.reads, RayTracingRenderGraphResourceKind::sbt))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("reads"),
                               "trace pass must read at least one TLAS and one SBT");
            if (!has_kind(pass.writes, RayTracingRenderGraphResourceKind::output))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "trace pass must write at least one output");
        } else if (pass.kind == RayTracingRenderGraphPassKind::denoise) {
            if (!has_kind(pass.reads, RayTracingRenderGraphResourceKind::output))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("reads"),
                               "denoise pass must read an output");
            if (!has_kind(pass.writes, RayTracingRenderGraphResourceKind::output) &&
                !has_kind(pass.writes, RayTracingRenderGraphResourceKind::history))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "denoise pass must write output or history");
        } else if (pass.kind == RayTracingRenderGraphPassKind::resolve) {
            require_writes("render-graph.pass-shape", "resolve pass must write an output");
            if (!all_kind(pass.writes, RayTracingRenderGraphResourceKind::output))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "resolve pass writes must all be output resources");
        } else if (pass.kind == RayTracingRenderGraphPassKind::clear_history) {
            require_writes("render-graph.pass-shape", "clear-history pass must write history");
            if (!all_kind(pass.writes, RayTracingRenderGraphResourceKind::history))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "clear-history pass writes must all be history resources");
        } else if (pass.kind == RayTracingRenderGraphPassKind::raster_fallback) {
            require_writes("render-graph.pass-shape", "raster fallback must write an output");
            if (!all_kind(pass.writes, RayTracingRenderGraphResourceKind::output))
                add_diagnostic(diagnostics, "render-graph.pass-shape", path("writes"),
                               "raster fallback writes must all be output resources");
        }
    }

    if (pass_indices.size() == description.passes.size()) {
        const auto order = topological_order(description, pass_indices);
        if (order.size() != description.passes.size()) {
            add_diagnostic(diagnostics, "render-graph.pass-cycle", "passes",
                           "pass dependencies must form an acyclic graph");
        }
    }
    sort_diagnostics(diagnostics);
    return diagnostics;
}

RayTracingRenderGraphPlan build_raytracing_render_graph_plan(
    const RayTracingRenderGraphDescription& description,
    const RayTracingRenderGraphFrameState& frame) {
    RayTracingRenderGraphPlan plan;
    plan.schema = std::string(raytracing_render_graph_schema);
    plan.graph_id = bounded_text(description.graph_id);
    plan.frame_generation = frame.frame_generation;
    plan.graph_generation = description.graph_generation;
    plan.fallback = RayTracingRenderGraphFallback{};

    plan.diagnostics = validate_raytracing_render_graph(description);
    if (frame.frame_generation == 0U) {
        add_diagnostic(plan.diagnostics, "render-graph.frame-generation", "frameGeneration",
                       "frameGeneration must be greater than zero");
    }
    if (frame.graph_generation == 0U || frame.previous_graph_generation == 0U) {
        add_diagnostic(plan.diagnostics, "render-graph.frame-generation", "frameGraphGeneration",
                       "frame graph generations must be greater than zero");
    }

    std::unordered_map<std::string, RayTracingRenderGraphPreviousResource>
        previous_resources;
    previous_resources.reserve(frame.previous_resources.size());
    for (std::size_t index = 0U; index < frame.previous_resources.size(); ++index) {
        const auto& previous = frame.previous_resources[index];
        if (!stable_id_valid(previous.id)) {
            add_diagnostic(plan.diagnostics, "render-graph.previous-resource-identity",
                           "previousResources[" + std::to_string(index) + "].id",
                           "previous resource ID must be a stable ID");
            continue;
        }
        if (previous.generation == 0U) {
            add_diagnostic(plan.diagnostics, "render-graph.previous-resource-generation",
                           "previousResources[" + std::to_string(index) + "].generation",
                           "previous resource generation must be greater than zero");
        }
        if (!valid_resource_kind(previous.kind)) {
            add_diagnostic(plan.diagnostics, "render-graph.previous-resource-kind",
                           "previousResources[" + std::to_string(index) + "].kind",
                           "previous resource kind is not supported");
        }
        if (!previous_resources.emplace(previous.id, previous).second) {
            add_diagnostic(plan.diagnostics, "render-graph.previous-resource-duplicate",
                           "previousResources[" + std::to_string(index) + "].id",
                           "previous resource IDs must be unique");
        }
    }

    std::uint64_t required_resident = 0U;
    std::uint64_t required_scratch = 0U;
    std::uint64_t required_history = 0U;
    std::uint64_t required_output = 0U;
    bool budget_arithmetic_valid = true;
    for (const auto& resource : description.resources) {
        std::uint64_t resident = resource.bytes;
        if (resource.kind == RayTracingRenderGraphResourceKind::history) {
            if (!checked_mul(resource.bytes, resource.history_length, resident))
                budget_arithmetic_valid = false;
        }
        if (!checked_add(required_resident, resident, required_resident))
            budget_arithmetic_valid = false;
        if (!checked_add(required_scratch, resource.scratch_bytes, required_scratch))
            budget_arithmetic_valid = false;
        if (resource.kind == RayTracingRenderGraphResourceKind::history &&
            !checked_add(required_history, resident, required_history))
            budget_arithmetic_valid = false;
        if (resource.kind == RayTracingRenderGraphResourceKind::output &&
            !checked_add(required_output, resource.bytes, required_output))
            budget_arithmetic_valid = false;
    }
    plan.budget.required_resident_bytes = required_resident;
    plan.budget.required_scratch_bytes = required_scratch;
    plan.budget.required_history_bytes = required_history;
    plan.budget.required_output_bytes = required_output;
    plan.budget.available_resident_bytes = description.budget.max_resident_bytes;
    plan.budget.available_scratch_bytes = description.budget.max_scratch_bytes;
    plan.budget.available_history_bytes = description.budget.max_history_bytes;
    plan.budget.available_output_bytes = description.budget.max_output_bytes;
    plan.budget.fits =
        budget_arithmetic_valid && required_resident <= plan.budget.available_resident_bytes &&
        required_scratch <= plan.budget.available_scratch_bytes &&
        required_history <= plan.budget.available_history_bytes &&
        required_output <= plan.budget.available_output_bytes;

    // Build resource plans by stable ID, making observation order independent
    // from authoring order.  Validation will reject malformed IDs, but doing
    // this only for valid records keeps error evidence useful as well.
    auto resource_indices = std::vector<std::size_t>{};
    resource_indices.reserve(description.resources.size());
    for (std::size_t index = 0U; index < description.resources.size(); ++index)
        resource_indices.push_back(index);
    std::sort(resource_indices.begin(), resource_indices.end(), [&](const auto left,
                                                                    const auto right) {
        if (description.resources[left].id != description.resources[right].id)
            return description.resources[left].id < description.resources[right].id;
        return left < right;
    });
    for (const auto index : resource_indices) {
        const auto& resource = description.resources[index];
        RayTracingRenderGraphResourcePlan resource_plan;
        resource_plan.id = bounded_text(resource.id);
        resource_plan.kind = resource.kind;
        resource_plan.lifetime = resource.lifetime;
        resource_plan.generation = resource.generation;
        const auto* previous = previous_for(previous_resources, resource.id);
        resource_plan.previous_generation = previous == nullptr ? 0U : previous->generation;
        if (previous != nullptr && previous->kind != resource.kind) {
            add_diagnostic(plan.diagnostics, "render-graph.resource-kind-changed",
                           "resources." + resource.id + ".kind",
                           "a stable resource ID cannot change kind across frames");
        }
        if (previous != nullptr && resource.generation < previous->generation) {
            add_diagnostic(plan.diagnostics, "render-graph.resource-generation-regressed",
                           "resources." + resource.id + ".generation",
                           "resource generation regressed relative to the previous frame");
        }

        if (resource.kind == RayTracingRenderGraphResourceKind::history) {
            const bool reset = is_history_reset_required(resource, frame, previous);
            resource_plan.reset_history = reset;
            resource_plan.preserve_history = !reset;
            resource_plan.decision = reset
                                         ? RayTracingRenderGraphBuildDecision::clear
                                         : RayTracingRenderGraphBuildDecision::none;
        } else if (resource.kind == RayTracingRenderGraphResourceKind::blas ||
                   resource.kind == RayTracingRenderGraphResourceKind::tlas) {
            const bool previous_ready = previous != nullptr && previous->ready;
            const bool changed = !previous_ready || resource.dirty ||
                                  resource.generation != resource_plan.previous_generation;
            if (changed) {
                if (!previous_ready)
                    resource_plan.decision = RayTracingRenderGraphBuildDecision::build;
                else if (resource.topology_changed)
                    resource_plan.decision = RayTracingRenderGraphBuildDecision::rebuild;
                else if (resource.refit_requested)
                    resource_plan.decision = RayTracingRenderGraphBuildDecision::refit;
                else
                    resource_plan.decision = RayTracingRenderGraphBuildDecision::update;
            }
        } else if (resource.kind == RayTracingRenderGraphResourceKind::sbt) {
            const bool previous_ready = previous != nullptr && previous->ready;
            const bool changed = !previous_ready || resource.dirty ||
                                  resource.generation != resource_plan.previous_generation;
            if (changed) {
                resource_plan.decision = previous_ready
                                             ? RayTracingRenderGraphBuildDecision::rebuild
                                             : RayTracingRenderGraphBuildDecision::build;
            }
        } else if (resource.kind == RayTracingRenderGraphResourceKind::output) {
            const bool previous_ready = previous != nullptr && previous->ready;
            if (!previous_ready || resource.dirty ||
                resource.generation != resource_plan.previous_generation)
                resource_plan.decision = RayTracingRenderGraphBuildDecision::clear;
        }
        plan.resources.push_back(std::move(resource_plan));
    }
    sort_resource_plans(plan.resources);

    std::unordered_map<std::string, std::size_t> pass_indices;
    pass_indices.reserve(description.passes.size());
    for (std::size_t index = 0U; index < description.passes.size(); ++index)
        pass_indices.emplace(description.passes[index].id, index);
    const auto order = topological_order(description, pass_indices);
    plan.execution_order.reserve(order.size());
    for (const auto index : order)
        plan.execution_order.push_back(bounded_text(description.passes[index].id));

    if (!plan.diagnostics.empty()) {
        sort_diagnostics(plan.diagnostics);
        plan.valid = false;
        plan.supported = false;
        plan.mode = RayTracingRenderGraphMode::error;
        plan.code = plan.diagnostics.front().code;
        plan.detail = plan.diagnostics.front().message;
        return plan;
    }

    plan.valid = true;
    plan.code = "ready";
    plan.detail = "persistent ray-tracing graph is ready for backend translation";

    const auto set_unsupported = [&](const std::string_view code,
                                     const std::string_view detail) {
        plan.code = std::string(code);
        plan.detail = std::string(detail);
        if (description.policy.allow_raster_fallback) {
            plan.mode = RayTracingRenderGraphMode::raster_fallback;
            plan.fallback.active = true;
            plan.fallback.reason = std::string(code);
        } else {
            plan.mode = RayTracingRenderGraphMode::unsupported;
            plan.fallback.active = false;
            plan.fallback.reason = std::string(code);
        }
        plan.supported = false;
    };

    if (!description.policy.enabled) {
        set_unsupported("disabled", "ray tracing policy is disabled");
    } else if (!description.capabilities.device_supported) {
        set_unsupported("device-unsupported",
                        "backend device does not support this graph contract");
    } else if (!description.capabilities.acceleration_structure_supported) {
        set_unsupported("acceleration-structure-unsupported",
                        "backend lacks acceleration-structure support");
    } else if (!description.capabilities.ray_tracing_pipeline_supported) {
        set_unsupported("ray-tracing-pipeline-unsupported",
                        "backend lacks a ray-tracing pipeline");
    } else if (!plan.budget.fits) {
        set_unsupported("budget-exceeded",
                        "graph resource requirements exceed the declared budget");
    } else {
        bool update_unsupported = false;
        bool refit_unsupported = false;
        for (const auto& resource : plan.resources) {
            if (resource.decision == RayTracingRenderGraphBuildDecision::update &&
                !description.capabilities.update_supported)
                update_unsupported = true;
            if (resource.decision == RayTracingRenderGraphBuildDecision::refit &&
                !description.capabilities.refit_supported)
                refit_unsupported = true;
        }
        if (refit_unsupported) {
            set_unsupported("refit-unsupported",
                            "graph requests BLAS/TLAS refit but backend cannot refit");
            for (auto& resource : plan.resources)
                if (resource.decision == RayTracingRenderGraphBuildDecision::refit)
                    resource.decision = RayTracingRenderGraphBuildDecision::unsupported;
        } else if (update_unsupported) {
            set_unsupported("update-unsupported",
                            "graph requests BLAS/TLAS update but backend cannot update");
            for (auto& resource : plan.resources)
                if (resource.decision == RayTracingRenderGraphBuildDecision::update)
                    resource.decision = RayTracingRenderGraphBuildDecision::unsupported;
        } else {
            plan.mode = RayTracingRenderGraphMode::ray_tracing;
            plan.supported = true;
        }
    }

    if (plan.fallback.active)
        plan.fallback.mode = "raster-pbr";
    const bool ray_tracing_mode = plan.mode == RayTracingRenderGraphMode::ray_tracing;
    plan.passes.reserve(description.passes.size());
    for (std::size_t execution_index = 0U; execution_index < order.size();
         ++execution_index) {
        const auto& pass = description.passes[order[execution_index]];
        const bool selected =
            pass.enabled && (ray_tracing_mode
                                 ? pass.kind != RayTracingRenderGraphPassKind::raster_fallback
                                 : pass.kind == RayTracingRenderGraphPassKind::raster_fallback);
        plan.passes.push_back(RayTracingRenderGraphPassPlan{
            .id = bounded_text(pass.id),
            .kind = pass.kind,
            .execution_index = execution_index,
            .selected = selected,
            .enabled = pass.enabled,
        });
    }
    sort_pass_plans(plan.passes);
    sort_diagnostics(plan.diagnostics);
    return plan;
}

std::string raytracing_render_graph_canonical_description(
    const RayTracingRenderGraphDescription& description) {
    std::vector<std::size_t> resource_order;
    resource_order.reserve(description.resources.size());
    for (std::size_t index = 0U; index < description.resources.size(); ++index)
        resource_order.push_back(index);
    std::sort(resource_order.begin(), resource_order.end(), [&](const auto left,
                                                                 const auto right) {
        if (description.resources[left].id != description.resources[right].id)
            return description.resources[left].id < description.resources[right].id;
        return left < right;
    });
    Json resources = Json::array();
    for (const auto index : resource_order)
        resources.push_back(resource_json(description.resources[index]));

    std::vector<std::size_t> pass_order;
    pass_order.reserve(description.passes.size());
    for (std::size_t index = 0U; index < description.passes.size(); ++index)
        pass_order.push_back(index);
    std::sort(pass_order.begin(), pass_order.end(), [&](const auto left,
                                                        const auto right) {
        if (description.passes[left].id != description.passes[right].id)
            return description.passes[left].id < description.passes[right].id;
        return left < right;
    });
    Json passes = Json::array();
    for (const auto index : pass_order)
        passes.push_back(pass_json(description.passes[index]));

    return Json{
        {"schema", bounded_text(description.schema)},
        {"graphId", bounded_text(description.graph_id)},
        {"graphGeneration", description.graph_generation},
        {"policy", policy_json(description.policy)},
        {"budget", budget_json(description.budget)},
        {"capabilities", capabilities_json(description.capabilities)},
        {"resources", std::move(resources)},
        {"passes", std::move(passes)},
    }
        .dump();
}

std::string raytracing_render_graph_canonical_evidence(
    const RayTracingRenderGraphPlan& plan) {
    auto resources = plan.resources;
    sort_resource_plans(resources);
    auto passes = plan.passes;
    sort_pass_plans(passes);
    auto diagnostics = plan.diagnostics;
    sort_diagnostics(diagnostics);
    if (diagnostics.size() > raytracing_render_graph_max_diagnostics)
        diagnostics.resize(raytracing_render_graph_max_diagnostics);

    Json execution_order = Json::array();
    for (const auto& pass : plan.execution_order)
        execution_order.push_back(bounded_text(pass));
    Json resource_values = Json::array();
    for (const auto& resource : resources)
        resource_values.push_back(resource_plan_json(resource));
    Json pass_values = Json::array();
    for (const auto& pass : passes)
        pass_values.push_back(pass_plan_json(pass));
    Json diagnostic_values = Json::array();
    for (const auto& diagnostic : diagnostics)
        diagnostic_values.push_back(diagnostic_json(diagnostic));

    return Json{
        {"schema", bounded_text(plan.schema)},
        {"graphId", bounded_text(plan.graph_id)},
        {"frameGeneration", plan.frame_generation},
        {"graphGeneration", plan.graph_generation},
        {"mode", raytracing_render_graph_mode_name(plan.mode)},
        {"valid", plan.valid},
        {"supported", plan.supported},
        {"code", bounded_text(plan.code)},
        {"detail", bounded_text(plan.detail)},
        {"budget", budget_report_json(plan.budget)},
        {"fallback", fallback_json(plan.fallback)},
        {"executionOrder", std::move(execution_order)},
        {"resources", std::move(resource_values)},
        {"passes", std::move(pass_values)},
        {"diagnostics", std::move(diagnostic_values)},
        {"diagnosticsTruncated",
         plan.diagnostics.size() > raytracing_render_graph_max_diagnostics},
        // This is a deliberate boundary assertion for Agent observation: the
        // plan is portable evidence, not an accidental native handle dump.
        {"nativeHandlesExposed", false},
    }
        .dump();
}

std::string raytracing_render_graph_observation_json(
    const RayTracingRenderGraphPlan& plan) {
    Json observation = Json::parse(raytracing_render_graph_canonical_evidence(plan));
    observation["observation"] = "persistent-raytracing-render-graph";
    observation["observationVersion"] = "0.1";
    return observation.dump();
}

std::string raytracing_render_graph_fingerprint(
    const RayTracingRenderGraphPlan& plan) {
    return hex_u64(fnv1a(raytracing_render_graph_canonical_evidence(plan)));
}

} // namespace noemancer
