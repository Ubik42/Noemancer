#include "engine/vfx_runtime.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <vector>

int main() {
    using Json = nlohmann::json;
    noemancer::VfxRuntime runtime(4096);
    const auto default_graph = noemancer::VfxRuntime::default_graph_json();
    const auto load = runtime.load_graph_json(default_graph);
    if (!load.success || load.graph_id != "vfx.debug-impact") return 1;
    const auto alpha_load = runtime.load_graph_json(noemancer::VfxRuntime::default_alpha_graph_json());
    const auto alpha_graph = Json::parse(runtime.graph_json("vfx.debug-smoke"));
    if (!alpha_load.success || !alpha_graph.at("valid") ||
        alpha_graph.at("nodes").at(3).at("parameters").at("blendMode") != "alpha" ||
        alpha_graph.at("nodes").at(3).at("parameters").at("pixelAlignment") != "profile" ||
        alpha_graph.at("nodes").at(3).at("parameters").at("sizeQuantization") != "profile" ||
        alpha_graph.at("nodes").at(3).at("parameters").at("sampling") != "profile") return 17;

    auto legacy_graph = Json::parse(default_graph);
    legacy_graph["graphId"] = "vfx.debug-legacy";
    legacy_graph["nodes"].at(3)["parameters"].erase("pixelAlignment");
    legacy_graph["nodes"].at(3)["parameters"].erase("sizeQuantization");
    legacy_graph["nodes"].at(3)["parameters"].erase("sampling");
    const auto legacy_load = runtime.load_graph_json(legacy_graph.dump());
    const auto legacy_canonical = Json::parse(runtime.graph_json("vfx.debug-legacy"));
    const auto& legacy_policy = legacy_canonical.at("nodes").at(3).at("parameters");
    if (!legacy_load.success || legacy_policy.at("pixelAlignment") != "profile" ||
        legacy_policy.at("sizeQuantization") != "profile" || legacy_policy.at("sampling") != "profile") return 18;

    auto authored_policy_graph = Json::parse(default_graph);
    authored_policy_graph["graphId"] = "vfx.debug-pixel-policy";
    auto& authored_policy = authored_policy_graph["nodes"].at(3)["parameters"];
    authored_policy["pixelAlignment"] = "none";
    authored_policy["sizeQuantization"] = "none";
    authored_policy["sampling"] = "linear";
    const auto authored_policy_load = runtime.load_graph_json(authored_policy_graph.dump());
    const auto authored_canonical = Json::parse(runtime.graph_json("vfx.debug-pixel-policy"));
    const auto& authored_output = authored_canonical.at("nodes").at(3).at("parameters");
    if (!authored_policy_load.success || authored_output.at("pixelAlignment") != "none" ||
        authored_output.at("sizeQuantization") != "none" || authored_output.at("sampling") != "linear") return 19;
    noemancer::VfxRuntime roundtrip_runtime(4096);
    if (!roundtrip_runtime.load_graph_json(authored_canonical.dump()).success ||
        Json::parse(roundtrip_runtime.graph_json("vfx.debug-pixel-policy")) != authored_canonical) return 20;

    const auto graph = Json::parse(runtime.graph_json(load.graph_id));
    if (!graph.at("valid") || graph.at("schemaVersion") != "noemancer.vfx-graph/0.1" ||
        graph.at("execution").at("preferred") != "gpu-compute" || graph.at("nodes").size() != 4 ||
        graph.at("nodes").at(3).at("parameters").at("pixelAlignment") != "profile" ||
        graph.at("nodes").at(3).at("parameters").at("sizeQuantization") != "profile" ||
        graph.at("nodes").at(3).at("parameters").at("sampling") != "profile") return 2;
    const auto gpu_program = Json::parse(runtime.gpu_program_json(load.graph_id));
    const auto& gpu_sprite_policy = gpu_program.at("spritePolicy");
    if (!gpu_program.at("valid") || gpu_program.at("abi") != "structured-particle-gpu-lifecycle-indirect/0.7" ||
        gpu_sprite_policy.at("pixelAlignment") != "profile" ||
        gpu_sprite_policy.at("sizeQuantization") != "profile" || gpu_sprite_policy.at("sampling") != "profile" ||
        gpu_program.at("kernel").at("artifactStem") != "vfx_sim.comp" ||
        gpu_program.at("kernel").at("spawnArtifactStem") != "vfx_spawn.comp" ||
        gpu_program.at("kernel").at("groupArtifactStem") != "vfx_group.comp" ||
        gpu_program.at("kernel").at("sortArtifactStem") != "vfx_sort_alpha.comp" ||
        gpu_program.at("kernel").at("threadGroup").at(0) != 64 || gpu_program.at("buffers").size() != 13 ||
        gpu_program.at("workingSetBytes") != 499792 || gpu_program.at("execution").at("dispatchActive") != false ||
        gpu_program.at("execution").at("randomInitialization") != "gpu-stateless-u32-hash/seed-particle-index-channel" ||
        gpu_program.at("execution").at("alphaSort") != "gpu-bitonic-multi-dispatch/back-to-front/stable-particle-id/dynamic-span/max-8192" ||
        gpu_program.at("graphicsConsumer").at("submission") != "dual-indirect-instanced") return 16;
    const auto authored_gpu_program = Json::parse(runtime.gpu_program_json("vfx.debug-pixel-policy"));
    if (!authored_gpu_program.at("valid") || authored_gpu_program.at("spritePolicy").at("pixelAlignment") != "none" ||
        authored_gpu_program.at("spritePolicy").at("sizeQuantization") != "none" ||
        authored_gpu_program.at("spritePolicy").at("sampling") != "linear") return 21;

    const auto first = Json::parse(runtime.preview_json(load.graph_id, 42, 20, 1.0F / 60.0F, 8));
    const auto repeated = Json::parse(runtime.preview_json(load.graph_id, 42, 20, 1.0F / 60.0F, 8));
    const auto changed = Json::parse(runtime.preview_json(load.graph_id, 43, 20, 1.0F / 60.0F, 8));
    if (!first.at("valid") || first.at("digest") != repeated.at("digest") ||
        !first.at("particles").at(0).contains("spawnProvenance") ||
        first.at("particles") != repeated.at("particles") || first.at("digest") == changed.at("digest")) {
        std::cerr << "Fixed-seed VFX preview is not deterministic\n";
        return 3;
    }
    if (first.at("aliveCount") != 48 || !first.at("truncated") || first.at("particles").size() != 8) return 4;

    if (!runtime.bind_event("combat.hit", load.graph_id)) return 5;
    std::vector<noemancer::GameplayEvent> events{{7, "combat.hit", "entity.attacker", "entity.target",
        R"({"position":{"x":3,"y":2,"z":1}})"}};
    runtime.consume_gameplay_events(events);
    runtime.consume_gameplay_events(events);
    runtime.tick(1.0F / 60.0F);
    const auto observation = Json::parse(runtime.observe_json(4));
    bool found_authored_sprite_policy = false;
    for (const auto& policy : observation.at("spritePolicies")) {
        if (policy.at("graphId") == "vfx.debug-pixel-policy") {
            found_authored_sprite_policy = policy.at("pixelAlignment") == "none" &&
                policy.at("sizeQuantization") == "none" && policy.at("sampling") == "linear";
        }
    }
    if (observation.at("aliveCount") != 48 || observation.at("lastEventSequence") != 7 ||
        observation.at("particles").at(0).at("position").at("x").get<float>() < 2.9F ||
        observation.at("gpuContract") != "soa-pool-data-channel-indirect/0.1"||
        observation.at("poolLayout")!="structure-of-arrays/0.1" || !found_authored_sprite_policy) return 6;
    const auto benchmark=Json::parse(runtime.benchmark_json(load.graph_id,16384,30,1.0F/120.0F));
    const auto benchmark_repeat=Json::parse(runtime.benchmark_json(load.graph_id,16384,30,1.0F/120.0F));
    if(!benchmark.at("valid")||benchmark.at("layout")!="structure-of-arrays/0.1"||
       benchmark.at("particleSteps")!=491520||benchmark.at("digest")!=benchmark_repeat.at("digest")||
       benchmark.at("scope")!="cpu-deterministic-reference-not-gpu-performance") return 17;

    auto invalid = Json::parse(noemancer::VfxRuntime::default_graph_json());
    invalid["capacity"] = 0;
    const auto invalid_result = runtime.load_graph_json(invalid.dump());
    if (invalid_result.success || invalid_result.code != "vfx.graph.invalid-capacity") return 7;

    auto invalid_pixel_alignment = authored_policy_graph;
    invalid_pixel_alignment["graphId"] = "vfx.debug-invalid-pixel-alignment";
    invalid_pixel_alignment["nodes"].at(3)["parameters"]["pixelAlignment"] = "snap";
    const auto invalid_pixel_alignment_result = runtime.load_graph_json(invalid_pixel_alignment.dump());
    if (invalid_pixel_alignment_result.success || invalid_pixel_alignment_result.code != "vfx.graph.invalid-parameter") return 22;
    auto invalid_size_quantization = authored_policy_graph;
    invalid_size_quantization["graphId"] = "vfx.debug-invalid-size-quantization";
    invalid_size_quantization["nodes"].at(3)["parameters"]["sizeQuantization"] = "quantized";
    const auto invalid_size_quantization_result = runtime.load_graph_json(invalid_size_quantization.dump());
    if (invalid_size_quantization_result.success || invalid_size_quantization_result.code != "vfx.graph.invalid-parameter") return 23;
    auto invalid_sampling = authored_policy_graph;
    invalid_sampling["graphId"] = "vfx.debug-invalid-sampling";
    invalid_sampling["nodes"].at(3)["parameters"]["sampling"] = "nearest";
    const auto invalid_sampling_result = runtime.load_graph_json(invalid_sampling.dump());
    if (invalid_sampling_result.success || invalid_sampling_result.code != "vfx.graph.invalid-parameter") return 24;

    noemancer::VfxRuntime constrained(16);
    auto constrained_graph = Json::parse(noemancer::VfxRuntime::default_graph_json());
    constrained_graph["capacity"] = 16;
    constrained_graph["nodes"][0]["parameters"]["count"] = 16;
    if (!constrained.load_graph_json(constrained_graph.dump()).success || !constrained.bind_event("combat.hit", load.graph_id)) return 8;
    constrained.consume_gameplay_events(events);
    events[0].sequence = 8;
    constrained.consume_gameplay_events(events);
    const auto constrained_observation = Json::parse(constrained.observe_json());
    if (constrained_observation.at("aliveCount") != 16 || constrained_observation.at("droppedCount") != 16) return 9;

    const auto revision_before_patch = runtime.revision();
    const auto plan = Json::parse(runtime.plan_graph_patch_json(load.graph_id,
        R"({"nodes":[{"id":"spawn.burst","op":"spawn.burst","parameters":{"count":12}},{"id":"initialize","op":"particle.initialize","parameters":{"lifetime":{"min":0.5,"max":1.0},"speed":{"min":1.0,"max":3.0},"size":{"start":0.15,"end":0.0},"color":{"start":{"r":1,"g":0.7,"b":0.2,"a":1},"end":{"r":1,"g":0.1,"b":0,"a":0}}}},{"id":"forces","op":"particle.forces","parameters":{"gravity":{"x":0,"y":-9.81,"z":0},"drag":0.1}},{"id":"output","op":"render.sprite","parameters":{"blendMode":"alpha"}}]})",
        revision_before_patch));
    if (!plan.at("valid") || plan.at("comparison").at("changedPathCount").get<std::size_t>() == 0 ||
        plan.at("candidate").at("nodes").at(0).at("parameters").at("count") != 12) return 10;
    const auto dry_run = Json::parse(runtime.apply_graph_plan_json(plan.dump(), true));
    if (!dry_run.at("success") || runtime.revision() != revision_before_patch) return 11;
    auto tampered_plan = plan;
    tampered_plan["candidate"]["capacity"] = 1024;
    const auto tampered = Json::parse(runtime.apply_graph_plan_json(tampered_plan.dump(), true));
    if (tampered.at("success") || tampered.at("code") != "vfx.plan.integrity-failed") return 12;
    const auto applied = Json::parse(runtime.apply_graph_plan_json(plan.dump(), false));
    if (!applied.at("success") || runtime.revision() == revision_before_patch ||
        Json::parse(runtime.graph_json(load.graph_id)).at("nodes").at(0).at("parameters").at("count") != 12) return 13;
    const auto revision_after_apply = runtime.revision();
    const auto stale = Json::parse(runtime.apply_graph_plan_json(plan.dump(), false));
    if (stale.at("success") || stale.at("code") != "vfx.revision-conflict") return 14;
    const auto undo = Json::parse(runtime.undo_graph_json(revision_after_apply));
    if (!undo.at("success") || Json::parse(runtime.graph_json(load.graph_id)).at("nodes").at(0).at("parameters").at("count") != 48) return 15;
    return 0;
}
