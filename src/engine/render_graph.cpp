#include "engine/render_graph.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {

CompiledRenderGraph RenderGraphCompiler::compile(
    std::string graph_id, std::vector<RenderResourceDefinition> resources,
    std::vector<RenderPassDefinition> passes) {
    CompiledRenderGraph graph;
    graph.graph_id = std::move(graph_id);
    graph.resources = std::move(resources);
    graph.passes = std::move(passes);
    std::unordered_set<std::string> resource_ids;
    for (const auto& resource : graph.resources) {
        if (resource.id.empty() || !resource_ids.insert(resource.id).second)
            graph.errors.push_back("duplicate-or-empty-resource:" + resource.id);
        if (resource.format.empty() || resource.layers==0U || (resource.dimension!="2d" && resource.dimension!="2d-array" && resource.dimension!="3d" && resource.dimension!="cube" && resource.dimension!="buffer"))
            graph.errors.push_back("invalid-resource-description:"+resource.id);
        if (resource.resolution_space!="render" && resource.resolution_space!="output" &&
            resource.resolution_space!="half-output" && resource.resolution_space!="half-render" &&
            resource.resolution_space!="quarter-output" && resource.resolution_space!="eighth-output" &&
            resource.resolution_space!="sixteenth-output" &&
            resource.resolution_space!="scalar" && resource.resolution_space!="shadow" &&
            resource.resolution_space!="camera-volume")
            graph.errors.push_back("invalid-resolution-space:"+resource.id);
    }
    std::unordered_map<std::string, std::size_t> pass_indices;
    for (std::size_t i = 0; i < graph.passes.size(); ++i)
        if (graph.passes[i].id.empty() || !pass_indices.emplace(graph.passes[i].id, i).second)
            graph.errors.push_back("duplicate-or-empty-pass:" + graph.passes[i].id);
    std::vector<std::vector<std::size_t>> edges(graph.passes.size());
    std::vector<std::size_t> indegree(graph.passes.size());
    std::unordered_map<std::string, std::string> writers;
    for (std::size_t i = 0; i < graph.passes.size(); ++i) {
        const auto& pass = graph.passes[i];
        if (pass.pipeline_id.empty()) graph.errors.push_back("missing-pipeline:" + pass.id);
        for (const auto& resource : pass.reads)
            if (!resource_ids.contains(resource)) graph.errors.push_back("unknown-read:" + pass.id + ":" + resource);
        for (const auto& resource : pass.writes) {
            if (!resource_ids.contains(resource)) graph.errors.push_back("unknown-write:" + pass.id + ":" + resource);
            else if (const auto found=writers.find(resource); found!=writers.end()) {
                const bool read_modify_write=std::ranges::find(pass.reads,resource)!=pass.reads.end() &&
                    std::ranges::find(pass.depends_on,found->second)!=pass.depends_on.end();
                if (!read_modify_write) graph.errors.push_back("multiple-writers:" + resource + ":" + found->second + ":" + pass.id);
                else found->second=pass.id;
            } else writers.emplace(resource,pass.id);
        }
        for (const auto& dependency : pass.depends_on) {
            const auto found = pass_indices.find(dependency);
            if (found == pass_indices.end()) graph.errors.push_back("unknown-dependency:" + pass.id + ":" + dependency);
            else { edges[found->second].push_back(i); ++indegree[i]; }
        }
    }
    for (const auto& pass : graph.passes) for (const auto& resource : pass.reads)
        if (resource_ids.contains(resource) && !writers.contains(resource) &&
            !std::ranges::any_of(graph.resources,[&](const auto& definition){return definition.id==resource&&definition.external;}))
            graph.errors.push_back("read-before-producer:" + pass.id + ":" + resource);
    std::priority_queue<std::string, std::vector<std::string>, std::greater<>> ready;
    for (std::size_t i = 0; i < graph.passes.size(); ++i) if (indegree[i] == 0) ready.push(graph.passes[i].id);
    while (!ready.empty()) {
        auto id = ready.top(); ready.pop();
        graph.execution_order.push_back(id);
        const auto index = pass_indices.at(id);
        for (const auto next : edges[index]) if (--indegree[next] == 0) ready.push(graph.passes[next].id);
    }
    if (graph.execution_order.size() != graph.passes.size()) graph.errors.push_back("dependency-cycle");
    if (graph.execution_order.size()==graph.passes.size()) {
        std::unordered_map<std::string,std::size_t> execution_indices;
        for (std::size_t index=0;index<graph.execution_order.size();++index) execution_indices.emplace(graph.execution_order[index],index);
        for (const auto& resource:graph.resources) {
            RenderResourcePlan plan; plan.resource_id=resource.id; plan.transient=resource.transient;
            plan.first_use_pass=graph.execution_order.size();
            for (const auto& pass:graph.passes) {
                const auto execution_index=execution_indices.at(pass.id);
                const bool reads=std::ranges::find(pass.reads,resource.id)!=pass.reads.end();
                const bool writes=std::ranges::find(pass.writes,resource.id)!=pass.writes.end();
                if (!reads&&!writes) continue;
                plan.first_use_pass=std::min(plan.first_use_pass,execution_index);
                plan.last_use_pass=std::max(plan.last_use_pass,execution_index);
                if (reads) plan.readers.push_back(pass.id);
                if (writes) plan.writers.push_back(pass.id);
            }
            if (plan.first_use_pass==graph.execution_order.size()) plan.first_use_pass=plan.last_use_pass=0;
            plan.alias_candidate=plan.transient && !plan.readers.empty() && !plan.writers.empty() &&
                plan.last_use_pass+1U<graph.execution_order.size();
            graph.resource_plans.push_back(std::move(plan));
        }
    }
    graph.valid = graph.errors.empty();
    return graph;
}

