#include "engine/retained_ui_runtime.hpp"
#include "engine/scene_document.hpp"
#include "engine/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <limits>

int main() {
    try {
    noemancer::World world;
    if (!world.load_scene(noemancer::make_bootstrap_scene_document()).success) return 1;

    const auto semantic_document = world.semantic_ui_document_json("entity.demo-cube", "zh-CN");
    const auto rml = noemancer::retained_ui_rml_from_semantic_document(semantic_document);
    if (rml.find("data-semantic-id=\"editor.inspector.entity.demo-cube.PbrMaterial.roughness\"") == std::string::npos ||
        rml.find("class=\"property-row\"") == std::string::npos||rml.find("data-action=\"world.property.plan\"")==std::string::npos||
        rml.find("&quot;entityId&quot;:&quot;entity.demo-cube&quot;")==std::string::npos||
        rml.find("type=\"checkbox\"")==std::string::npos||rml.find("PbrMaterial.roughness.editor")==std::string::npos||
        rml.find("class=\"axis-input vector-x\"")==std::string::npos||rml.find("data-local-action=\"toggle-group\"")==std::string::npos||
        rml.find("section.identity\" data-role=\"group\" data-enabled=\"true\" data-editable=\"false\" data-expanded=\"false\"")==std::string::npos) {
        std::cerr << "Semantic UI did not project to retained markup with stable identity\n";
        return 2;
    }
    auto combo_document=nlohmann::json::parse(semantic_document);
    for(auto& node:combo_document["nodes"])if(node.value("role",std::string{})=="property") {
        node["state"]["editable"]=true;node["value"]="option-b";node["binding"]["valueType"]="string";
        node["presentation"]={{"control","combo"},{"constraints",{{"options",{"option-a","option-b"}}}}};
        node["actions"]=nlohmann::json::array({{{"id","world.property.plan"}}});break;
    }
    const auto combo_rml=noemancer::retained_ui_rml_from_semantic_document(combo_document.dump());
    if(combo_rml.find("<select")==std::string::npos||combo_rml.find("option-b\" selected")==std::string::npos)return 33;
    const auto animation_rml=noemancer::retained_ui_rml_from_semantic_document(
        world.semantic_ui_document_json("entity.demo-skeletal-cube","en-US"));
    if(animation_rml.find("entity.demo-skeletal-cube.AnimationPlayer.clipAsset.editor")==std::string::npos||
       animation_rml.find("class=\"value-editor\"")==std::string::npos) {
        std::cerr<<"Editable asset fields did not project to retained text controls\n";
        return 20;
    }

    auto preview = nlohmann::json::parse(noemancer::retained_ui_preview_json(semantic_document, 960, 720, 1.25F));
    if (!preview.at("valid").get<bool>() || preview.at("code") != "ok" ||
        preview.at("observation").at("implementation").at("name") != "RmlUi" ||
        preview.at("observation").at("implementation").at("version") != "6.2" ||
        preview.at("observation").at("viewport").at("densityScale") != 1.25F ||
        !preview.at("observation").at("text").at("defaultFontLoaded").get<bool>() ||
        preview.at("observation").at("nodeCount") != nlohmann::json::parse(semantic_document).at("nodes").size() ||
        preview.at("observation").at("layoutDiagnostics").at("actionableOverflowCount") != 0 ||
        preview.dump().find("engine.entity.material.roughness") == std::string::npos ||
        preview.at("renderPacket").at("residentGeometryCount").get<std::size_t>() == 0 ||
        preview.dump().size() >= 32 * 1024) {
        std::cerr << "RmlUi retained preview did not expose layout and renderer-neutral draw evidence\n"
                  << preview.dump(2) << '\n';
        return 3;
    }
    {
        noemancer::RetainedUiRuntime binary_runtime;
        if(!binary_runtime.initialize(960,720,1.0F)||!binary_runtime.load_document("ui.binary",rml)||!binary_runtime.render()) return 6;
        const auto packet=binary_runtime.render_packet();
        const auto has_textured_draw=std::ranges::any_of(packet.draws,[](const auto& draw){return draw.texture_id!=0;});
        if(packet.vertices.empty()||packet.indices.empty()||packet.draws.empty()||packet.textures.empty()||
           !has_textured_draw||packet.indices.size()%3U!=0U) {
            std::cerr << "RmlUi did not expose a renderer-native typed draw packet\n"; return 7;
        }
        const auto default_density_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.binary"));
        if(default_density_observation.at("viewport").at("densityIndependentPixelRatio")!=1.0F)return 76;
        if(binary_runtime.pointer_move(40,70)||!binary_runtime.update()) return 8;
        const auto interaction=nlohmann::json::parse(binary_runtime.observation_json("ui.binary")).at("interaction");
        if(!interaction.at("pointerInteracting").get<bool>()||
           interaction.at("hoveredNodeId").get<std::string>().empty()) {
            std::cerr << "Retained UI input did not resolve to a stable semantic node\n"; return 9;
        }
        const auto action_rml=std::string(
            "<rml><head><style>body{margin:0;width:100%;height:100%;pointer-events:none;font-family:LatoLatin;}"
            "button{position:absolute;left:8px;top:8px;width:180px;height:36px;pointer-events:auto;"
            "background-color:#273449;color:#f2f5fa;border:1px #5a6b84;}"
            "</style></head><body><button id=\"ui.action.cook\" data-semantic-id=\"ui.action.cook\" "
            "data-role=\"button\" data-action=\"asset.cook\" data-binding=\"{&quot;assetId&quot;:&quot;asset.demo&quot;}\">Cook</button></body></rml>");
        if(!binary_runtime.load_document("ui.actions",action_rml)||!binary_runtime.render())return 25;
        static_cast<void>(binary_runtime.pointer_move(24,24));
        static_cast<void>(binary_runtime.pointer_button(0,true));
        static_cast<void>(binary_runtime.pointer_button(0,false));
        const auto actions=binary_runtime.consume_action_events();
        if(actions.size()!=1U||actions.front().sequence!=1U||actions.front().kind!=noemancer::RetainedUiActionKind::invoke||
           actions.front().surface_id!="primary"||actions.front().document_id!="ui.actions"||actions.front().node_id!="ui.action.cook"||
           actions.front().action_id!="asset.cook"||actions.front().binding_json!=R"({"assetId":"asset.demo"})"||actions.front().value_json!="null") {
            std::cerr<<"Retained action did not preserve stable semantic intent\n";return 26;
        }
        const auto consumed_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.actions"));
        if(consumed_observation.at("interaction").at("actions").at("pendingCount")!=0||
           consumed_observation.at("interaction").at("actions").at("lastSequence")!=1)return 27;
        const auto change_rml=std::string(
            "<rml><head><style>body{margin:0;}input{width:220px;height:32px;}</style></head><body>"
            "<input id=\"ui.property.name\" data-semantic-id=\"ui.property.name\" data-role=\"property\" "
            "data-action=\"world.property.plan\" data-binding=\"{&quot;kind&quot;:&quot;world-property&quot;,&quot;entityId&quot;:&quot;entity.demo-cube&quot;,&quot;property&quot;:&quot;engine.entity.sprite.clip&quot;,&quot;revision&quot;:1}\" value=\"idle\"/>"
            "<button id=\"ui.property.commit\" data-semantic-id=\"ui.property.commit\">Commit</button></body></rml>");
        if(!binary_runtime.load_document("ui.change",change_rml)||
           !binary_runtime.focus_node("ui.change","ui.property.name")||
           binary_runtime.text_input("-run")||
           !binary_runtime.focus_node("ui.change","ui.property.commit")||!binary_runtime.update())return 28;
        const auto changes=binary_runtime.consume_action_events();
        const auto changed_value=changes.empty()?std::string{}:nlohmann::json::parse(changes.front().value_json).get<std::string>();
        if(changes.size()!=1U||changes.front().sequence!=2U||changes.front().kind!=noemancer::RetainedUiActionKind::value_changed||
           changes.front().document_id!="ui.change"||changes.front().node_id!="ui.property.name"||
           changes.front().action_id!="world.property.plan"||changed_value.find("idle")==std::string::npos||changed_value.find("run")==std::string::npos||
           nlohmann::json::parse(changes.front().binding_json).at("revision")!=1){
            std::cerr<<"Retained value change did not preserve its revision-bound binding; events="<<changes.size();
            for(const auto& change:changes)std::cerr<<" sequence="<<change.sequence<<" kind="<<static_cast<int>(change.kind)<<" document="<<change.document_id<<
                " node="<<change.node_id<<" action="<<change.action_id<<" binding="<<change.binding_json<<" value="<<change.value_json;
            std::cerr<<'\n';return 29;
        }
        const auto position_id=std::string("editor.inspector.entity.demo-cube.Transform.position");
        if(!binary_runtime.focus_node("ui.binary",position_id+".editor.x")||binary_runtime.text_input("1")||
           !binary_runtime.focus_node("ui.change","ui.property.commit")||!binary_runtime.update())return 34;
        const auto vector_changes=binary_runtime.consume_action_events();
        const auto vector_value=vector_changes.empty()?nlohmann::json{}:nlohmann::json::parse(vector_changes.front().value_json);
        if(vector_changes.size()!=1U||vector_changes.front().sequence!=3U||vector_changes.front().node_id!=position_id||
           vector_changes.front().action_id!="world.property.plan"||!vector_value.is_object()||
           !vector_value.contains("x")||!vector_value.contains("y")||!vector_value.contains("z")) {
            std::cerr<<"Retained vector editor did not emit one typed property value\n";return 35;
        }
        auto binary_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.binary"));
        const auto transform_id=std::string("editor.inspector.entity.demo-cube.section.transform");
        auto transform_node=std::ranges::find_if(binary_observation.at("nodes"),[&](const auto& node){return node.at("id")==transform_id;});
        if(transform_node==binary_observation.at("nodes").end()||!transform_node->at("state").at("expanded").get<bool>())return 36;
        const auto& transform_layout=transform_node->at("layout");
        static_cast<void>(binary_runtime.pointer_move(static_cast<int>(transform_layout.at("x").get<float>()+12.0F),
                                                      static_cast<int>(transform_layout.at("y").get<float>()+12.0F)));
        static_cast<void>(binary_runtime.pointer_button(0,true));
        static_cast<void>(binary_runtime.pointer_button(0,false));
        if(!binary_runtime.update()||!binary_runtime.consume_action_events().empty())return 37;
        binary_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.binary"));
        transform_node=std::ranges::find_if(binary_observation.at("nodes"),[&](const auto& node){return node.at("id")==transform_id;});
        if(transform_node==binary_observation.at("nodes").end()||transform_node->at("state").at("expanded").get<bool>()||
           !binary_runtime.reload_document("ui.binary",rml))return 38;
        binary_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.binary"));
        transform_node=std::ranges::find_if(binary_observation.at("nodes"),[&](const auto& node){return node.at("id")==transform_id;});
        if(transform_node==binary_observation.at("nodes").end()||transform_node->at("state").at("expanded").get<bool>())return 39;
        const auto primary_packet_before=binary_runtime.render_packet();
        if(!binary_runtime.create_surface("editor.inspector",420,640,1.25F)||
           !binary_runtime.load_surface_document("editor.inspector","ui.inspector",action_rml)||
           !binary_runtime.render_surface("editor.inspector"))return 30;
        const auto inspector_packet=binary_runtime.surface_render_packet("editor.inspector");
        const auto primary_packet_after=binary_runtime.render_packet();
        const auto inspector_observation=nlohmann::json::parse(
            binary_runtime.surface_observation_json("editor.inspector","ui.inspector"));
        if(inspector_packet.draws.empty()||inspector_packet.vertices.empty()||
           primary_packet_before.draws.size()!=primary_packet_after.draws.size()||
           inspector_observation.at("surfaceId")!="editor.inspector"||
           inspector_observation.at("viewport").at("width")!=420||
           inspector_observation.at("viewport").at("densityScale")!=1.25F){
            std::cerr<<"Independent retained surface evidence invalid: draws="<<inspector_packet.draws.size()<<
                " vertices="<<inspector_packet.vertices.size()<<" primaryBefore="<<primary_packet_before.draws.size()<<
                " primaryAfter="<<primary_packet_after.draws.size()<<" observation="<<inspector_observation.dump()<<'\n';return 31;
        }
        static_cast<void>(binary_runtime.surface_pointer_move("editor.inspector",24,24));
        static_cast<void>(binary_runtime.surface_pointer_button("editor.inspector",0,true));
        static_cast<void>(binary_runtime.surface_pointer_button("editor.inspector",0,false));
        const auto surface_actions=binary_runtime.consume_action_events();
        if(surface_actions.size()!=1U||surface_actions.front().sequence!=4U||
           surface_actions.front().surface_id!="editor.inspector"||surface_actions.front().action_id!="asset.cook"||
           !binary_runtime.destroy_surface("editor.inspector")||
           !binary_runtime.surface_render_packet("editor.inspector").draws.empty())return 32;
        const auto collection_document=nlohmann::json{
            {"schemaVersion","noemancer.ui-document/0.1"},{"documentId","ui.collection"},
            {"nodes",nlohmann::json::array({
                {{"id","ui.assets"},{"role","tree"},{"label","Assets"},{"state",{{"enabled",true}}}},
                {{"id","asset.alpha"},{"parentId","ui.assets"},{"role","tree-item"},{"label","Alpha"},
                    {"state",{{"enabled",true},{"selected",true}}},
                    {"binding",{{"assetId","asset.alpha"}}},{"actions",nlohmann::json::array({{{"id","asset.open"}}})}},
                {{"id","asset.beta"},{"parentId","ui.assets"},{"role","tree-item"},{"label","Beta"},
                    {"state",{{"enabled",true}}},{"binding",{{"assetId","asset.beta"}}},
                    {"actions",nlohmann::json::array({{{"id","asset.open"}}})}},
                {{"id","asset.gamma"},{"parentId","ui.assets"},{"role","tree-item"},{"label","Gamma"},
                    {"state",{{"enabled",true}}},{"binding",{{"assetId","asset.gamma"}}},
                    {"actions",nlohmann::json::array({{{"id","asset.open"}}})}}
            })}};
        const auto collection_rml=noemancer::retained_ui_rml_from_semantic_document(collection_document.dump());
        if(collection_rml.find("data-role=\"tree-item\"")==std::string::npos||
           collection_rml.find("aria-selected=\"true\"")==std::string::npos||
           !binary_runtime.load_document("ui.collection",collection_rml)||
           !binary_runtime.focus_node("ui.collection","asset.alpha"))return 40;
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::down,true));
        if(!binary_runtime.update())return 41;
        auto collection_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.collection"));
        const auto row_state=[&](const std::string_view id) {
            for(const auto& node:collection_observation.at("nodes"))
                if(node.at("id").get<std::string>()==id)return node.at("state");
            return nlohmann::json{};
        };
        if(!row_state("asset.beta").value("selected",false)||!row_state("asset.beta").value("focused",false)||
           row_state("asset.alpha").value("selected",true)) {
            std::cerr<<"Selectable tree row did not move focus and selection with Down\n";return 42;
        }
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::end,true));
        if(!binary_runtime.update()||!binary_runtime.reload_document("ui.collection",collection_rml))return 43;
        collection_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.collection"));
        if(!row_state("asset.gamma").value("selected",false)) {
            std::cerr<<"Selectable tree row state did not survive document reload\n";return 44;
        }
        if(!binary_runtime.focus_node("ui.collection","asset.gamma"))return 45;
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::home,true));
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::enter,true));
        const auto collection_actions=binary_runtime.consume_action_events();
        if(collection_actions.size()!=1U||collection_actions.front().sequence!=5U||
           collection_actions.front().node_id!="asset.alpha"||collection_actions.front().action_id!="asset.open"||
           nlohmann::json::parse(collection_actions.front().binding_json).at("assetId")!="asset.alpha") {
            std::cerr<<"Selectable tree Enter did not emit stable bounded semantic intent\n";return 46;
        }
        nlohmann::json grid_nodes=nlohmann::json::array({
            {{"id","ui.grid"},{"role","grid"},{"label","Items"},
                {"presentation",{{"gridColumns",3}}},{"state",{{"enabled",true}}}}
        });
        for(std::size_t index=0;index<6U;++index) {
            nlohmann::json item{{"id","item."+std::to_string(index)},{"parentId","ui.grid"},
                {"role",index==5U?"griditem":"grid-item"},{"label","Item "+std::to_string(index)},
                {"state",{{"enabled",true}}},{"binding",{{"itemId","item."+std::to_string(index)}}},
                {"actions",nlohmann::json::array({{{"id","item.open"}}})}};
            if(index==0U) {
                item["presentation"]={{"imageSource","preview://item/0"}};
                item["metadata"]={{"status","Ready"},{"revision",7}};
            }
            grid_nodes.push_back(std::move(item));
        }
        const auto grid_document=nlohmann::json{{"schemaVersion","noemancer.ui-document/0.1"},
            {"documentId","ui.grid"},{"designTokens",{{"gridColumns",2}}},{"nodes",std::move(grid_nodes)}};
        const auto grid_rml=noemancer::retained_ui_rml_from_semantic_document(grid_document.dump());
        const std::array<std::uint8_t,16> preview_pixels{
            255U,0U,0U,255U, 0U,255U,0U,255U, 0U,0U,255U,255U, 255U,255U,255U,255U};
        const auto invalid_image_dimensions=binary_runtime.register_image_rgba8("preview://invalid",0U,2U,preview_pixels);
        const auto invalid_image_bytes=binary_runtime.register_image_rgba8("preview://invalid",2U,2U,
            std::span<const std::uint8_t>(preview_pixels).first(8U));
        const auto exceeded_image_bytes=binary_runtime.register_image_rgba8("preview://too-large",2048U,2048U,
            std::span<const std::uint8_t>(preview_pixels).first(4U));
        const auto registered_preview=binary_runtime.register_image_rgba8("preview://item/0",2U,2U,preview_pixels);
        if(invalid_image_dimensions.success||invalid_image_dimensions.code!="ui.image-dimensions-invalid"||
           invalid_image_bytes.success||invalid_image_bytes.code!="ui.image-bytes-mismatch"||
           exceeded_image_bytes.success||exceeded_image_bytes.code!="ui.image-bytes-exceeded"||
           !registered_preview.success||registered_preview.code!="ui.image-registered"||
           registered_preview.resident_bytes!=preview_pixels.size())return 54;
        if(grid_rml.find("data-grid-columns=\"3\"")==std::string::npos||
           grid_rml.find("style=\"width:33.3333%\"")==std::string::npos||
           grid_rml.find("data-image-source=\"preview://item/0\"")==std::string::npos||
           grid_rml.find("src=\"preview://item/0\"")==std::string::npos||
           grid_rml.find("status: Ready")==std::string::npos||
           !binary_runtime.load_document("ui.grid",grid_rml)||!binary_runtime.render()||
           !binary_runtime.focus_node("ui.grid","item.0"))return 49;
        if(!binary_runtime.create_surface("ui.gallery",420U,320U)||
           !binary_runtime.load_surface_document("ui.gallery","ui.gallery.document",grid_rml)||
           !binary_runtime.render_surface("ui.gallery"))return 55;
        const auto registered_texture=[](const noemancer::RetainedUiRenderPacket& packet,
                                         const std::span<const std::uint8_t> expected,
                                         const std::uint32_t width,const std::uint32_t height) {
            return std::ranges::any_of(packet.textures,[&](const auto& texture) {
                return texture.width==width&&texture.height==height&&
                    std::ranges::equal(texture.rgba8,expected);
            });
        };
        if(!registered_texture(binary_runtime.render_packet(),preview_pixels,2U,2U)||
           !registered_texture(binary_runtime.surface_render_packet("ui.gallery"),preview_pixels,2U,2U)||
           !std::ranges::any_of(binary_runtime.surface_render_packet("ui.gallery").draws,
                [](const auto& draw){return draw.texture_id!=0U;}))return 56;
        const std::array<std::uint8_t,4> replacement_pixels{17U,34U,51U,255U};
        const auto replaced_preview=binary_runtime.register_image_rgba8("preview://item/0",1U,1U,replacement_pixels);
        if(!replaced_preview.success||replaced_preview.code!="ui.image-replaced"||
           replaced_preview.revision<=registered_preview.revision||replaced_preview.resident_bytes!=4U||
           !binary_runtime.render_surface("ui.gallery")||
           !registered_texture(binary_runtime.surface_render_packet("ui.gallery"),replacement_pixels,1U,1U))return 57;
        const auto removed_preview=binary_runtime.remove_image("preview://item/0");
        const auto removed_again=binary_runtime.remove_image("preview://item/0");
        if(!removed_preview.success||removed_preview.code!="ui.image-removed"||removed_preview.resident_bytes!=0U||
           removed_again.success||removed_again.code!="ui.image-not-found"||
           !binary_runtime.render_surface("ui.gallery")||
           registered_texture(binary_runtime.surface_render_packet("ui.gallery"),replacement_pixels,1U,1U)||
           !binary_runtime.destroy_surface("ui.gallery"))return 58;
        noemancer::RetainedUiRuntime image_budget_runtime;
        constexpr std::array<std::uint8_t,4> one_pixel{1U,2U,3U,255U};
        for(std::size_t index=0;index<256U;++index) {
            const auto receipt=image_budget_runtime.register_image_rgba8(
                "budget://"+std::to_string(index),1U,1U,one_pixel);
            if(!receipt.success)return 59;
        }
        const auto image_count_exceeded=image_budget_runtime.register_image_rgba8("budget://overflow",1U,1U,one_pixel);
        if(image_count_exceeded.success||image_count_exceeded.code!="ui.image-count-exceeded")return 60;
        const auto oversized_action_id=std::string(257U,'x');
        nlohmann::json limit_actions=nlohmann::json::array();
        nlohmann::json limit_ids=nlohmann::json::array();
        for(std::size_t index=0;index<9U;++index) {
            const auto action_id="limit."+std::to_string(index);
            limit_ids.push_back(action_id);
            limit_actions.push_back({{"id",action_id},{"label","Limit "+std::to_string(index)},
                {"state",{{"enabled",true}}},{"binding",{{"index",index}}}});
        }
        nlohmann::json excessive_options=nlohmann::json::array();
        for(std::size_t index=0;index<257U;++index)
            excessive_options.push_back({{"value",std::to_string(index)},{"label","Option "+std::to_string(index)}});
        const auto multi_action_document=nlohmann::json{{"schemaVersion","noemancer.ui-document/0.1"},
            {"documentId","ui.multi-action"},{"nodes",nlohmann::json::array({
                {{"id","ui.multi"},{"role","list-item"},{"label","Multiple actions"},
                    {"state",{{"enabled",true}}},
                    {"presentation",{{"inlineActionIds",{"action.alpha","action.beta","action.text","action.combo","action.disabled","action.unknown",oversized_action_id}}}},
                    {"actions",nlohmann::json::array({
                        {{"id","action.main"},{"binding",{{"scope","main"}}}},
                        {{"id","action.alpha"},{"label","Alpha"},{"state",{{"enabled",true}}},{"binding",{{"scope","alpha"},{"revision",11}}}},
                        {{"id","action.beta"},{"label","Beta"},{"state",{{"enabled",true}}},{"binding",{{"scope","beta"},{"revision",22}}}},
                        {{"id","action.text"},{"label","Rename"},{"state",{{"enabled",true}}},{"binding",{{"scope","text"}}},
                            {"input",{{"field","name"},{"control","text"},{"value","draft"},{"placeholder","Name"},{"maxLength",32}}},
                            {"confirmation",{{"field","confirmed"},{"label","Confirm"},{"required",true}}}},
                        {{"id","action.combo"},{"label","Mode"},{"state",{{"enabled",true}}},{"binding",{{"scope","combo"}}},
                            {"input",{{"field","mode"},{"control","combo"},{"value","fast"},
                                {"options",nlohmann::json::array({{{"value","safe"},{"label","Safe"}},{{"value","fast"},{"label","Fast"}}})}}}},
                        {{"id","action.disabled"},{"label","Disabled"},{"state",{{"enabled",false}}},{"binding",{{"scope","disabled"}}}},
                        {{"id",oversized_action_id},{"state",{{"enabled",true}}}}
                    })}},
                {{"id","ui.limit"},{"role","list-item"},{"label","Bounded actions"},
                    {"state",{{"enabled",true}}},{"presentation",{{"inlineActionIds",limit_ids}}},{"actions",limit_actions}},
                {{"id","ui.invalid-form"},{"role","list-item"},{"label","Invalid form"},{"state",{{"enabled",true}}},
                    {"presentation",{{"inlineActionIds",{"action.invalid-form"}}}},
                    {"actions",nlohmann::json::array({{{"id","action.invalid-form"},{"state",{{"enabled",true}}},
                        {"input",{{"field","mode"},{"control","combo"},{"options",excessive_options}}}}})}}
            })}};
        const auto multi_action_rml=noemancer::retained_ui_rml_from_semantic_document(multi_action_document.dump());
        if(multi_action_rml.find("data-action=\"action.main\"")==std::string::npos||
           multi_action_rml.find("data-inline-action-id=\"action.alpha\"")==std::string::npos||
           multi_action_rml.find("data-inline-action-id=\"action.beta\"")==std::string::npos||
           multi_action_rml.find("id=\"ui.multi.inline-action.2.input\"")==std::string::npos||
           multi_action_rml.find("id=\"ui.multi.inline-action.2.confirmation\"")==std::string::npos||
           multi_action_rml.find("<select id=\"ui.multi.inline-action.3.input\"")==std::string::npos||
           multi_action_rml.find("id=\"ui.invalid-form.inline-action.0.input\"")!=std::string::npos||
           multi_action_rml.find("data-inline-action-id=\"action.disabled\"")!=std::string::npos||
           multi_action_rml.find("action.unknown")!=std::string::npos||
           multi_action_rml.find(oversized_action_id)!=std::string::npos||
           multi_action_rml.find("data-inline-action-id=\"limit.7\"")==std::string::npos||
           multi_action_rml.find("data-inline-action-id=\"limit.8\"")!=std::string::npos)return 61;
        constexpr std::string_view form_surface="ui.form-test";
        if(!binary_runtime.create_surface(form_surface,1280U,720U)||
           !binary_runtime.load_surface_document(form_surface,"ui.multi-action",multi_action_rml))return 61;
        if(!binary_runtime.focus_surface_node(form_surface,"ui.multi-action","ui.multi.inline-action.0"))return 62;
        static_cast<void>(binary_runtime.surface_key(form_surface,noemancer::RetainedUiKey::enter,true));
        const auto alpha_actions=binary_runtime.consume_action_events();
        if(alpha_actions.size()!=1U||alpha_actions.front().node_id!="ui.multi"||
           alpha_actions.front().action_id!="action.alpha"||
           alpha_actions.front().value_json!="null"||
           nlohmann::json::parse(alpha_actions.front().binding_json)!=nlohmann::json{{"revision",11},{"scope","alpha"}})return 63;
        if(!binary_runtime.focus_surface_node(form_surface,"ui.multi-action","ui.multi.inline-action.1"))return 64;
        static_cast<void>(binary_runtime.surface_key(form_surface,noemancer::RetainedUiKey::enter,true));
        const auto beta_actions=binary_runtime.consume_action_events();
        if(beta_actions.size()!=1U||beta_actions.front().node_id!="ui.multi"||
           beta_actions.front().action_id!="action.beta"||
           beta_actions.front().value_json!="null"||
           nlohmann::json::parse(beta_actions.front().binding_json)!=nlohmann::json{{"revision",22},{"scope","beta"}})return 65;
        if(!binary_runtime.focus_surface_node(form_surface,"ui.multi-action","ui.multi.inline-action.2"))return 67;
        static_cast<void>(binary_runtime.surface_key(form_surface,noemancer::RetainedUiKey::enter,true));
        if(!binary_runtime.consume_action_events().empty())return 68;
        if(!binary_runtime.focus_surface_node(form_surface,"ui.multi-action","ui.multi.inline-action.2.input"))return 69;
        static_cast<void>(binary_runtime.surface_text_input(form_surface,"-edited"));
        static_cast<void>(binary_runtime.surface_key(form_surface,noemancer::RetainedUiKey::enter,true));
        if(!binary_runtime.update_surface(form_surface)||!binary_runtime.render_surface(form_surface)||
           !binary_runtime.consume_action_events().empty())return 70;
        auto multi_observation=nlohmann::json::parse(binary_runtime.surface_observation_json(form_surface,"ui.multi-action"));
        nlohmann::json confirmation_control;
        for(const auto& node:multi_observation.at("nodes"))if(node.at("id")=="ui.multi")
            for(const auto& control:node.at("inlineControls"))
                if(control.at("id")=="ui.multi.inline-action.2.confirmation")confirmation_control=control;
        if(confirmation_control.empty()||!confirmation_control.at("required").get<bool>())return 71;
        const auto confirmation_layout=confirmation_control.at("layout");
        static_cast<void>(binary_runtime.surface_pointer_move(form_surface,
            static_cast<int>(confirmation_layout.at("x").get<float>()+confirmation_layout.at("width").get<float>()*0.5F),
            static_cast<int>(confirmation_layout.at("y").get<float>()+confirmation_layout.at("height").get<float>()*0.5F)));
        static_cast<void>(binary_runtime.surface_pointer_button(form_surface,0,true));
        static_cast<void>(binary_runtime.surface_pointer_button(form_surface,0,false));
        if(!binary_runtime.consume_action_events().empty()||
           !binary_runtime.focus_surface_node(form_surface,"ui.multi-action","ui.multi.inline-action.2"))return 72;
        static_cast<void>(binary_runtime.surface_key(form_surface,noemancer::RetainedUiKey::enter,true));
        const auto text_form_actions=binary_runtime.consume_action_events();
        const auto text_form_value=text_form_actions.empty()?nlohmann::json{}:
            nlohmann::json::parse(text_form_actions.front().value_json);
        if(text_form_actions.size()!=1U||text_form_actions.front().action_id!="action.text"||
           nlohmann::json::parse(text_form_actions.front().binding_json)!=nlohmann::json{{"scope","text"}}||
           !text_form_value.value("confirmed",false)||
           text_form_value.value("name",std::string{}).find("draft")==std::string::npos||
           text_form_value.value("name",std::string{}).find("edited")==std::string::npos)return 73;
        if(!binary_runtime.focus_surface_node(form_surface,"ui.multi-action","ui.multi.inline-action.3"))return 74;
        static_cast<void>(binary_runtime.surface_key(form_surface,noemancer::RetainedUiKey::enter,true));
        const auto combo_form_actions=binary_runtime.consume_action_events();
        if(combo_form_actions.size()!=1U||combo_form_actions.front().action_id!="action.combo"||
           nlohmann::json::parse(combo_form_actions.front().binding_json)!=nlohmann::json{{"scope","combo"}}||
           nlohmann::json::parse(combo_form_actions.front().value_json)!=nlohmann::json{{"mode","fast"}}||
           !binary_runtime.destroy_surface(form_surface))return 75;
        if(!binary_runtime.focus_node("ui.grid","item.0"))return 66;
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::right,true));
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::down,true));
        auto grid_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.grid"));
        const auto grid_node=[&](const std::string_view id) {
            for(const auto& node:grid_observation.at("nodes"))
                if(node.at("id").get<std::string>()==id)return node;
            return nlohmann::json{};
        };
        if(!grid_node("item.4").at("state").value("selected",false)||
           !grid_node("item.4").at("state").value("focused",false)||
           grid_node("ui.grid").at("collection").at("gridColumns")!=3||
           grid_node("item.0").at("presentation").at("imageSource")!="preview://item/0"||
           grid_node("item.0").at("metadata").at("revision")!=7) {
            std::cerr<<"Grid keyboard navigation or plain-data card projection failed\n";return 50;
        }
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::left,true));
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::up,true));
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::end,true));
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::home,true));
        grid_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.grid"));
        if(!grid_node("item.0").at("state").value("selected",false))return 51;
        const auto item_two_layout=grid_node("item.2").at("layout");
        static_cast<void>(binary_runtime.pointer_move(
            static_cast<int>(item_two_layout.at("x").get<float>()+item_two_layout.at("width").get<float>()*0.5F),
            static_cast<int>(item_two_layout.at("y").get<float>()+item_two_layout.at("height").get<float>()*0.5F)));
        static_cast<void>(binary_runtime.pointer_button(0,true));
        static_cast<void>(binary_runtime.pointer_button(0,false));
        grid_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.grid"));
        const auto clicked_grid_actions=binary_runtime.consume_action_events();
        if(!grid_node("item.2").at("state").value("selected",false)||clicked_grid_actions.size()!=1U||
           clicked_grid_actions.front().node_id!="item.2"||clicked_grid_actions.front().action_id!="item.open") {
            std::cerr<<"Grid pointer selection did not preserve semantic action identity\n";return 52;
        }
        static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::enter,true));
        const auto grid_enter_actions=binary_runtime.consume_action_events();
        if(grid_enter_actions.size()!=1U||grid_enter_actions.front().node_id!="item.2"||
           nlohmann::json::parse(grid_enter_actions.front().binding_json).at("itemId")!="item.2")return 53;
        for(std::size_t index=0;index<132U;++index)
            static_cast<void>(binary_runtime.key(noemancer::RetainedUiKey::enter,true));
        const auto bounded_actions=binary_runtime.consume_action_events();
        const auto bounded_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.collection"));
        if(bounded_actions.size()!=128U||bounded_observation.at("interaction").at("actions").at("droppedCount")<4U) {
            std::cerr<<"Selectable row action queue exceeded its bounded contract\n";return 47;
        }
        auto excessive_document=collection_document;
        excessive_document["nodes"]=nlohmann::json::array();
        for(std::size_t index=0;index<2049U;++index)
            excessive_document["nodes"].push_back({{"id","node."+std::to_string(index)},{"role","list-item"}});
        if(!noemancer::retained_ui_rml_from_semantic_document(excessive_document.dump()).empty()) {
            std::cerr<<"Retained UI claimed an unbounded collection document\n";return 48;
        }
        auto themed=nlohmann::json::parse(semantic_document);
        themed["designTokens"]={{"surfaceColor","#201028ee"},{"groupColor","#30203a"},{"textColor","#fff4ff"},
                                {"accentColor","#ff77dd"},{"surfaceWidthPx",420}};
        const auto themed_rml=noemancer::retained_ui_rml_from_semantic_document(themed.dump());
        if (themed_rml.find("width:420dp")==std::string::npos || themed_rml.find("#ff77dd")==std::string::npos ||
            !binary_runtime.reload_document("ui.binary",themed_rml) || !binary_runtime.render()) return 10;
        if (!nlohmann::json::parse(binary_runtime.observation_json("ui.binary")).at("valid").get<bool>()) return 11;
        const auto arabic_rml=std::string(
            "<rml><head><style>body{font-family:LatoLatin;font-size:28px;color:#fff;}"
            "</style></head><body dir=\"rtl\" lang=\"ar-SA\"><div id=\"ui.arabic\" data-semantic-id=\"ui.arabic\">"
            "Noemancer 42 \xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85</div></body></rml>");
        if(!binary_runtime.load_document("ui.arabic",arabic_rml)||!binary_runtime.render()) return 23;
        const auto arabic_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.arabic"));
        const auto& shaping_stats=arabic_observation.at("text").at("shapingStats");
        if(!arabic_observation.at("text").at("retainedGlyphRunRendering").get<bool>()||
           shaping_stats.at("stringsShaped").get<unsigned long long>()==0ULL||
           shaping_stats.at("visualRuns").get<unsigned long long>()<2ULL||
           shaping_stats.at("glyphsEmitted").get<unsigned long long>()==0ULL||
           shaping_stats.at("fallbackRuns").get<unsigned long long>()==0ULL||
           shaping_stats.at("atlasGlyphsLoaded").get<unsigned long long>()==0ULL) {
            std::cerr<<"Retained Arabic text did not traverse BiDi shaping and the fallback glyph atlas\n";
            return 24;
        }
        const auto text_capabilities=nlohmann::json::parse(noemancer::retained_ui_text_capabilities_json("zh-CN"));
        if(!text_capabilities.at("valid").get<bool>()||text_capabilities.at("requiredScript")!="Han"||
           !text_capabilities.at("textInput").at("committedUtf8").get<bool>()||
           !text_capabilities.at("textInput").at("compositionPreview").get<bool>()||
           !text_capabilities.at("shaping").at("harfBuzz").get<bool>()||
           !text_capabilities.at("segmentation").at("bidirectionalLayout").get<bool>()||
           !text_capabilities.at("segmentation").at("localeLineBreaking").get<bool>()||
           !text_capabilities.at("retainedRenderIntegration").at("glyphRunConsumer").get<bool>()) return 12;
