#include "engine/render_graph.hpp"
#include "engine/clustered_lighting.hpp"
#include "engine/render_world.hpp"
#include "engine/sprite_asset.hpp"
#include "engine/stable_range_allocator.hpp"
#include "engine/world.hpp"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>

int main() {
    noemancer::StableRangeAllocator stable_ranges(32);
    const auto chunk_a=stable_ranges.acquire("chunk-a",3,1);
    const auto chunk_b=stable_ranges.acquire("chunk-b",4,1);
    const auto chunk_a_stable=stable_ranges.acquire("chunk-a",2,2);
    const auto chunk_a_grown=stable_ranges.acquire("chunk-a",5,3);
    if(!chunk_a.valid||chunk_a.capacity!=4||!chunk_b.valid||chunk_b.first!=4||
       chunk_a_stable.first!=chunk_a.first||chunk_a_stable.moved||!chunk_a_grown.moved||chunk_a_grown.capacity!=8) {
        std::cerr << "Stable range allocation did not preserve or grow chunk slots correctly\n";
        return 15;
    }
    static_cast<void>(stable_ranges.sweep(125,120));
    const auto range_stats=stable_ranges.statistics();
    if(range_stats.live_ranges!=0||range_stats.high_water!=0||range_stats.evictions!=2) {
        std::cerr << "Stable range allocation did not reclaim expired chunks\n";
        return 16;
    }
    const auto graph = noemancer::make_forward_render_graph();
    const std::vector<std::string> expected_forward_order{
        "render.pass.shadow-depth", "render.pass.gpu-visibility", "render.pass.sky-atmosphere", "render.pass.opaque-lit",
        "render.pass.depth-pyramid-seed", "render.pass.depth-pyramid-reduce", "render.pass.aerial-perspective",
        "render.pass.ambient-occlusion", "render.pass.ambient-occlusion-denoise-horizontal",
        "render.pass.ambient-occlusion-denoise-vertical", "render.pass.ambient-occlusion-composite",
        "render.pass.ssgi-hierarchical-gather", "render.pass.ssgi-spatial-resolve",
        "render.pass.ssgi-temporal-resolve", "render.pass.ssgi-composite",
        "render.pass.ssr-hierarchical-trace", "render.pass.ssr-temporal-resolve", "render.pass.ssr-composite", "render.pass.transparent-lit",
        "render.pass.temporal-resolve", "render.pass.auto-exposure", "render.pass.bloom-downsample-half",
        "render.pass.bloom-downsample-quarter", "render.pass.bloom-downsample-eighth",
        "render.pass.bloom-downsample-sixteenth", "render.pass.bloom-upsample-eighth",
        "render.pass.bloom-upsample-quarter", "render.pass.bloom-upsample-half", "render.pass.tone-map"};
    if (!graph.valid || graph.execution_order != expected_forward_order) {
        std::cerr << "Forward graph did not produce the required deterministic pass order\n";
        return 1;
    }
    const auto graph_json = noemancer::render_graph_json(graph);
    if (graph_json.find("render.resource.object-id") == std::string::npos ||
        graph_json.find("render.resource.scene-hdr") == std::string::npos ||
        graph_json.find("render.pipeline.aces-tone-map") == std::string::npos ||
        graph_json.find("render.pipeline.pbr-forward-alpha") == std::string::npos ||
         graph_json.find("render.pipeline.shared-temporal-denoise") == std::string::npos ||
        graph_json.find("render.pipeline.bloom-downsample-half") == std::string::npos ||
        graph_json.find("render.pipeline.bloom-downsample-quarter") == std::string::npos ||
        graph_json.find("render.pipeline.bloom-downsample-eighth") == std::string::npos ||
        graph_json.find("render.pipeline.bloom-downsample-sixteenth") == std::string::npos ||
        graph_json.find("render.pipeline.bloom-upsample-eighth") == std::string::npos ||
        graph_json.find("render.pipeline.bloom-upsample-quarter") == std::string::npos ||
        graph_json.find("render.pipeline.bloom-upsample-half") == std::string::npos ||
         graph_json.find("render.pipeline.gtao") == std::string::npos ||
         graph_json.find("render.pipeline.depth-pyramid-seed") == std::string::npos ||
         graph_json.find("render.pipeline.depth-pyramid-reduce") == std::string::npos ||
        graph_json.find("render.pipeline.ao-bilateral-horizontal") == std::string::npos ||
        graph_json.find("render.pipeline.ao-bilateral-vertical") == std::string::npos ||
        graph_json.find("render.pipeline.ao-indirect-composite") == std::string::npos ||
        graph_json.find("render.pipeline.log-luminance-exposure") == std::string::npos ||
        graph_json.find("render.resource.ambient-occlusion") == std::string::npos ||
        graph_json.find("render.resource.ambient-occlusion-filtered") == std::string::npos ||
        graph_json.find("render.resource.scene-indirect") == std::string::npos ||
        graph_json.find("render.resource.scene-hdr-ao") == std::string::npos ||
        graph_json.find("render.resource.exposure-history") == std::string::npos ||
        graph_json.find("render.resource.bloom-half") == std::string::npos ||
        graph_json.find("render.resource.motion-vectors") == std::string::npos ||
         graph_json.find("render.resource.temporal-history") == std::string::npos ||
         graph_json.find("render.resource.temporal-depth-history") == std::string::npos ||
         graph_json.find("render.resource.previous-normal-history") == std::string::npos ||
         graph_json.find("render.resource.scene-depth-pyramid") == std::string::npos ||
         graph_json.find("render.resource.scene-reflection-history") == std::string::npos ||
         graph_json.find("render.resource.scene-gi-history") == std::string::npos ||
         graph_json.find("render.resource.scene-gi-bent-normal-visibility") == std::string::npos ||
         graph_json.find("render.pipeline.ssgi-hiz-gather") == std::string::npos ||
         graph_json.find("render.pipeline.ssgi-spatial-resolve") == std::string::npos ||
         graph_json.find("render.pipeline.ssgi-temporal-resolve") == std::string::npos ||
         graph_json.find("render.pipeline.ssgi-diffuse-ibl-composite") == std::string::npos ||
         graph_json.find("render.resource.surface-reflection-properties") == std::string::npos ||
         graph_json.find("render.resource.scene-specular-indirect") == std::string::npos ||
         graph_json.find("render.pipeline.ssr-hiz-trace") == std::string::npos ||
         graph_json.find("render.pipeline.ssr-temporal-resolve") == std::string::npos ||
         graph_json.find("render.pipeline.ssr-energy-conserving-composite") == std::string::npos ||
        graph_json.find("render.resource.reactive-mask") == std::string::npos ||
        graph_json.find("render.resource.gpu-indirect-commands") == std::string::npos ||
        graph_json.find("render.pipeline.compute-frustum-hiz-compact") == std::string::npos ||
        graph_json.find("render.pass.sky-atmosphere") == std::string::npos ||
        graph_json.find("render.pipeline.sky-atmosphere") == std::string::npos ||
        graph_json.find("render.resource.atmosphere-camera-volume") == std::string::npos ||
        graph_json.find("render.resource.scene-hdr-aerial") == std::string::npos ||
        graph_json.find("render.pass.aerial-perspective") == std::string::npos ||
        graph_json.find("render.pipeline.aerial-perspective") == std::string::npos ||
        graph_json.find("render.resource.world-normal") == std::string::npos ||
        graph_json.find("render.resource.scene-depth") == std::string::npos) {
        std::cerr << "Render evidence resources are absent from the graph\n";
        return 2;
    }
    const auto atmosphere_volume_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.atmosphere-camera-volume";
    });
    const auto aerial_hdr_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.scene-hdr-aerial";
    });
    if (atmosphere_volume_resource==graph.resources.end() || atmosphere_volume_resource->format!="RGBA16_FLOAT" ||
        atmosphere_volume_resource->dimension!="3d" || atmosphere_volume_resource->layers!=1U ||
        atmosphere_volume_resource->transient || !atmosphere_volume_resource->external ||
        atmosphere_volume_resource->resolution_space!="camera-volume" ||
        aerial_hdr_resource==graph.resources.end() || aerial_hdr_resource->format!="RGBA16_FLOAT" ||
        !aerial_hdr_resource->transient || aerial_hdr_resource->external) {
        std::cerr << "Atmosphere camera-volume resources are not described with the expected lifetime and dimensionality\n";
        return 22;
    }
    const auto hdr_plan=std::ranges::find_if(graph.resource_plans,[](const noemancer::RenderResourcePlan& plan){
        return plan.resource_id=="render.resource.scene-hdr";
    });
    if (hdr_plan==graph.resource_plans.end() || hdr_plan->first_use_pass!=2U || hdr_plan->last_use_pass!=6U ||
        hdr_plan->writers!=std::vector<std::string>{"render.pass.sky-atmosphere", "render.pass.opaque-lit"} ||
        hdr_plan->readers!=std::vector<std::string>{"render.pass.opaque-lit", "render.pass.aerial-perspective"} ||
        !hdr_plan->alias_candidate || graph.schema_version!="noemancer.render-graph.v11") {
        std::cerr << "Render resource lifetime plan is incomplete\n";
        return 10;
    }
    const auto opaque_pass=std::ranges::find_if(graph.passes,[](const noemancer::RenderPassDefinition& pass){
        return pass.id=="render.pass.opaque-lit";
    });
    const auto sky_pass=std::ranges::find_if(graph.passes,[](const noemancer::RenderPassDefinition& pass){
        return pass.id=="render.pass.sky-atmosphere";
    });
    const auto aerial_pass=std::ranges::find_if(graph.passes,[](const noemancer::RenderPassDefinition& pass){
        return pass.id=="render.pass.aerial-perspective";
    });
    const auto gpu_instances_plan=std::ranges::find_if(graph.resource_plans,[](const noemancer::RenderResourcePlan& plan){
        return plan.resource_id=="render.resource.gpu-scene-instances";
    });
    if (sky_pass==graph.passes.end() || sky_pass->pipeline_id!="render.pipeline.sky-atmosphere" ||
        !sky_pass->reads.empty() ||
        sky_pass->writes!=std::vector<std::string>{"render.resource.scene-hdr", "render.resource.atmosphere-camera-volume"} ||
        sky_pass->depends_on!=std::vector<std::string>{"render.pass.gpu-visibility"} ||
         graph.graph_id!="render.graph.forward.v17" ||
        aerial_pass==graph.passes.end() || aerial_pass->pipeline_id!="render.pipeline.aerial-perspective" ||
        aerial_pass->reads!=std::vector<std::string>{"render.resource.scene-hdr", "render.resource.scene-depth", "render.resource.atmosphere-camera-volume"} ||
        aerial_pass->writes!=std::vector<std::string>{"render.resource.scene-hdr-aerial"} ||
         aerial_pass->depends_on!=std::vector<std::string>{"render.pass.opaque-lit", "render.pass.depth-pyramid-reduce"} ||
        opaque_pass==graph.passes.end() ||
        std::ranges::count(opaque_pass->reads,"render.resource.gpu-scene-instances")!=1 ||
         std::ranges::count(opaque_pass->reads,"render.resource.scene-hdr")!=1 ||
         std::ranges::count(opaque_pass->writes,"render.resource.scene-specular-indirect")!=1 ||
         std::ranges::count(opaque_pass->writes,"render.resource.surface-reflection-properties")!=1 ||
        opaque_pass->depends_on != std::vector<std::string>{"render.pass.gpu-visibility", "render.pass.sky-atmosphere"} ||
        gpu_instances_plan==graph.resource_plans.end() || gpu_instances_plan->first_use_pass!=1U ||
        gpu_instances_plan->last_use_pass!=3U || !gpu_instances_plan->writers.empty() ||
        gpu_instances_plan->readers != std::vector<std::string>{"render.pass.gpu-visibility","render.pass.opaque-lit"}) {
        std::cerr << "Opaque GPU-driven pass did not retain the exact scene-instance resource dependency closure\n";
        return 18;
    }
    const auto exposure_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.exposure-history";
    });
    if (exposure_resource==graph.resources.end() || exposure_resource->transient || !exposure_resource->external ||
        exposure_resource->resolution_space!="scalar" || exposure_resource->format!="R16_FLOAT") {
        std::cerr << "Auto-exposure history is not a persistent scalar graph resource\n";
        return 15;
    }
    const auto bloom_resource=[&](const std::string& id) {
        return std::ranges::find_if(graph.resources,[&](const noemancer::RenderResourceDefinition& resource){
            return resource.id==id;
        });
    };
    const auto bloom_half_down=bloom_resource("render.resource.bloom-half-down");
    const auto bloom_quarter_down=bloom_resource("render.resource.bloom-quarter-down");
    const auto bloom_eighth_down=bloom_resource("render.resource.bloom-eighth-down");
    const auto bloom_sixteenth_down=bloom_resource("render.resource.bloom-sixteenth-down");
    const auto bloom_eighth_up=bloom_resource("render.resource.bloom-eighth-up");
    const auto bloom_quarter_up=bloom_resource("render.resource.bloom-quarter-up");
    const auto bloom_half=bloom_resource("render.resource.bloom-half");
    if (bloom_half_down==graph.resources.end() || bloom_half_down->resolution_space!="half-output" ||
        bloom_quarter_down==graph.resources.end() || bloom_quarter_down->resolution_space!="quarter-output" ||
        bloom_eighth_down==graph.resources.end() || bloom_eighth_down->resolution_space!="eighth-output" ||
        bloom_sixteenth_down==graph.resources.end() || bloom_sixteenth_down->resolution_space!="sixteenth-output" ||
        bloom_eighth_up==graph.resources.end() || bloom_eighth_up->resolution_space!="eighth-output" ||
        bloom_quarter_up==graph.resources.end() || bloom_quarter_up->resolution_space!="quarter-output" ||
        bloom_half==graph.resources.end() || bloom_half->resolution_space!="half-output" ||
        std::ranges::any_of(graph.resources.begin(),graph.resources.end(),[](const noemancer::RenderResourceDefinition& resource){
            return resource.id.starts_with("render.resource.bloom-") && resource.format!="RGBA16_FLOAT";
        })) {
        std::cerr << "Bloom resources do not describe the four downsample levels and upsample outputs\n";
        return 14;
    }
    const auto find_pass=[&](const std::string& id) {
        return std::ranges::find_if(graph.passes,[&](const noemancer::RenderPassDefinition& pass){return pass.id==id;});
    };
    const auto bloom_downsample_half=find_pass("render.pass.bloom-downsample-half");
    const auto bloom_downsample_quarter=find_pass("render.pass.bloom-downsample-quarter");
    const auto bloom_downsample_eighth=find_pass("render.pass.bloom-downsample-eighth");
    const auto bloom_downsample_sixteenth=find_pass("render.pass.bloom-downsample-sixteenth");
    const auto bloom_upsample_eighth=find_pass("render.pass.bloom-upsample-eighth");
    const auto bloom_upsample_quarter=find_pass("render.pass.bloom-upsample-quarter");
    const auto bloom_upsample_half=find_pass("render.pass.bloom-upsample-half");
    const auto tone_map=find_pass("render.pass.tone-map");
    const auto ao=find_pass("render.pass.ambient-occlusion");
    const auto ao_horizontal=find_pass("render.pass.ambient-occlusion-denoise-horizontal");
    const auto ao_vertical=find_pass("render.pass.ambient-occlusion-denoise-vertical");
    const auto ao_composite=find_pass("render.pass.ambient-occlusion-composite");
    const auto transparent=find_pass("render.pass.transparent-lit");
    const auto temporal=find_pass("render.pass.temporal-resolve");
    const auto depth_seed=find_pass("render.pass.depth-pyramid-seed");
    const auto depth_reduce=find_pass("render.pass.depth-pyramid-reduce");
    const auto ssgi_gather=find_pass("render.pass.ssgi-hierarchical-gather");
    const auto ssgi_spatial=find_pass("render.pass.ssgi-spatial-resolve");
    const auto ssgi_temporal=find_pass("render.pass.ssgi-temporal-resolve");
    const auto ssgi_composite=find_pass("render.pass.ssgi-composite");
    const auto ssr_trace=find_pass("render.pass.ssr-hierarchical-trace");
    const auto ssr_temporal=find_pass("render.pass.ssr-temporal-resolve");
    const auto ssr_composite=find_pass("render.pass.ssr-composite");
    if(depth_seed==graph.passes.end() || depth_seed->pipeline_id!="render.pipeline.depth-pyramid-seed" ||
       depth_seed->reads!=std::vector<std::string>{"render.resource.scene-depth"} ||
       depth_seed->writes!=std::vector<std::string>{"render.resource.scene-depth-pyramid"} ||
       depth_seed->depends_on!=std::vector<std::string>{"render.pass.opaque-lit"} ||
       depth_reduce==graph.passes.end() || depth_reduce->pipeline_id!="render.pipeline.depth-pyramid-reduce" ||
       depth_reduce->reads!=std::vector<std::string>{"render.resource.scene-depth-pyramid"} ||
       depth_reduce->writes!=std::vector<std::string>{"render.resource.scene-depth-pyramid"} ||
       depth_reduce->depends_on!=std::vector<std::string>{"render.pass.depth-pyramid-seed"} ||
       ao==graph.passes.end() ||
       ao->reads!=std::vector<std::string>{"render.resource.scene-depth","render.resource.world-normal"} ||
       ao->writes!=std::vector<std::string>{"render.resource.ambient-occlusion"} ||
       ao->depends_on!=std::vector<std::string>{"render.pass.opaque-lit", "render.pass.depth-pyramid-reduce"} ||
       ao_horizontal==graph.passes.end() ||
       ao_horizontal->reads!=std::vector<std::string>{"render.resource.ambient-occlusion","render.resource.scene-depth","render.resource.world-normal"} ||
       ao_horizontal->writes!=std::vector<std::string>{"render.resource.ambient-occlusion-temp"} ||
       ao_vertical==graph.passes.end() ||
       ao_vertical->reads!=std::vector<std::string>{"render.resource.ambient-occlusion-temp","render.resource.scene-depth","render.resource.world-normal"} ||
       ao_vertical->writes!=std::vector<std::string>{"render.resource.ambient-occlusion-filtered"} ||
       ao_composite==graph.passes.end() ||
        ao_composite->reads!=std::vector<std::string>{"render.resource.scene-hdr-aerial","render.resource.scene-indirect","render.resource.ambient-occlusion-filtered"} ||
        ao_composite->writes!=std::vector<std::string>{"render.resource.scene-hdr-ao"} ||
        ssgi_gather==graph.passes.end() || ssgi_gather->pipeline_id!="render.pipeline.ssgi-hiz-gather" ||
        ssgi_gather->writes!=std::vector<std::string>{"render.resource.scene-gi-raw","render.resource.scene-gi-raw-bent-normal"} ||
        ssgi_spatial==graph.passes.end() || ssgi_spatial->pipeline_id!="render.pipeline.ssgi-spatial-resolve" ||
        ssgi_temporal==graph.passes.end() || ssgi_temporal->pipeline_id!="render.pipeline.ssgi-temporal-resolve" ||
        ssgi_temporal->writes!=std::vector<std::string>{"render.resource.scene-gi-resolved","render.resource.scene-gi-bent-normal-visibility","render.resource.scene-gi-history","render.resource.scene-gi-bent-normal-history"} ||
        ssgi_composite==graph.passes.end() || ssgi_composite->pipeline_id!="render.pipeline.ssgi-diffuse-ibl-composite" ||
        ssgi_composite->writes!=std::vector<std::string>{"render.resource.scene-hdr-gi"} ||
        ssr_trace==graph.passes.end() || ssr_trace->pipeline_id!="render.pipeline.ssr-hiz-trace" ||
        ssr_trace->reads!=std::vector<std::string>{"render.resource.scene-hdr-gi","render.resource.scene-depth","render.resource.scene-depth-pyramid","render.resource.world-normal","render.resource.surface-reflection-properties"} ||
        ssr_trace->writes!=std::vector<std::string>{"render.resource.scene-reflection-raw"} ||
        ssr_trace->depends_on!=std::vector<std::string>{"render.pass.ssgi-composite","render.pass.depth-pyramid-reduce"} ||
        ssr_temporal==graph.passes.end() || ssr_temporal->pipeline_id!="render.pipeline.ssr-temporal-resolve" ||
        ssr_temporal->reads!=std::vector<std::string>{"render.resource.scene-reflection-raw","render.resource.motion-vectors","render.resource.scene-depth","render.resource.world-normal","render.resource.scene-reflection-history","render.resource.temporal-depth-history","render.resource.previous-normal-history","render.resource.reactive-mask"} ||
        ssr_temporal->writes!=std::vector<std::string>{"render.resource.scene-reflection-resolved","render.resource.scene-reflection-history"} ||
        ssr_temporal->depends_on!=std::vector<std::string>{"render.pass.ssr-hierarchical-trace"} ||
        ssr_composite==graph.passes.end() || ssr_composite->pipeline_id!="render.pipeline.ssr-energy-conserving-composite" ||
        ssr_composite->reads!=std::vector<std::string>{"render.resource.scene-hdr-gi","render.resource.scene-specular-indirect","render.resource.ambient-occlusion-filtered","render.resource.scene-reflection-resolved","render.resource.surface-reflection-properties"} ||
        ssr_composite->writes!=std::vector<std::string>{"render.resource.scene-hdr-reflected"} ||
        ssr_composite->depends_on!=std::vector<std::string>{"render.pass.ssr-temporal-resolve","render.pass.ssgi-composite"} ||
        transparent==graph.passes.end() ||
        std::ranges::count(transparent->reads,"render.resource.scene-hdr-reflected")!=1 ||
        std::ranges::count(transparent->writes,"render.resource.scene-hdr-reflected")!=1 ||
        temporal==graph.passes.end() || temporal->pipeline_id!="render.pipeline.shared-temporal-denoise" ||
        temporal->reads!=std::vector<std::string>{"render.resource.scene-hdr-reflected", "render.resource.motion-vectors", "render.resource.scene-depth", "render.resource.scene-depth-pyramid", "render.resource.world-normal", "render.resource.temporal-history", "render.resource.temporal-depth-history", "render.resource.previous-normal-history", "render.resource.reactive-mask"} ||
       temporal->writes!=std::vector<std::string>{"render.resource.scene-resolved", "render.resource.temporal-history", "render.resource.temporal-depth-history", "render.resource.previous-normal-history"} ||
       temporal->depends_on!=std::vector<std::string>{"render.pass.transparent-lit", "render.pass.depth-pyramid-reduce"}) {
        std::cerr << "Screen-space DAG does not preserve depth reduction, temporal history, and AO composition\n";
        return 21;
    }
    if (bloom_downsample_half==graph.passes.end() || bloom_downsample_half->reads!=std::vector<std::string>{"render.resource.scene-resolved"} ||
        bloom_downsample_half->writes!=std::vector<std::string>{"render.resource.bloom-half-down"} ||
        bloom_downsample_half->depends_on!=std::vector<std::string>{"render.pass.auto-exposure"} ||
        bloom_downsample_quarter==graph.passes.end() || bloom_downsample_quarter->reads!=std::vector<std::string>{"render.resource.bloom-half-down"} ||
        bloom_downsample_quarter->writes!=std::vector<std::string>{"render.resource.bloom-quarter-down"} ||
        bloom_downsample_quarter->depends_on!=std::vector<std::string>{"render.pass.bloom-downsample-half"} ||
        bloom_downsample_eighth==graph.passes.end() || bloom_downsample_eighth->reads!=std::vector<std::string>{"render.resource.bloom-quarter-down"} ||
        bloom_downsample_eighth->writes!=std::vector<std::string>{"render.resource.bloom-eighth-down"} ||
        bloom_downsample_eighth->depends_on!=std::vector<std::string>{"render.pass.bloom-downsample-quarter"} ||
        bloom_downsample_sixteenth==graph.passes.end() || bloom_downsample_sixteenth->reads!=std::vector<std::string>{"render.resource.bloom-eighth-down"} ||
        bloom_downsample_sixteenth->writes!=std::vector<std::string>{"render.resource.bloom-sixteenth-down"} ||
        bloom_downsample_sixteenth->depends_on!=std::vector<std::string>{"render.pass.bloom-downsample-eighth"} ||
        bloom_upsample_eighth==graph.passes.end() ||
        bloom_upsample_eighth->reads!=std::vector<std::string>{"render.resource.bloom-sixteenth-down","render.resource.bloom-eighth-down"} ||
        bloom_upsample_eighth->writes!=std::vector<std::string>{"render.resource.bloom-eighth-up"} ||
        bloom_upsample_eighth->depends_on!=std::vector<std::string>{"render.pass.bloom-downsample-sixteenth","render.pass.bloom-downsample-eighth"} ||
        bloom_upsample_quarter==graph.passes.end() ||
        bloom_upsample_quarter->reads!=std::vector<std::string>{"render.resource.bloom-eighth-up","render.resource.bloom-quarter-down"} ||
        bloom_upsample_quarter->writes!=std::vector<std::string>{"render.resource.bloom-quarter-up"} ||
        bloom_upsample_quarter->depends_on!=std::vector<std::string>{"render.pass.bloom-upsample-eighth","render.pass.bloom-downsample-quarter"} ||
        bloom_upsample_half==graph.passes.end() ||
        bloom_upsample_half->reads!=std::vector<std::string>{"render.resource.bloom-quarter-up","render.resource.bloom-half-down"} ||
        bloom_upsample_half->writes!=std::vector<std::string>{"render.resource.bloom-half"} ||
        bloom_upsample_half->depends_on!=std::vector<std::string>{"render.pass.bloom-upsample-quarter","render.pass.bloom-downsample-half"} ||
        tone_map==graph.passes.end() ||
        tone_map->reads!=std::vector<std::string>{"render.resource.scene-resolved","render.resource.bloom-half","render.resource.exposure-history"} ||
        tone_map->depends_on!=std::vector<std::string>{"render.pass.bloom-upsample-half"}) {
        std::cerr << "Bloom DAG does not retain the exact downsample/upsample dependency closure\n";
        return 19;
    }
    const auto find_plan=[&](const std::string& id) {
        return std::ranges::find_if(graph.resource_plans,[&](const noemancer::RenderResourcePlan& plan){return plan.resource_id==id;});
    };
    const auto bloom_sixteenth_plan=find_plan("render.resource.bloom-sixteenth-down");
    const auto bloom_half_plan=find_plan("render.resource.bloom-half");
    const auto atmosphere_volume_plan=find_plan("render.resource.atmosphere-camera-volume");
    const auto aerial_hdr_plan=find_plan("render.resource.scene-hdr-aerial");
    if (atmosphere_volume_plan==graph.resource_plans.end() || atmosphere_volume_plan->first_use_pass!=2U ||
         atmosphere_volume_plan->last_use_pass!=6U ||
        atmosphere_volume_plan->writers!=std::vector<std::string>{"render.pass.sky-atmosphere"} ||
        atmosphere_volume_plan->readers!=std::vector<std::string>{"render.pass.aerial-perspective"} ||
        atmosphere_volume_plan->transient || atmosphere_volume_plan->alias_candidate ||
         aerial_hdr_plan==graph.resource_plans.end() || aerial_hdr_plan->first_use_pass!=6U ||
         aerial_hdr_plan->last_use_pass!=10U ||
        aerial_hdr_plan->writers!=std::vector<std::string>{"render.pass.aerial-perspective"} ||
        aerial_hdr_plan->readers!=std::vector<std::string>{"render.pass.ambient-occlusion-composite"} ||
        !aerial_hdr_plan->transient || !aerial_hdr_plan->alias_candidate ||
         bloom_sixteenth_plan==graph.resource_plans.end() || bloom_sixteenth_plan->first_use_pass!=24U ||
         bloom_sixteenth_plan->last_use_pass!=25U || bloom_sixteenth_plan->writers!=std::vector<std::string>{"render.pass.bloom-downsample-sixteenth"} ||
        bloom_sixteenth_plan->readers!=std::vector<std::string>{"render.pass.bloom-upsample-eighth"} ||
         bloom_half_plan==graph.resource_plans.end() || bloom_half_plan->first_use_pass!=27U ||
         bloom_half_plan->last_use_pass!=28U ||
        bloom_half_plan->writers!=std::vector<std::string>{"render.pass.bloom-upsample-half"} ||
        bloom_half_plan->readers!=std::vector<std::string>{"render.pass.tone-map"}) {
        std::cerr << "Bloom resource lifetime plans do not match the multi-scale DAG\n";
        return 20;
    }
    const auto shadow_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.shadow-depth";
    });
    if (shadow_resource==graph.resources.end() || shadow_resource->dimension!="2d-array" || shadow_resource->layers!=4U) {
        std::cerr << "Render graph does not describe the four-layer CSM resource\n";
        return 11;
    }
    const auto history_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.temporal-history";
    });
    if (history_resource==graph.resources.end() || history_resource->transient || !history_resource->external ||
         history_resource->resolution_space!="output") {
        std::cerr << "Temporal history is not represented as a persistent external graph resource\n";
        return 12;
    }
    const auto depth_pyramid_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.scene-depth-pyramid";
    });
    const auto gpu_visibility_pass=std::ranges::find_if(graph.passes,[](const noemancer::RenderPassDefinition& pass){
        return pass.id=="render.pass.gpu-visibility";
    });
    if(depth_pyramid_resource==graph.resources.end() || depth_pyramid_resource->transient ||
       gpu_visibility_pass==graph.passes.end() ||
       std::ranges::count(gpu_visibility_pass->reads,"render.resource.scene-depth-pyramid")!=1) {
        std::cerr << "GPU visibility did not declare the persistent previous-frame HiZ input\n";
        return 23;
    }
    const auto previous_normal_history_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.previous-normal-history";
    });
    const auto ssr_history_resource=std::ranges::find_if(graph.resources,[](const noemancer::RenderResourceDefinition& resource){
        return resource.id=="render.resource.scene-reflection-history";
    });
    if(previous_normal_history_resource==graph.resources.end() || previous_normal_history_resource->format!="RGBA16_FLOAT" ||
       previous_normal_history_resource->dimension!="2d" || previous_normal_history_resource->layers!=1U ||
       previous_normal_history_resource->transient || !previous_normal_history_resource->external ||
       previous_normal_history_resource->resolution_space!="output" ||
       depth_pyramid_resource==graph.resources.end() || depth_pyramid_resource->format!="RG32_FLOAT" ||
       depth_pyramid_resource->dimension!="2d" || depth_pyramid_resource->layers!=1U ||
       depth_pyramid_resource->transient || depth_pyramid_resource->external ||
       depth_pyramid_resource->resolution_space!="render" ||
       ssr_history_resource==graph.resources.end() || ssr_history_resource->format!="RGBA16_FLOAT" ||
       ssr_history_resource->transient || !ssr_history_resource->external ||
       ssr_history_resource->resolution_space!="render") {
        std::cerr << "Shared screen-space history resources do not describe the expected persistent/ephemeral lifetimes\n";
        return 23;
    }
    const auto previous_normal_plan=find_plan("render.resource.previous-normal-history");
    const auto depth_pyramid_plan=find_plan("render.resource.scene-depth-pyramid");
    const auto ssr_history_plan=find_plan("render.resource.scene-reflection-history");
    if(previous_normal_plan==graph.resource_plans.end() ||
       previous_normal_plan->writers!=std::vector<std::string>{"render.pass.temporal-resolve"} ||
       previous_normal_plan->readers!=std::vector<std::string>{"render.pass.ssr-temporal-resolve","render.pass.temporal-resolve"} ||
       previous_normal_plan->transient || previous_normal_plan->alias_candidate ||
       depth_pyramid_plan==graph.resource_plans.end() ||
       depth_pyramid_plan->writers!=std::vector<std::string>{"render.pass.depth-pyramid-seed", "render.pass.depth-pyramid-reduce"} ||
       depth_pyramid_plan->readers!=std::vector<std::string>{"render.pass.gpu-visibility", "render.pass.depth-pyramid-reduce", "render.pass.ssgi-hierarchical-gather", "render.pass.ssr-hierarchical-trace", "render.pass.temporal-resolve"} ||
       depth_pyramid_plan->transient || depth_pyramid_plan->alias_candidate ||
       ssr_history_plan==graph.resource_plans.end() ||
       ssr_history_plan->writers!=std::vector<std::string>{"render.pass.ssr-temporal-resolve"} ||
       ssr_history_plan->readers!=std::vector<std::string>{"render.pass.ssr-temporal-resolve"} ||
       ssr_history_plan->transient || ssr_history_plan->alias_candidate) {
        std::cerr << "Shared screen-space resource plans do not preserve history ownership or HiZ reduction order\n";
        return 24;
    }
    const auto missing = noemancer::RenderGraphCompiler::compile("invalid",
        {{"color", "RGBA8", false}}, {{"pass", "pipeline", {"missing"}, {"color"}, {}}});
    if (missing.valid || missing.errors.empty()) {
        std::cerr << "Unknown render resources were not rejected\n";
        return 3;
    }
    const auto external_history = noemancer::RenderGraphCompiler::compile("external-history",
        {{"history", "RGBA16F", false, "2d", 1, true}, {"resolved", "RGBA16F", true}},
        {{"resolve", "pipeline.taa", {"history"}, {"resolved"}, {}}});
    if (!external_history.valid) {
        std::cerr << "Persistent external graph input was rejected\n";
        return 13;
    }
    const auto cyclic = noemancer::RenderGraphCompiler::compile("cyclic", {},
        {{"a", "p.a", {}, {}, {"b"}}, {"b", "p.b", {}, {}, {"a"}}});
    if (cyclic.valid || cyclic.errors.back() != "dependency-cycle") {
        std::cerr << "Render pass dependency cycle was not rejected\n";
        return 4;
    }
    const auto invalid_multi_writer=noemancer::RenderGraphCompiler::compile("invalid-writers",
        {{"color","RGBA16F",true}},{{"a","p.a",{}, {"color"},{}},{"b","p.b",{}, {"color"},{"a"}}});
    if (invalid_multi_writer.valid) {
        std::cerr << "Unordered multi-writer resources were accepted\n";
        return 9;
    }
    noemancer::World world;
    auto render_scene=noemancer::make_bootstrap_scene_document();
    render_scene.entities.push_back({.guid="entity.light.local",.name="Local Light",
        .transform=noemancer::SceneTransform{{0.0,3.0,2.0}},
        .local_light=noemancer::SceneLocalLight{"spot",{1.0,0.5,0.2},900.0,10.0,{0.0,-1.0,-0.2},20.0,35.0,0.1,false}});
    const auto rotated_entity=std::ranges::find(render_scene.entities,"entity.demo-cube",&noemancer::SceneEntityDocument::guid);
    if(rotated_entity!=render_scene.entities.end()&&rotated_entity->transform) rotated_entity->transform->rotation_euler_degrees={0.0,45.0,0.0};
    const auto load = world.load_scene(render_scene);
    if (!load.success) {
        std::cerr << "Bootstrap scene could not be loaded for extraction\n";
        return 5;
    }
    static_cast<void>(world.vfx_spawn_json("vfx.debug-impact",{0.0F,1.0F,0.0F},0x52454e444552ULL));
    world.tick(1.0F / 60.0F);
    const auto snapshot = noemancer::RenderWorldExtractor::extract(world.revision(), 42, world.entity_views(), world.vfx_particles());
    const auto rotated_instance=std::ranges::find(snapshot.instances,"entity.demo-cube",&noemancer::RenderInstanceSnapshot::entity_id);
    if (!snapshot.camera || !snapshot.directional_light || snapshot.local_lights.size()!=1 || snapshot.instances.size() < 4 ||
        rotated_instance==snapshot.instances.end()||std::abs(rotated_instance->rotation[1])<0.3F||
        snapshot.world_revision != world.revision() || snapshot.frame_index != 42 || snapshot.vfx_particle_count != 48) {
        std::cerr << "Render World extraction omitted bootstrap scene state\n";
        return 6;
    }
    for (std::size_t i = 1; i < snapshot.instances.size(); ++i) {
        if (snapshot.instances[i - 1].entity_id > snapshot.instances[i].entity_id) {
            std::cerr << "Render instances are not stable-ID ordered\n";
            return 7;
        }
    }
    const auto json = nlohmann::json::parse(noemancer::render_world_json(snapshot));
    if (json.at("schemaVersion") != "noemancer.render-world.v15" ||
        json.at("extractionId") != snapshot.extraction_id || json.at("instances").empty() ||
        json.at("localLights").size()!=1 || json.at("localLights").at(0).at("luminousPowerLumens")!=900.0 ||
        json.at("vfxParticleCount") != 48 || json.at("vfxParticles").size() != 48 ||
        json.at("vfxSortPolicy") != "blend-group/alpha-back-to-front/stable-id" ||
        json.at("vfxParticles").at(0).at("velocity").size() != 3 ||
        json.at("vfxParticles").at(0).at("spritePolicy").at("pixelAlignment") != "profile" ||
        json.at("vfxParticles").at(0).at("spritePolicy").at("sizeQuantization") != "profile" ||
        json.at("vfxParticles").at(0).at("spritePolicy").at("sampling") != "profile" ||
        !json.at("vfxParticles").at(0).contains("spawnProvenance") ||
        json.at("vfxParticles").at(0).at("spawnProvenance").at("speedRange").size() != 2 ||
        json.at("vfxParticles").at(0).at("curve").at("colorStart").size() != 4 ||
        json.at("instances").at(0).at("rotationQuaternion").size()!=4 ||
        json.dump().find("vfx.debug-impact") == std::string::npos) {
        std::cerr << "Render World JSON contract is incomplete\n";
        return 8;
    }
    const auto clusters=noemancer::build_clustered_lighting({},{
        snapshot.camera->position,snapshot.camera->target,snapshot.camera->vertical_fov_degrees,16.0F/9.0F,
        snapshot.camera->near_clip,snapshot.camera->far_clip,snapshot.camera->projection=="orthographic",
        snapshot.camera->orthographic_height},snapshot.local_lights);
    if(clusters.clusters.size()!=16U*9U*24U||clusters.accepted_light_count!=1||clusters.light_indices.empty()||
       std::ranges::any_of(clusters.light_indices,[](const std::uint32_t index){return index!=0U;})) {
        std::cerr << "Clustered light assignment did not conservatively cover the local light\n";
        return 17;
    }
    const std::vector<noemancer::VfxRuntime::Particle> transparent_particles{
        {2,1,"alpha",{7.0F,5.5F,7.5F},{},0.0F,1.0F,0.1F,{},"alpha"},
        {3,1,"alpha",{7.0F,5.5F,-1.5F},{},0.0F,1.0F,0.1F,{},"alpha"},
        {5,1,"additive",{7.0F,5.5F,8.0F},{},0.0F,1.0F,0.1F,{},"additive"}};
    const auto sorted_snapshot=noemancer::RenderWorldExtractor::extract(world.revision(),43,world.entity_views(),transparent_particles);
    if(sorted_snapshot.vfx_particles.size()!=3||sorted_snapshot.vfx_particles[0].particle_id!=5||
       sorted_snapshot.vfx_particles[1].particle_id!=3||sorted_snapshot.vfx_particles[2].particle_id!=2||
       sorted_snapshot.vfx_particles[1].camera_distance_squared<=sorted_snapshot.vfx_particles[2].camera_distance_squared) {
        std::cerr << "VFX blend grouping and alpha back-to-front reference order are incorrect\n";
        return 10;
    }
    noemancer::WorldEntityView sprite_entity;
    sprite_entity.id="entity.sprite.courier";sprite_entity.transform=noemancer::Transform{.x=2.0F,.y=3.0F,.z=0.5F};
    sprite_entity.sprite_renderer=noemancer::SpriteRenderer{.playback={.asset_id="sprite.courier",.clip_id="idle"},
        .flip_x=true,.sorting_layer="actors",.sorting_order=7};
    sprite_entity.tilemap_renderer=noemancer::TilemapRenderer{.tilemap_asset="tilemap.level",.visible=true,.collision_enabled=true};
    sprite_entity.sprite_frame=noemancer::SpriteResolvedFrame{.asset_id="sprite.courier",.clip_id="idle",
        .texture_asset="texture.courier.atlas",.texture_width=64,.texture_height=32,.pixels_per_unit=16.0F,
        .sampling="nearest",.alpha_mode="cutout",
        .material=noemancer::SpriteMaterialChannels{.normal_texture_asset="texture.courier.normal",
            .emissive_mask_texture_asset="texture.courier.emissive",.depth_texture_asset="texture.courier.depth",
            .normal_strength=0.75F,.emissive_r=0.2F,.emissive_g=0.5F,.emissive_b=1.0F,
            .emissive_intensity=2.0F,.depth_bias=0.0005F,.shading_model="lit",.metallic=0.35F,
            .roughness=0.45F,.receives_shadows=true,.casts_shadows=true},
        .frame={.id="idle.0",.x=16,.y=0,.width=16,.height=24,.trim_x=2,.trim_y=4,
            .source_width=20,.source_height=28,.pivot_x=0.5F,.pivot_y=0.1F}};
    sprite_entity.tilemap_cells.push_back({.stable_id="entity.sprite.courier/tile/ground/9,-1",.tilemap_asset="tilemap.level",
        .layer_id="ground",.sorting_layer="terrain",.sorting_order=-2,.cell_x=9,.cell_y=-1,
        .cell_width=0.5F,.cell_height=0.75F,.tile_id="terrain",.autotile_group="terrain",.autotile_mask=10,
        .flip_x=true,.sprite_frame=*sprite_entity.sprite_frame});
    sprite_entity.tilemap_total_cell_count=1;
    const auto sprite_snapshot=noemancer::RenderWorldExtractor::extract(10,44,{sprite_entity});
    const auto sprite_json=nlohmann::json::parse(noemancer::render_world_json(sprite_snapshot));
    if(sprite_snapshot.sprites.size()!=1||sprite_snapshot.sprites[0].uv_rect!=std::array<float,4>{0.25F,0.0F,0.5F,0.75F}||
       sprite_snapshot.sprites[0].sorting_order!=7||sprite_json.at("sprites").at(0).at("frameId")!="idle.0"||
       sprite_snapshot.sprites[0].normal_texture_asset!="texture.courier.normal"||
       sprite_snapshot.sprites[0].shading_model!="lit"||sprite_snapshot.sprites[0].metallic!=0.35F||
       sprite_snapshot.sprites[0].roughness!=0.45F||!sprite_snapshot.sprites[0].receives_shadows||
       !sprite_snapshot.sprites[0].casts_shadows||
       sprite_json.at("sprites").at(0).at("material").at("depthTextureAsset")!="texture.courier.depth"||
       sprite_json.at("sprites").at(0).at("material").at("emissiveIntensity")!=2.0F||
       sprite_json.at("sprites").at(0).at("material").at("shadingModel")!="lit"||
       sprite_json.at("sprites").at(0).at("material").at("metallic")!=0.35F||
       sprite_json.at("sprites").at(0).at("material").at("roughness")!=0.45F||
       sprite_json.at("sprites").at(0).at("material").at("receivesShadows")!=true||
       sprite_json.at("sprites").at(0).at("material").at("castsShadows")!=true||
       sprite_snapshot.tilemaps.size()!=1||sprite_json.at("tilemaps").at(0).at("tilemapAsset")!="tilemap.level"||
       sprite_snapshot.tile_cells.size()!=1||sprite_snapshot.tile_cells[0].local_rect!=std::array<float,4>{4.5F,0.0F,5.0F,-0.75F}||
       sprite_snapshot.tile_cells[0].shading_model!="lit"||sprite_snapshot.tile_cells[0].metallic!=0.35F||
       sprite_snapshot.tile_cells[0].roughness!=0.45F||!sprite_snapshot.tile_cells[0].receives_shadows||
       !sprite_snapshot.tile_cells[0].casts_shadows||
       sprite_json.at("tileCells").at(0).at("stableId")!="entity.sprite.courier/tile/ground/9,-1"||
       sprite_json.at("tileCells").at(0).at("tileId")!="terrain"||sprite_json.at("tileCells").at(0).at("autotile").at("neighborMask")!=10||
       sprite_json.at("tileCells").at(0).at("chunk").at("position")!=nlohmann::json::array({0,0})||
       sprite_json.at("tileCells").at(0).at("material").at("shadingModel")!="lit"||
       sprite_json.at("tileCells").at(0).at("material").at("metallic")!=0.35F||
       sprite_json.at("tileCells").at(0).at("material").at("roughness")!=0.45F||
       sprite_json.at("tileCells").at(0).at("material").at("receivesShadows")!=true||
       sprite_json.at("tileCells").at(0).at("material").at("castsShadows")!=true||
       sprite_json.at("tilemaps").at(0).at("resolvedCellCount")!=1||
       sprite_json.at("spriteSortPolicy")!="sorting-layer/sorting-order/stable-id") {
        std::cerr<<"Resolved sprites did not cross the renderer-neutral extraction boundary\n";
        return 11;
    }
    // A cooked SpriteAtlas page is still renderer-neutral at this boundary:
    // the runtime overlay changes only the texture identity, page-local rect
    // and derived UVs; the source Sprite authoring contract remains intact.
    auto atlas_entity=sprite_entity;
    atlas_entity.id="entity.sprite.courier.atlas-page";
    atlas_entity.tilemap_renderer.reset();
    atlas_entity.tilemap_cells.clear();
    atlas_entity.sprite_frame->texture_asset="texture.courier.atlas.page.0";
    atlas_entity.sprite_frame->texture_width=32U;
    atlas_entity.sprite_frame->texture_height=32U;
    atlas_entity.sprite_frame->frame.x=2U;
    atlas_entity.sprite_frame->frame.y=3U;
    atlas_entity.sprite_frame->frame.width=16U;
    atlas_entity.sprite_frame->frame.height=24U;
    const auto atlas_snapshot=noemancer::RenderWorldExtractor::extract(10,45,{atlas_entity});
    const auto atlas_json=nlohmann::json::parse(noemancer::render_world_json(atlas_snapshot));
    if(atlas_snapshot.sprites.size()!=1U||atlas_snapshot.sprites[0].texture_asset!="texture.courier.atlas.page.0"||
       atlas_snapshot.sprites[0].texture_size!=std::array<std::uint32_t,2>{32U,32U}||
       atlas_snapshot.sprites[0].pixel_rect!=std::array<std::uint32_t,4>{2U,3U,16U,24U}||
       atlas_snapshot.sprites[0].uv_rect!=std::array<float,4>{2.0F/32.0F,3.0F/32.0F,18.0F/32.0F,27.0F/32.0F}||
       atlas_json.at("sprites").at(0).at("textureAsset")!="texture.courier.atlas.page.0"||
       atlas_json.at("sprites").at(0).at("pixelRect")!=nlohmann::json::array({2U,3U,16U,24U})) {
        std::cerr<<"SpriteAtlas page binding did not preserve renderer-neutral page-local geometry\n";
        return 18;
    }
    noemancer::TilemapRenderBakeCache bake_cache;
    auto cache_entity=sprite_entity;cache_entity.tilemap_cells[0].chunk_content_fingerprint="chunk-a";
    cache_entity.tilemap_early_visibility_applied=true;cache_entity.tilemap_resolved_chunk_count=2;
    cache_entity.tilemap_skipped_chunk_count=3;cache_entity.tilemap_cells_skipped_before_resolution=96;
    auto second_chunk=cache_entity.tilemap_cells[0];second_chunk.stable_id="second-chunk";second_chunk.cell_x=20;second_chunk.chunk_x=1;
    second_chunk.chunk_content_fingerprint="chunk-b";cache_entity.tilemap_cells.push_back(std::move(second_chunk));
    const auto first_bake=noemancer::RenderWorldExtractor::extract(10,45,{cache_entity},{},&bake_cache);
    const auto cached_bake=noemancer::RenderWorldExtractor::extract(10,46,{cache_entity},{},&bake_cache);
    auto offscreen_entity=cache_entity;offscreen_entity.tilemap_cells.pop_back();
    const auto offscreen_bake=noemancer::RenderWorldExtractor::extract(10,47,{offscreen_entity},{},&bake_cache);
    auto dirty_entity=cache_entity;dirty_entity.tilemap_cells[0].chunk_content_fingerprint="chunk-a-edited";
    const auto dirty_bake=noemancer::RenderWorldExtractor::extract(11,48,{dirty_entity},{},&bake_cache);
    auto moved_entity=dirty_entity;moved_entity.transform->x+=1.0F;
    const auto rebuilt_bake=noemancer::RenderWorldExtractor::extract(12,49,{moved_entity},{},&bake_cache);
    const auto evicted_bake=noemancer::RenderWorldExtractor::extract(13,50,{}, {},&bake_cache);
    const auto cached_json=nlohmann::json::parse(noemancer::render_world_json(cached_bake));
    noemancer::TilemapRenderBakeCache expiry_cache;expiry_cache.begin_frame();
    expiry_cache.store("offscreen","signature",std::vector<noemancer::RenderTileCellSnapshot>{{}});static_cast<void>(expiry_cache.end_frame(true));
    std::size_t expiry_evictions=0;for(int frame=0;frame<121;++frame){expiry_cache.begin_frame();expiry_evictions=expiry_cache.end_frame(true);}
    if(first_bake.tilemap_bake_cache_rebuilds!=2||first_bake.tilemap_bake_cache_hits!=0||cached_bake.tilemap_bake_cache_hits!=2||
       cached_bake.tilemap_bake_cache_rebuilds!=0||offscreen_bake.tilemap_bake_cache_hits!=1||offscreen_bake.tilemap_bake_retained_offscreen_chunks!=1||
       offscreen_bake.tilemap_bake_cached_chunks!=2||offscreen_bake.tilemap_bake_cache_evictions!=0||offscreen_bake.tile_chunk_ranges.size()!=1||
       first_bake.tile_chunk_ranges.size()!=2||first_bake.tile_chunk_ranges[0].cell_count!=1||dirty_bake.tilemap_bake_cache_hits!=1||dirty_bake.tilemap_bake_cache_rebuilds!=1||
       rebuilt_bake.tilemap_bake_cache_rebuilds!=2||rebuilt_bake.tile_cells[0].position[0]!=3.0F||
       evicted_bake.tilemap_bake_cache_evictions!=2||evicted_bake.tilemap_bake_cached_chunks!=0||
       cached_json.at("tilemapBakeCache").at("invalidation")!="chunk-content/sprite-frame/entity-transform/render-visibility"||
       cached_json.at("tilemapEarlyVisibility").at("stage")!="compiled-chunk-before-sprite-frame-resolution"||
       cached_json.at("tilemapEarlyVisibility").at("cellsSkippedBeforeResolution")!=96||expiry_evictions!=1||expiry_cache.size()!=0)return 14;
    noemancer::TilemapRenderBakeCache material_bake_cache;
    auto material_signature_entity=sprite_entity;
    material_signature_entity.tilemap_cells[0].chunk_content_fingerprint="material-chunk";
    const auto material_first=noemancer::RenderWorldExtractor::extract(20,1,{material_signature_entity},{},&material_bake_cache);
    auto changed_material=material_signature_entity;
    changed_material.tilemap_cells[0].sprite_frame.material->metallic=0.9F;
    const auto material_changed=noemancer::RenderWorldExtractor::extract(20,2,{changed_material},{},&material_bake_cache);
    if(material_first.tilemap_bake_cache_rebuilds!=1||material_first.tilemap_bake_cache_hits!=0||
       material_changed.tilemap_bake_cache_rebuilds!=1||material_changed.tilemap_bake_cache_hits!=0) {
        std::cerr<<"Sprite material changes did not invalidate the renderer-neutral tile chunk fingerprint\n";
        return 18;
    }
    auto culled_snapshot=sprite_snapshot;
    culled_snapshot.camera=noemancer::RenderCameraSnapshot{.entity_id="camera.editor",.position={5,0,10},.target={5,0,0},
        .near_clip=0.1F,.far_clip=100.0F,.projection="orthographic",.orthographic_height=10.0F};
    auto distant_cell=culled_snapshot.tile_cells.front();distant_cell.stable_id="distant";distant_cell.cell_x=200;distant_cell.chunk_x=25;
    distant_cell.local_rect={100.0F,0.0F,100.5F,-0.75F};culled_snapshot.tile_cells.push_back(std::move(distant_cell));
    noemancer::RenderWorldExtractor::cull_tilemap_chunks(culled_snapshot,1000,1000);
    const auto culled_json=nlohmann::json::parse(noemancer::render_world_json(culled_snapshot));
    if(!culled_snapshot.tilemap_chunk_culling_applied||culled_snapshot.tile_cells.size()!=1||
       culled_snapshot.tilemap_visible_chunk_count!=1||culled_snapshot.tilemap_culled_chunk_count!=1||
       culled_json.at("tilemapChunkCulling").at("cellsBefore")!=2||culled_json.at("tilemapChunkCulling").at("cellsAfter")!=1)return 12;
    const auto pressure=nlohmann::json::parse(noemancer::tilemap_pressure_report_json(8,8,16,2));
    const auto pressure_repeat=nlohmann::json::parse(noemancer::tilemap_pressure_report_json(8,8,16,2));
    if(!pressure.at("valid")||pressure.at("workload").at("totalCells")!=16384||
       pressure.at("culling").at("culledChunks").get<std::size_t>()==0||pressure.at("culling")!=pressure_repeat.at("culling")||
       pressure.at("submissionEstimate").at("estimatedDraws").get<std::size_t>()>=pressure.at("submissionEstimate").at("drawsWithoutInstancing").get<std::size_t>()||
       pressure.at("schemaVersion")!="noemancer.tilemap-pressure/0.3"||
       pressure.at("stableResidency").at("retainedOffsetsStable")!=pressure.at("stableResidency").at("retainedRanges")||
       pressure.at("stableResidency").at("rangeMoves")!=0||
       pressure.at("stableResidency").at("drawIndexBytes").get<std::size_t>()>=pressure.at("stableResidency").at("packedTailWorstCaseBytes").get<std::size_t>()||
       pressure.at("scope")!="deterministic-render-extraction-and-residency-simulation-not-gpu-timing")return 13;
    const auto sparse_pressure=nlohmann::json::parse(noemancer::tilemap_pressure_report_json(64,64,32,3,8));
    if(!sparse_pressure.at("valid")||sparse_pressure.at("workload").at("totalChunks")!=4096||
       sparse_pressure.at("workload").at("addressableCells")!=4194304||
       sparse_pressure.at("workload").at("occupiedCells")!=32768||
       sparse_pressure.at("workload").at("occupancyRatio").get<double>()!=0.0078125||
       sparse_pressure.at("culling").at("culledChunks").get<std::size_t>()==0||
       sparse_pressure.at("stableResidency").at("retainedOffsetsStable")!=sparse_pressure.at("stableResidency").at("retainedRanges"))return 19;
    const auto sprite_pressure=nlohmann::json::parse(noemancer::sprite_pressure_report_json(1024,8,256,64,16));
    if(!sprite_pressure.at("valid")||sprite_pressure.at("workload").at("frames")!=1024||
       sprite_pressure.at("workload").at("totalClipFrameReferences")!=2048||
       sprite_pressure.at("atlas").at("pageCount")!=1||sprite_pressure.at("atlas").at("overlapArea")!=0||
       !sprite_pressure.at("pagePlan").at("valid")||sprite_pressure.at("pagePlan").at("pageCount").get<std::size_t>()<1||
       sprite_pressure.at("scope")!="deterministic-source-layout-and-reference-pressure-not-gpu-timing")return 20;
    return 0;
}