CompiledRenderGraph make_forward_render_graph(
    const bool include_native_rt_debug_composite) {
    // v17 keeps every existing resource/pass identity intact while making the
    // previous-frame occlusion input explicit.  The depth pyramid is one logical
    // RG32_FLOAT mip-chain resource: seed writes mip 0 and reduce fills the
    // remaining mips through an explicit read/modify/write edge.  Keeping the
    // chain behind one stable resource ID lets SSR, SSGI and future occlusion
    // consumers attach to the same graph contract instead of creating local
    // HiZ resources.
    auto graph = RenderGraphCompiler::compile("render.graph.forward.v17",
        {{"render.resource.shadow-depth", "D32_FLOAT", true, "2d-array", 4, false, "shadow"}, {"render.resource.scene-hdr", "RGBA16_FLOAT", true},
         {"render.resource.atmosphere-camera-volume", "RGBA16_FLOAT", false, "3d", 1, true, "camera-volume"},
         {"render.resource.scene-hdr-aerial", "RGBA16_FLOAT", true},
          {"render.resource.scene-indirect", "RGBA16_FLOAT", true},
          {"render.resource.scene-specular-indirect", "RGBA16_FLOAT", true},
          {"render.resource.surface-reflection-properties", "RGBA16_FLOAT", true},
          {"render.resource.scene-hdr-ao", "RGBA16_FLOAT", true},
          {"render.resource.scene-gi-raw", "RGBA16_FLOAT", true},
          {"render.resource.scene-gi-raw-bent-normal", "RGBA16_FLOAT", true},
          {"render.resource.scene-gi-spatial", "RGBA16_FLOAT", true},
          {"render.resource.scene-gi-spatial-bent-normal", "RGBA16_FLOAT", true},
          {"render.resource.scene-gi-bent-normal-visibility", "RGBA16_FLOAT", true},
          {"render.resource.scene-gi-resolved", "RGBA16_FLOAT", true},
          {"render.resource.scene-gi-history", "RGBA16_FLOAT", false, "2d", 1, true, "render"},
          {"render.resource.scene-gi-bent-normal-history", "RGBA16_FLOAT", false, "2d", 1, true, "render"},
          {"render.resource.scene-hdr-gi", "RGBA16_FLOAT", true},
          {"render.resource.scene-reflection-raw", "RGBA16_FLOAT", true},
          {"render.resource.scene-reflection-resolved", "RGBA16_FLOAT", true},
          {"render.resource.scene-reflection-history", "RGBA16_FLOAT", false, "2d", 1, true, "render"},
          {"render.resource.scene-hdr-reflected", "RGBA16_FLOAT", true},
         {"render.resource.scene-color", "RGBA8_SRGB", false, "2d", 1, false, "output"},
         {"render.resource.scene-resolved", "RGBA16_FLOAT", true, "2d", 1, false, "output"},
         {"render.resource.bloom-half-down", "RGBA16_FLOAT", true, "2d", 1, false, "half-output"},
         {"render.resource.bloom-quarter-down", "RGBA16_FLOAT", true, "2d", 1, false, "quarter-output"},
         {"render.resource.bloom-eighth-down", "RGBA16_FLOAT", true, "2d", 1, false, "eighth-output"},
         {"render.resource.bloom-sixteenth-down", "RGBA16_FLOAT", true, "2d", 1, false, "sixteenth-output"},
         {"render.resource.bloom-eighth-up", "RGBA16_FLOAT", true, "2d", 1, false, "eighth-output"},
         {"render.resource.bloom-quarter-up", "RGBA16_FLOAT", true, "2d", 1, false, "quarter-output"},
         {"render.resource.bloom-half", "RGBA16_FLOAT", true, "2d", 1, false, "half-output"},
         {"render.resource.ambient-occlusion", "R8_UNORM", true, "2d", 1, false, "half-render"},
         {"render.resource.ambient-occlusion-temp", "R8_UNORM", true, "2d", 1, false, "half-render"},
         {"render.resource.ambient-occlusion-filtered", "R8_UNORM", true, "2d", 1, false, "half-render"},
         {"render.resource.exposure-history", "R16_FLOAT", false, "2d", 1, true, "scalar"},
         {"render.resource.motion-vectors", "RG16_FLOAT", false},
         {"render.resource.reactive-mask", "R8_UNORM", false},
         {"render.resource.temporal-history", "RGBA16_FLOAT", false, "2d", 1, true, "output"},
         {"render.resource.temporal-depth-history", "R32_FLOAT", false, "2d", 1, true, "output"},
         {"render.resource.previous-normal-history", "RGBA16_FLOAT", false, "2d", 1, true, "output"},
         {"render.resource.scene-depth-pyramid", "RG32_FLOAT", false, "2d", 1, false, "render"},
         {"render.resource.scene-depth", "D32_FLOAT", false}, {"render.resource.object-id", "R32_UINT", false},
         {"render.resource.world-normal", "RGBA16_FLOAT", false},
         {"render.resource.gpu-scene-instances","STRUCTURED-224",false,"buffer",1,true,"scalar"},
         {"render.resource.gpu-draw-batches","STRUCTURED-16",false,"buffer",1,true,"scalar"},
         {"render.resource.gpu-visible-indices","UINT32",false,"buffer",1,true,"scalar"},
         {"render.resource.gpu-indirect-commands","INDEXED-INDIRECT-20",false,"buffer",1,true,"scalar"}},
        {{"render.pass.shadow-depth", "render.pipeline.shadow-depth", {}, {"render.resource.shadow-depth"}, {}},
         {"render.pass.gpu-visibility","render.pipeline.compute-frustum-hiz-compact",
          {"render.resource.gpu-scene-instances","render.resource.gpu-draw-batches","render.resource.scene-depth-pyramid"},
          {"render.resource.gpu-visible-indices","render.resource.gpu-indirect-commands"},{"render.pass.shadow-depth"}},
         {"render.pass.sky-atmosphere", "render.pipeline.sky-atmosphere", {},
          {"render.resource.scene-hdr", "render.resource.atmosphere-camera-volume"}, {"render.pass.gpu-visibility"}},
         {"render.pass.opaque-lit", "render.pipeline.pbr-forward", {"render.resource.scene-hdr","render.resource.shadow-depth","render.resource.gpu-scene-instances","render.resource.gpu-visible-indices","render.resource.gpu-indirect-commands"},
           {"render.resource.scene-hdr", "render.resource.scene-indirect", "render.resource.scene-depth", "render.resource.object-id", "render.resource.world-normal", "render.resource.motion-vectors", "render.resource.reactive-mask", "render.resource.scene-specular-indirect", "render.resource.surface-reflection-properties"},
          {"render.pass.gpu-visibility", "render.pass.sky-atmosphere"}},
         {"render.pass.depth-pyramid-seed", "render.pipeline.depth-pyramid-seed", {"render.resource.scene-depth"},
          {"render.resource.scene-depth-pyramid"}, {"render.pass.opaque-lit"}},
         {"render.pass.depth-pyramid-reduce", "render.pipeline.depth-pyramid-reduce", {"render.resource.scene-depth-pyramid"},
          {"render.resource.scene-depth-pyramid"}, {"render.pass.depth-pyramid-seed"}},
         {"render.pass.ambient-occlusion", "render.pipeline.gtao", {"render.resource.scene-depth", "render.resource.world-normal"},
           {"render.resource.ambient-occlusion"}, {"render.pass.opaque-lit", "render.pass.depth-pyramid-reduce"}},
         {"render.pass.ambient-occlusion-denoise-horizontal", "render.pipeline.ao-bilateral-horizontal",
          {"render.resource.ambient-occlusion", "render.resource.scene-depth", "render.resource.world-normal"},
          {"render.resource.ambient-occlusion-temp"}, {"render.pass.ambient-occlusion"}},
         {"render.pass.ambient-occlusion-denoise-vertical", "render.pipeline.ao-bilateral-vertical",
          {"render.resource.ambient-occlusion-temp", "render.resource.scene-depth", "render.resource.world-normal"},
          {"render.resource.ambient-occlusion-filtered"}, {"render.pass.ambient-occlusion-denoise-horizontal"}},
         {"render.pass.aerial-perspective", "render.pipeline.aerial-perspective",
          {"render.resource.scene-hdr", "render.resource.scene-depth", "render.resource.atmosphere-camera-volume"},
          {"render.resource.scene-hdr-aerial"}, {"render.pass.opaque-lit", "render.pass.depth-pyramid-reduce"}},
          {"render.pass.ambient-occlusion-composite", "render.pipeline.ao-indirect-composite",
           {"render.resource.scene-hdr-aerial", "render.resource.scene-indirect", "render.resource.ambient-occlusion-filtered"},
          {"render.resource.scene-hdr-ao"}, {"render.pass.ambient-occlusion-denoise-vertical", "render.pass.aerial-perspective"}},
          {"render.pass.ssgi-hierarchical-gather", "render.pipeline.ssgi-hiz-gather",
           {"render.resource.scene-hdr-ao", "render.resource.scene-depth", "render.resource.scene-depth-pyramid", "render.resource.world-normal", "render.resource.surface-reflection-properties"},
           {"render.resource.scene-gi-raw", "render.resource.scene-gi-raw-bent-normal"}, {"render.pass.ambient-occlusion-composite", "render.pass.depth-pyramid-reduce"}},
          {"render.pass.ssgi-spatial-resolve", "render.pipeline.ssgi-spatial-resolve",
           {"render.resource.scene-gi-raw", "render.resource.scene-gi-raw-bent-normal", "render.resource.scene-depth", "render.resource.world-normal", "render.resource.surface-reflection-properties"},
           {"render.resource.scene-gi-spatial", "render.resource.scene-gi-spatial-bent-normal"}, {"render.pass.ssgi-hierarchical-gather"}},
          {"render.pass.ssgi-temporal-resolve", "render.pipeline.ssgi-temporal-resolve",
           {"render.resource.scene-gi-spatial", "render.resource.scene-gi-spatial-bent-normal", "render.resource.scene-gi-history", "render.resource.scene-gi-bent-normal-history", "render.resource.scene-depth", "render.resource.temporal-depth-history", "render.resource.motion-vectors", "render.resource.reactive-mask"},
           {"render.resource.scene-gi-resolved", "render.resource.scene-gi-bent-normal-visibility", "render.resource.scene-gi-history", "render.resource.scene-gi-bent-normal-history"}, {"render.pass.ssgi-spatial-resolve"}},
          {"render.pass.ssgi-composite", "render.pipeline.ssgi-diffuse-ibl-composite",
           {"render.resource.scene-hdr-ao", "render.resource.scene-indirect", "render.resource.scene-specular-indirect", "render.resource.ambient-occlusion-filtered", "render.resource.scene-gi-resolved", "render.resource.scene-gi-bent-normal-visibility", "render.resource.surface-reflection-properties"},
           {"render.resource.scene-hdr-gi"}, {"render.pass.ssgi-temporal-resolve", "render.pass.ambient-occlusion-composite"}},
          {"render.pass.ssr-hierarchical-trace", "render.pipeline.ssr-hiz-trace",
           {"render.resource.scene-hdr-gi", "render.resource.scene-depth", "render.resource.scene-depth-pyramid", "render.resource.world-normal", "render.resource.surface-reflection-properties"},
           {"render.resource.scene-reflection-raw"}, {"render.pass.ssgi-composite", "render.pass.depth-pyramid-reduce"}},
          {"render.pass.ssr-temporal-resolve", "render.pipeline.ssr-temporal-resolve",
           {"render.resource.scene-reflection-raw", "render.resource.motion-vectors", "render.resource.scene-depth", "render.resource.world-normal", "render.resource.scene-reflection-history", "render.resource.temporal-depth-history", "render.resource.previous-normal-history", "render.resource.reactive-mask"},
           {"render.resource.scene-reflection-resolved", "render.resource.scene-reflection-history"}, {"render.pass.ssr-hierarchical-trace"}},
          {"render.pass.ssr-composite", "render.pipeline.ssr-energy-conserving-composite",
           {"render.resource.scene-hdr-gi", "render.resource.scene-specular-indirect", "render.resource.ambient-occlusion-filtered", "render.resource.scene-reflection-resolved", "render.resource.surface-reflection-properties"},
           {"render.resource.scene-hdr-reflected"}, {"render.pass.ssr-temporal-resolve", "render.pass.ssgi-composite"}},
          {"render.pass.transparent-lit", "render.pipeline.pbr-forward-alpha", {"render.resource.shadow-depth", "render.resource.scene-hdr-reflected", "render.resource.scene-indirect", "render.resource.scene-depth", "render.resource.object-id", "render.resource.world-normal", "render.resource.motion-vectors", "render.resource.reactive-mask"},
           {"render.resource.scene-hdr-reflected", "render.resource.scene-indirect", "render.resource.object-id", "render.resource.world-normal", "render.resource.motion-vectors", "render.resource.reactive-mask"}, {"render.pass.ssr-composite", "render.pass.opaque-lit"}},
          {"render.pass.temporal-resolve", "render.pipeline.shared-temporal-denoise", {"render.resource.scene-hdr-reflected", "render.resource.motion-vectors", "render.resource.scene-depth", "render.resource.scene-depth-pyramid", "render.resource.world-normal", "render.resource.temporal-history", "render.resource.temporal-depth-history", "render.resource.previous-normal-history", "render.resource.reactive-mask"},
          {"render.resource.scene-resolved", "render.resource.temporal-history", "render.resource.temporal-depth-history", "render.resource.previous-normal-history"}, {"render.pass.transparent-lit", "render.pass.depth-pyramid-reduce"}},
         {"render.pass.auto-exposure", "render.pipeline.log-luminance-exposure", {"render.resource.scene-resolved", "render.resource.exposure-history"},
          {"render.resource.exposure-history"}, {"render.pass.temporal-resolve"}},
         {"render.pass.bloom-downsample-half", "render.pipeline.bloom-downsample-half", {"render.resource.scene-resolved"},
          {"render.resource.bloom-half-down"}, {"render.pass.auto-exposure"}},
         {"render.pass.bloom-downsample-quarter", "render.pipeline.bloom-downsample-quarter", {"render.resource.bloom-half-down"},
          {"render.resource.bloom-quarter-down"}, {"render.pass.bloom-downsample-half"}},
         {"render.pass.bloom-downsample-eighth", "render.pipeline.bloom-downsample-eighth", {"render.resource.bloom-quarter-down"},
          {"render.resource.bloom-eighth-down"}, {"render.pass.bloom-downsample-quarter"}},
         {"render.pass.bloom-downsample-sixteenth", "render.pipeline.bloom-downsample-sixteenth", {"render.resource.bloom-eighth-down"},
          {"render.resource.bloom-sixteenth-down"}, {"render.pass.bloom-downsample-eighth"}},
         {"render.pass.bloom-upsample-eighth", "render.pipeline.bloom-upsample-eighth",
          {"render.resource.bloom-sixteenth-down", "render.resource.bloom-eighth-down"}, {"render.resource.bloom-eighth-up"},
          {"render.pass.bloom-downsample-sixteenth", "render.pass.bloom-downsample-eighth"}},
         {"render.pass.bloom-upsample-quarter", "render.pipeline.bloom-upsample-quarter",
          {"render.resource.bloom-eighth-up", "render.resource.bloom-quarter-down"}, {"render.resource.bloom-quarter-up"},
          {"render.pass.bloom-upsample-eighth", "render.pass.bloom-downsample-quarter"}},
         {"render.pass.bloom-upsample-half", "render.pipeline.bloom-upsample-half",
          {"render.resource.bloom-quarter-up", "render.resource.bloom-half-down"}, {"render.resource.bloom-half"},
          {"render.pass.bloom-upsample-quarter", "render.pass.bloom-downsample-half"}},
         {"render.pass.tone-map", "render.pipeline.aces-tone-map", {"render.resource.scene-resolved", "render.resource.bloom-half", "render.resource.exposure-history"},
          {"render.resource.scene-color"}, {"render.pass.bloom-upsample-half"}}});
    if (!include_native_rt_debug_composite) return graph;

    // The first native RT presentation path is deliberately a diagnostic
    // overlay, not RTGI. Its output is a distinct logical resource that may
    // alias the physical presentation texture after the regular tone-map
    // lifetime ends. This keeps the external native surface explicit without
    // pretending that the fixed-camera probe participates in scene lighting.
    auto resources = graph.resources;
    resources.push_back({"render.resource.native-rt-debug-output",
        "R32G32B32A32_UINT", false, "2d", 1, true, "output"});
    resources.push_back({"render.resource.scene-color-native-rt-debug",
        "RGBA8_UNORM", true, "2d", 1, false, "output"});
    auto passes = graph.passes;
    passes.push_back({"render.pass.native-rt-debug-composite",
        "render.pipeline.native-rt-debug-composite",
        {"render.resource.native-rt-debug-output"},
        {"render.resource.scene-color-native-rt-debug"},
        {"render.pass.tone-map"}});
    return RenderGraphCompiler::compile("render.graph.forward.native-rt-debug.v18",
        std::move(resources), std::move(passes));
}