#ifdef _WIN32
        if(!text_capabilities.at("requiredScriptFallbackAvailable").get<bool>()||
           text_capabilities.at("platformFallbackFaces").empty()) return 13;
#endif
        const auto input_rml=std::string(
            "<rml><head><style>body{font-family:LatoLatin;font-size:18px;}input{width:260px;height:32px;}"
            "</style></head><body><input id=\"ui.text.input\" data-semantic-id=\"ui.text.input\" "
            "data-role=\"text-input\" type=\"text\" value=\"\"/></body></rml>");
        if(!binary_runtime.load_document("ui.text",input_rml)||
           !binary_runtime.focus_node("ui.text","ui.text.input")) return 14;
        const auto keyboard=binary_runtime.keyboard_request();
        if(!keyboard.active||keyboard.line_height<=0) return 15;
        constexpr auto chinese_text="\xE4\xBD\xA0\xE5\xA5\xBD";
        if(!binary_runtime.text_composition(chinese_text,2,0)||!binary_runtime.update()) return 16;
        auto text_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.text"));
        if(text_observation.at("nodes").at(0).at("editableValue")!=chinese_text||
           !text_observation.at("interaction").at("textInput").at("active").get<bool>()||
           !text_observation.at("interaction").at("textInput").at("composition").at("active").get<bool>()||
           !text_observation.at("text").at("fallbackGlyphSelection").get<bool>()) return 17;
        if(binary_runtime.text_input(chinese_text)||!binary_runtime.update()) return 21;
        text_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.text"));
        if(text_observation.at("nodes").at(0).at("editableValue")!=chinese_text||
           text_observation.at("interaction").at("textInput").at("composition").at("active").get<bool>()) return 22;
        if(binary_runtime.key(noemancer::RetainedUiKey::backspace,true)||!binary_runtime.update()) return 18;
        text_observation=nlohmann::json::parse(binary_runtime.observation_json("ui.text"));
        if(text_observation.at("nodes").at(0).at("editableValue")!="\xE4\xBD\xA0") return 19;
        const auto density_rml=std::string(
            "<rml><head><style>body{margin:0;font-family:LatoLatin;}div{display:block;position:absolute;left:0;top:0;width:80dp;height:24dp;}</style></head>"
            "<body><div id=\"ui-density-probe\" data-semantic-id=\"ui-density-probe\">DPI</div></body></rml>");
        if(!binary_runtime.set_density_independent_pixel_ratio(1.0F)||
           !binary_runtime.load_document("ui.density-primary",density_rml)||
           !binary_runtime.create_surface("ui.density-surface",320U,200U)||
           binary_runtime.create_surface("ui.invalid-density-surface",320U,200U,3.1F)||
           !binary_runtime.load_surface_document("ui.density-surface","ui.density-secondary",density_rml))return 77;
        const auto density_width=[](const nlohmann::json& observation) {
            for(const auto& node:observation.at("nodes"))if(node.at("id")=="ui-density-probe")
                return node.at("layout").at("width").get<float>();
            return 0.0F;
        };
        const auto primary_density_before=nlohmann::json::parse(binary_runtime.observation_json("ui.density-primary"));
        const auto surface_density_before=nlohmann::json::parse(
            binary_runtime.surface_observation_json("ui.density-surface","ui.density-secondary"));
        const auto primary_width_before=density_width(primary_density_before);
        const auto surface_width_before=density_width(surface_density_before);
        if(primary_density_before.at("viewport").at("densityIndependentPixelRatio")!=1.0F||
           surface_density_before.at("viewport").at("densityIndependentPixelRatio")!=1.0F||
           primary_width_before<=0.0F||surface_width_before<=0.0F||
           !binary_runtime.set_density_independent_pixel_ratio(2.0F)||!binary_runtime.update()||
           !binary_runtime.update_surface("ui.density-surface")) {
            std::cerr<<"DPI setup failed: primary="<<primary_density_before.dump()<<" surface="
                <<surface_density_before.dump()<<" error="<<binary_runtime.last_error()<<'\n';return 78;
        }
        const auto primary_density_after=nlohmann::json::parse(binary_runtime.observation_json("ui.density-primary"));
        const auto surface_density_after=nlohmann::json::parse(
            binary_runtime.surface_observation_json("ui.density-surface","ui.density-secondary"));
        if(primary_density_after.at("viewport").at("densityIndependentPixelRatio")!=2.0F||
           surface_density_after.at("viewport").at("densityIndependentPixelRatio")!=2.0F||
           density_width(primary_density_after)<primary_width_before*1.9F||
           density_width(surface_density_after)<surface_width_before*1.9F)return 79;
        if(binary_runtime.set_density_independent_pixel_ratio(std::numeric_limits<float>::quiet_NaN())||
           binary_runtime.set_density_independent_pixel_ratio(0.5F)||
           binary_runtime.set_density_independent_pixel_ratio(3.1F))return 80;
        const auto density_after_invalid=nlohmann::json::parse(binary_runtime.observation_json("ui.density-primary"));
        if(density_after_invalid.at("viewport").at("densityIndependentPixelRatio")!=2.0F||
           !binary_runtime.set_density_independent_pixel_ratio(1.0F)||
           !binary_runtime.destroy_surface("ui.density-surface"))return 81;
    }

    noemancer::RetainedUiRuntime invalid_density_runtime;
    if(invalid_density_runtime.initialize(640,480,0.5F))return 82;
    noemancer::RetainedUiRuntime first;
    noemancer::RetainedUiRuntime second;
    if (!first.set_density_independent_pixel_ratio(1.5F)||!first.initialize(640, 480)||
        !first.load_document("ui.preinitialized-density",
            "<rml><body><div data-semantic-id=\"preinit-density\">DPI</div></body></rml>")||
        nlohmann::json::parse(first.observation_json("ui.preinitialized-density"))
            .at("viewport").at("densityIndependentPixelRatio")!=1.5F||
        second.initialize(640, 480) ||
        second.last_error().find("process-global state") == std::string::npos) {
        std::cerr << "Process-global RmlUi ownership is not guarded\n";
        return 4;
    }

    return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unhandled retained UI runtime test exception: " << error.what() << '\n';
        return 99;
    }
}
