#include "engine/animation_graph.hpp"
#include "engine/animation_graph_patch.hpp"
#include "engine/asset_registry.hpp"
#include "engine/command_registry.hpp"
#include "engine/scene_document.hpp"
#include "engine/vfs_document_reader.hpp"
#include "engine/virtual_file_system.hpp"
#include "engine/world.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {

constexpr auto valid_graph = R"({
  "schemaVersion":"noemancer.animation-graph/0.1",
  "assetId":"animation.graph.test",
  "parameters":[{"id":"speed","type":"float","default":0},{"id":"aimWeight","type":"float","default":0}],
  "nodes":[
    {"id":"idle","kind":"clip","clipAsset":"asset.animation.idle","looping":true},
    {"id":"run","kind":"clip","clipAsset":"asset.animation.run","looping":true},
    {"id":"locomotion","kind":"blend-1d","parameter":"speed","children":[{"nodeId":"run","threshold":1},{"nodeId":"idle","threshold":0}]},
    {"id":"aim","kind":"state-machine","stateMachineAsset":"animation.machine.aim"}
  ],
  "layers":[
    {"id":"base","rootNode":"locomotion","mode":"override","weight":1,"syncGroup":"locomotion"},
    {"id":"upper","rootNode":"aim","mode":"additive","weight":1,"weightParameter":"aimWeight","maskId":"upper-body"}
  ],
  "masks":[{"id":"upper-body","includeDescendants":true,"joints":[{"name":"spine","weight":1}]}],
  "syncGroups":[{"id":"locomotion","mode":"normalized-time"}],
  "editor":{"nodes":[{"id":"locomotion","position":[80,160],"collapsed":false}],"zoom":1,"pan":[0,0]}
})";

} // namespace