std::string render_graph_json(const CompiledRenderGraph& graph) {
    nlohmann::json out{{"schemaVersion", graph.schema_version}, {"graphId", graph.graph_id}, {"valid", graph.valid},
        {"resources", nlohmann::json::array()}, {"resourcePlans", nlohmann::json::array()}, {"passes", nlohmann::json::array()},
        {"executionOrder", graph.execution_order}, {"errors", graph.errors}};
    for (const auto& resource : graph.resources) out["resources"].push_back({{"id", resource.id}, {"format", resource.format},
        {"dimension",resource.dimension},{"layers",resource.layers},{"transient", resource.transient},{"external",resource.external},
        {"resolutionSpace",resource.resolution_space}});
    for (const auto& plan:graph.resource_plans) out["resourcePlans"].push_back({{"resourceId",plan.resource_id},
        {"firstUsePass",plan.first_use_pass},{"lastUsePass",plan.last_use_pass},
        {"lifetimePassCount",plan.last_use_pass-plan.first_use_pass+1U},{"readers",plan.readers},{"writers",plan.writers},
        {"transient",plan.transient},{"aliasCandidate",plan.alias_candidate}});
    for (const auto& pass : graph.passes) out["passes"].push_back({{"id", pass.id}, {"pipelineId", pass.pipeline_id},
        {"reads", pass.reads}, {"writes", pass.writes}, {"dependsOn", pass.depends_on}});
    return out.dump();
}

} // namespace noemancer
