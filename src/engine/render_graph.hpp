#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace noemancer {

struct RenderResourceDefinition final {
    std::string id;
    std::string format;
    bool transient{true};
    std::string dimension{"2d"};
    std::size_t layers{1};
    bool external{};
    std::string resolution_space{"render"};
};

struct RenderPassDefinition final {
    std::string id;
    std::string pipeline_id;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::vector<std::string> depends_on;
};

struct RenderResourcePlan final {
    std::string resource_id;
    std::size_t first_use_pass{};
    std::size_t last_use_pass{};
    std::vector<std::string> readers;
    std::vector<std::string> writers;
    bool transient{};
    bool alias_candidate{};
};

struct CompiledRenderGraph final {
    std::string schema_version{"noemancer.render-graph.v11"};
    std::string graph_id;
    bool valid{};
    std::vector<RenderResourceDefinition> resources;
    std::vector<RenderPassDefinition> passes;
    std::vector<std::string> execution_order;
    std::vector<RenderResourcePlan> resource_plans;
    std::vector<std::string> errors;
};

class RenderGraphCompiler final {
public:
    [[nodiscard]] static CompiledRenderGraph compile(
        std::string graph_id,
        std::vector<RenderResourceDefinition> resources,
        std::vector<RenderPassDefinition> passes);
};

[[nodiscard]] CompiledRenderGraph make_forward_render_graph(
    bool include_native_rt_debug_composite = false);
[[nodiscard]] std::string render_graph_json(const CompiledRenderGraph& graph);

} // namespace noemancer