int main() {
    using Json = nlohmann::json;
    const auto parsed = noemancer::AnimationGraphCodec::parse_json(valid_graph);
    if (!parsed || parsed.document->nodes.size() != 4 || parsed.document->layers.size() != 2) {
        std::cerr << "Valid Animation Graph failed to parse: " << parsed.code << '\n'; return 1;
    }
    const auto canonical = Json::parse(noemancer::AnimationGraphCodec::write_canonical_json(*parsed.document));
    if (canonical.at("schemaVersion").get<std::string>() != noemancer::animation_graph_schema ||
        canonical.at("nodes").at(2).at("children").at(0).at("nodeId") != "idle") return 2;
    const auto dependencies = noemancer::AnimationGraphCodec::asset_dependencies(*parsed.document);
    if (dependencies != std::vector<std::string>{"animation.machine.aim", "asset.animation.idle", "asset.animation.run"}) return 3;
    const auto midpoint = noemancer::AnimationGraphCodec::select_blend_1d(*parsed.document, "locomotion", {{"speed", 0.25F}});
    if (!midpoint.valid || midpoint.first_node != "idle" || midpoint.second_node != "run" ||
        midpoint.first_weight != 0.75F || midpoint.second_weight != 0.25F) return 4;
    const auto clamped = noemancer::AnimationGraphCodec::select_blend_1d(*parsed.document, "locomotion", {{"speed", 4.0F}});
    if (!clamped.valid || clamped.first_node != "run" || clamped.second_weight != 0.0F) return 5;

    auto duplicate = Json::parse(valid_graph); duplicate["nodes"].push_back(duplicate["nodes"].at(0));
    if (noemancer::AnimationGraphCodec::parse_json(duplicate.dump()).code != "animation.graph.node-invalid") return 6;
    auto missing = Json::parse(valid_graph); missing["nodes"].at(2)["children"].at(0)["nodeId"] = "missing";
    if (noemancer::AnimationGraphCodec::parse_json(missing.dump()).code != "animation.graph.node-reference-invalid") return 7;
    auto cycle = Json::parse(valid_graph);
    cycle["nodes"].at(0) = {{"id", "idle"}, {"kind", "blend-1d"}, {"parameter", "speed"},
        {"children", Json::array({{{"nodeId", "locomotion"}, {"threshold", 0}}, {{"nodeId", "run"}, {"threshold", 1}}})}};
    if (noemancer::AnimationGraphCodec::parse_json(cycle.dump()).code != "animation.graph.nested-blend-unsupported") return 8;
    auto bad_mask = Json::parse(valid_graph); bad_mask["masks"].at(0)["joints"].at(0)["weight"] = 2;
    if (noemancer::AnimationGraphCodec::parse_json(bad_mask.dump()).code != "animation.graph.mask-invalid") return 9;
    auto bad_layer = Json::parse(valid_graph); bad_layer["layers"].at(1)["maskId"] = "missing";
    if (noemancer::AnimationGraphCodec::parse_json(bad_layer.dump()).code != "animation.graph.layer-invalid") return 10;
    auto malformed = Json::parse(valid_graph); malformed["editor"]["pan"][0] = "bad";
    if (noemancer::AnimationGraphCodec::parse_json(malformed.dump()).code != "animation.graph.type-invalid") return 16;
    auto multiple_machines = Json::parse(valid_graph);multiple_machines["nodes"].push_back(
        {{"id","aim-secondary"},{"kind","state-machine"},{"stateMachineAsset","animation.machine.secondary"}});
    if(noemancer::AnimationGraphCodec::parse_json(multiple_machines.dump()).code!=
       "animation.graph.state-machine-count-unsupported")return 17;
    auto weighted_base=Json::parse(valid_graph);weighted_base["layers"].at(0)["weight"]=0.5F;
    if(noemancer::AnimationGraphCodec::parse_json(weighted_base.dump()).code!="animation.graph.base-layer-invalid")return 18;
    auto machine_blend=Json::parse(valid_graph);machine_blend["nodes"].at(2)["children"].at(0)["nodeId"]="aim";
    if(noemancer::AnimationGraphCodec::parse_json(machine_blend.dump()).code!="animation.graph.nested-blend-unsupported")return 19;
    auto bad_order=Json::parse(valid_graph);bad_order["layers"].push_back(
        {{"id","late-override"},{"rootNode","idle"},{"mode","override"},{"weight",0.5F}});
    if(noemancer::AnimationGraphCodec::parse_json(bad_order.dump()).code!="animation.graph.layer-order-unsupported")return 20;
    auto huge_layout=Json::parse(valid_graph);huge_layout["editor"]["nodes"][0]["position"][0]=1.0e20F;
    if(noemancer::AnimationGraphCodec::parse_json(huge_layout.dump()).code!="animation.graph.editor-invalid")return 21;

    const auto scene = noemancer::SceneDocumentCodec::parse_json(R"({
      "schema":"noemancer.scene/0.1","sceneGuid":"scene.animation-graph","name":"Animation Graph",
      "entities":[{"guid":"entity.actor","name":"Actor","parent":null,"components":{
        "Transform":{"position":[0,0,0]},
        "AnimationPlayer":{"clipAsset":"asset.animation.idle","playbackSpeed":1,"looping":true,"playing":true,
          "rootMotionMode":"ignore","animationGraphAsset":"animation.graph.test"}
      }}]
    })");
    if (!scene || !scene.document->entities.front().animation_player ||
        scene.document->entities.front().animation_player->animation_graph_asset != "animation.graph.test") return 11;
    const auto roundtrip = noemancer::SceneDocumentCodec::parse_json(
        noemancer::SceneDocumentCodec::write_canonical_json(*scene.document));
    if (!roundtrip || roundtrip.document->entities.front().animation_player->animation_graph_asset != "animation.graph.test") return 12;
    noemancer::World world;
    if (!world.load_scene(*scene.document).success || !world.register_animation_graph(*parsed.document)) return 13;
    if(!world.entity_views().front().animation_player||
       world.entity_views().front().animation_player->state_machine_asset!="animation.machine.aim")return 22;
    const auto observation = Json::parse(world.animation_graph_json("entity.actor"));
    if (!observation.at("valid") || observation.at("instance").at("parameters").at("speed") != 0.0F) return 14;
    const auto receipt = Json::parse(world.animation_graph_parameter_set_json("entity.actor", "speed", 0.5F));
    if (!receipt.at("success") || receipt.at("after") != 0.5F ||
        Json::parse(world.animation_graph_json("entity.actor")).at("instance").at("parameters").at("speed") != 0.5F) return 15;

    const auto tool_root=std::filesystem::temp_directory_path()/"noemancer-animation-graph-command-test";
    std::filesystem::remove_all(tool_root);std::filesystem::create_directories(tool_root);
    {std::ofstream output(tool_root/"registry.json");output<<R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"clip.idle","displayName":"Idle","kind":"Animation","uri":"builtin://animation/test-bob","license":"CC0","redistribution":"allowed"},
      {"id":"animation.graph.test","displayName":"Graph","kind":"AnimationGraph","uri":"asset://graph.animation-graph.json",
       "path":"graph.animation-graph.json","license":"CC0","redistribution":"allowed"}]})";}
    {std::ofstream output(tool_root/"graph.animation-graph.json");output<<valid_graph;}
    noemancer::AssetRegistry assets(tool_root);noemancer::CommandRegistry commands(world,assets);
    noemancer::VirtualFileSystem document_vfs;
    if(!document_vfs.mount({"animation-graph-test","asset://roots/0",tool_root,
        noemancer::VfsMountKind::directory,0,true}).success)return 33;
    std::size_t attached_read_count{};
    const auto attach_vfs_reader=[&](noemancer::CommandRegistry& target) {
        target.attach_asset_document_reader([&](const std::string_view asset_id,const std::size_t byte_budget) {
            ++attached_read_count;
            const auto document=noemancer::read_vfs_document(document_vfs,{
                .uri="asset://roots/0/graph.animation-graph.json",
                .kind=noemancer::VfsDocumentKind::text,
                .byte_budget=byte_budget});
            return noemancer::AssetDocumentReadResult{document.success,document.code,document.detail,
                std::string(asset_id),document.sha256,document.text};
        });
    };
    attach_vfs_reader(commands);
    const auto attached_inspection=Json::parse(
        commands.invoke("animation.graph.inspect",R"({"assetId":"animation.graph.test"})").output_json);
    if(!attached_inspection.at("ok")||!attached_inspection.at("result").at("valid")||
       attached_read_count!=1U||attached_inspection.at("result").at("definition").at("assetId")!=
           "animation.graph.test")return 34;
    noemancer::CommandRegistry detached_commands(world,assets);
    const auto detached_inspection=Json::parse(
        detached_commands.invoke("animation.graph.inspect",R"({"assetId":"animation.graph.test"})").output_json);
    if(!detached_inspection.at("ok")||!detached_inspection.at("result").at("valid"))return 35;
    commands.attach_asset_document_reader([](const std::string_view asset_id,const std::size_t) {
        return noemancer::AssetDocumentReadResult{false,"asset-read.hash-mismatch",
            "VFS content identity mismatch.",std::string(asset_id),{}, {}};
    });
    const auto failed_inspection=Json::parse(
        commands.invoke("animation.graph.inspect",R"({"assetId":"animation.graph.test"})").output_json);
    if(failed_inspection.at("result").at("valid")||
       failed_inspection.at("result").at("code")!="asset-read.hash-mismatch"||
       !failed_inspection.at("result").at("definition").is_null())return 36;
    attach_vfs_reader(commands);
    auto wrong_dependency=Json::parse(valid_graph);
    for(auto& node:wrong_dependency["nodes"])if(node.value("id",std::string{})=="aim")node["stateMachineAsset"]="clip.idle";
    const auto wrong_dependency_validation=assets.validate_animation_graph_source("animation.graph.test",wrong_dependency.dump());
    if(wrong_dependency_validation.valid||wrong_dependency_validation.code!="animation.graph-state-machine-dependency-kind-invalid")return 27;
    auto oversized_field=Json::parse(valid_graph);oversized_field["nodes"][0]["clipAsset"]=std::string(4097,'x');
    const auto oversized_validation=assets.validate_animation_graph_source("animation.graph.test",oversized_field.dump());
    if(oversized_validation.valid||oversized_validation.code!="animation.graph-string-too-large")return 28;
    const auto before_fingerprint=noemancer::AnimationGraphPatch::fingerprint(*parsed.document);
    const Json patch_arguments={{"assetId","animation.graph.test"},{"manager","test.animation-graph"},
        {"expectedFingerprint",before_fingerprint},{"dryRun",false},{"operations",Json::array({
            {{"operation","createNode"},{"nodeId","walk"},{"kind","clip"},{"clipAsset","asset.animation.walk"},{"looping",true}},
            {{"operation","connectBlend1DChild"},{"blendNodeId","locomotion"},{"childNodeId","walk"},{"threshold",0.5F}},
            {{"operation","setNodePosition"},{"nodeId","walk"},{"x",222.0F},{"y",333.0F}}})}};
    auto extra_field_arguments=patch_arguments;extra_field_arguments["dryRun"]=true;
    extra_field_arguments["operations"]=Json::array({{{"operation","setNodePosition"},{"nodeId","locomotion"},
        {"x",1.0F},{"y",2.0F},{"unexpected",true}}});
    const auto extra_field=commands.invoke("animation.graph.patch",extra_field_arguments.dump());
    if(extra_field.exit_code==0||extra_field.output_json.find("Unexpected field") == std::string::npos)return 29;
    auto excessive_operations=patch_arguments;excessive_operations["dryRun"]=true;
    excessive_operations["operations"]=Json::array();
    for(std::size_t index=0;index<=noemancer::animation_graph_patch_max_operations;++index)
        excessive_operations["operations"].push_back({{"operation","setNodePosition"},{"nodeId","locomotion"},
            {"x",static_cast<float>(index)},{"y",0.0F}});
    const auto excessive_operation_result=commands.invoke("animation.graph.patch",excessive_operations.dump());
    if(excessive_operation_result.exit_code==0||
       excessive_operation_result.output_json.find("more than 256 operations")==std::string::npos)return 30;
    auto oversized_dry_run=patch_arguments;oversized_dry_run["dryRun"]=true;
    oversized_dry_run["operations"]=Json::array({{{"operation","createNode"},
        {"nodeId",std::string(noemancer::animation_graph_patch_max_string_bytes+1U,'x')},
        {"kind","clip"},{"clipAsset","asset.animation.walk"}}});
    const auto oversized_dry_run_result=commands.invoke("animation.graph.patch",oversized_dry_run.dump());
    if(oversized_dry_run_result.exit_code==0||
       oversized_dry_run_result.output_json.find("animation.graph.patch-string-too-large")==std::string::npos)return 31;
    auto excessive_children=patch_arguments;excessive_children["dryRun"]=true;
    excessive_children["operations"]=Json::array({{{"operation","createNode"},{"nodeId","too-many-children"},
        {"kind","blend-1d"},{"parameter","speed"},{"children",Json::array()}}});
    for(std::size_t index=0;index<=noemancer::animation_graph_patch_max_children;++index)
        excessive_children["operations"][0]["children"].push_back({{"nodeId","child-"+std::to_string(index)},
            {"threshold",static_cast<float>(index)}});
    const auto excessive_children_result=commands.invoke("animation.graph.patch",excessive_children.dump());
    if(excessive_children_result.exit_code==0||
       excessive_children_result.output_json.find("more than 32 children")==std::string::npos)return 32;
    const auto committed=Json::parse(commands.invoke("animation.graph.patch",patch_arguments.dump()).output_json);
    if(!committed.at("result").value("success",false)||!committed.at("result").value("runtimeReloaded",false)||
       committed.at("result").at("source").get<std::string>().find("graph.animation-graph.json")==std::string::npos)return 19;
    std::ifstream committed_stream(tool_root/"graph.animation-graph.json",std::ios::binary);
    const std::string committed_text{std::istreambuf_iterator<char>(committed_stream),std::istreambuf_iterator<char>()};
    committed_stream.close();
    const auto committed_source=noemancer::AnimationGraphCodec::parse_json(committed_text);
    if(!committed_source||std::ranges::find(committed_source.document->nodes,"walk",
        &noemancer::AnimationGraphNode::id)==committed_source.document->nodes.end())return 24;
    const auto stale=Json::parse(commands.invoke("animation.graph.patch",patch_arguments.dump()).output_json);
    if(stale.at("result").value("success",true)||stale.at("result").value("code",std::string{})!="animation.graph.patch-conflict")return 20;
    const auto undo=Json::parse(commands.invoke("asset.source.undo",R"({"manager":"test.undo"})").output_json);
    if(!undo.at("result").value("success",false)) {std::cerr<<undo.dump()<<'\n';return 25;}
    const auto redo=Json::parse(commands.invoke("asset.source.redo",R"({"manager":"test.redo"})").output_json);
    if(!redo.at("result").value("success",false)) {std::cerr<<redo.dump()<<'\n';return 26;}
    {std::ofstream output(tool_root/"graph.animation-graph.json",std::ios::trunc);auto mismatch=Json::parse(valid_graph);
        mismatch["assetId"]="animation.graph.other";output<<mismatch.dump();}
    static_cast<void>(assets.refresh());
    const auto mismatch=Json::parse(commands.invoke("animation.graph.patch",Json{{"assetId","animation.graph.test"},{"manager","test"},
        {"dryRun",true},{"operations",Json::array({{{"operation","setNodePosition"},{"nodeId","locomotion"},{"x",1},{"y",2}}})}}.dump()).output_json);
    if(mismatch.at("result").value("code",std::string{})!="animation.graph-identity-mismatch")return 22;
    {std::ofstream output(tool_root/"graph.animation-graph.json",std::ios::trunc);output<<R"({"schemaVersion":"noemancer.animation-graph/0.1","assetId":12})";}
    try {static_cast<void>(assets.refresh());} catch(...) {return 23;}
    std::filesystem::remove_all(tool_root);
    return 0;
}
