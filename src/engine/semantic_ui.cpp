#include "engine/semantic_ui.hpp"
#include "engine/project_ui_authoring.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noemancer {
namespace {

using Json = nlohmann::json;

std::string fingerprint(const Json& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value.dump()) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

Json text_descriptor(const std::string& node_id, const std::string& fallback, const std::string_view locale) {
    static const std::unordered_map<std::string,std::string> keys{{"Inspector","ui.inspector"},{"Identity","ui.identity"},
        {"Transform","ui.transform"},{"Velocity","ui.velocity"},{"Rigid Body","ui.rigid-body"},
        {"Material","ui.material"},{"Collider","ui.collider"},{"Animation","ui.animation"},
        {"Player status","ui.player-status"},{"Health","ui.health"},{"Stamina","ui.stamina"},{"Status","ui.status"},
        {"Stable ID","ui.stable-id"},{"Source","ui.source"},{"Position","ui.position"},{"Base Color","ui.base-color"},
        {"Metallic","ui.metallic"},{"Roughness","ui.roughness"},{"Emissive Color","ui.emissive-color"},
        {"Emissive Intensity","ui.emissive-intensity"},{"Half Extents","ui.half-extents"},{"Radius","ui.radius"},
        {"Half Height","ui.half-height"},{"Hull Points","ui.hull-points"},{"Friction","ui.friction"},
        {"Restitution","ui.restitution"},{"Linear Velocity","ui.linear-velocity"},{"Angular Velocity","ui.angular-velocity"},
        {"Motion Type","ui.motion-type"},{"Mass","ui.mass"},{"Gravity Factor","ui.gravity-factor"},
        {"Linear Damping","ui.linear-damping"},{"Angular Damping","ui.angular-damping"},
        {"Continuous Collision","ui.continuous-collision"},{"Allow Sleeping","ui.allow-sleeping"},
        {"Clip","ui.clip"},{"Speed","ui.speed"},{"Looping","ui.looping"},
        {"Playing","ui.playing"},{"Next Clip","ui.next-clip"},{"Cross-fade","ui.cross-fade"},
        {"Root Motion","ui.root-motion"},{"State Machine","ui.state-machine"}};
    const auto found=keys.find(fallback); const auto key=found==keys.end()?node_id+".label":found->second;
    return {{"messageKey", key}, {"fallback", fallback}, {"resolved", fallback},
            {"resolvedLocale", locale}, {"source", "fallback"}};
}

Json load_ui_resource(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary); if(!stream) return Json();
    return Json::parse(stream,nullptr,false);
}

std::string load_ui_text(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary); if(!stream) return {};
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}

std::filesystem::path ui_resource_root() {
#ifdef NOEMANCER_SOURCE_DIR
    return std::filesystem::path(NOEMANCER_SOURCE_DIR)/"assets"/"ui";
#else
    return std::filesystem::path("assets")/"ui";
#endif
}

Json resource_bundle(const std::string_view requested_locale) {
    const auto root=ui_resource_root();
    const auto safe_locale=std::ranges::all_of(requested_locale,[](const char value) {
        return std::isalnum(static_cast<unsigned char>(value))||value=='-'||value=='_';
    })?std::string(requested_locale):std::string("en-US");
    const auto theme=load_ui_resource(root/"themes"/"noemancer-dark.tokens.json");
    const auto fallback=load_ui_resource(root/"locales"/"en-US.json");
    auto requested=load_ui_resource(root/"locales"/(safe_locale+".json"));
    const auto stylesheet=load_ui_text(root/"styles"/"base.rcss");
    if(requested.is_discarded()||!requested.is_object()) requested=Json::object();
    return {{"theme",theme.is_object()?theme:Json::object()},{"fallback",fallback.is_object()?fallback:Json::object()},
        {"requested",std::move(requested)},{"requestedLocale",safe_locale},{"stylesheet",stylesheet}};
}

void apply_resources(Json& document) {
    const auto bundle=resource_bundle(document.value("locale","en-US"));
    const auto theme=bundle.at("theme");
    document["themeId"]=theme.value("themeId","theme.noemancer-dark");
    const auto authored_tokens=document.value("designTokens",Json::object());
    document["designTokens"]=theme.value("tokens",Json::object());
    if(authored_tokens.is_object())document["designTokens"].update(authored_tokens);
    const auto requested=bundle.at("requested").value("messages",Json::object());
    const auto fallback=bundle.at("fallback").value("messages",Json::object());
    std::size_t localized{},fallback_count{},unresolved{}; Json unresolved_keys=Json::array();
    for(auto& node:document["nodes"]) {
        if(!node.contains("text")||!node.at("text").is_object()) continue;
        auto& text=node["text"]; const auto key=text.value("messageKey","");
        if(requested.contains(key)&&requested.at(key).is_string()) {
            text["resolved"]=requested.at(key); text["resolvedLocale"]=bundle.at("requestedLocale"); text["source"]="locale-resource"; ++localized;
        } else if(fallback.contains(key)&&fallback.at(key).is_string()) {
            text["resolved"]=fallback.at(key); text["resolvedLocale"]="en-US"; text["source"]="fallback-resource"; ++fallback_count;
        } else { ++unresolved; if(unresolved_keys.size()<32) unresolved_keys.push_back(key); }
        node["label"]=text.at("resolved"); node["fingerprint"]=fingerprint(node);
    }
    document["resources"]={{"theme",{{"uri","asset://ui/themes/noemancer-dark.tokens.json"},{"loaded",theme.is_object()}}},
        {"messages",{{"requestedUri","asset://ui/locales/"+bundle.at("requestedLocale").get<std::string>()+".json"},
            {"fallbackUri","asset://ui/locales/en-US.json"},{"requestedLoaded",bundle.at("requested").is_object()}}},
        {"stylesheet",{{"uri","asset://ui/styles/base.rcss"},{"loaded",!bundle.at("stylesheet").get<std::string>().empty()},
            {"content",bundle.at("stylesheet")},{"revision",fingerprint(bundle.at("stylesheet"))}}}};
    document["resourceRevision"]=fingerprint(bundle);
    const auto locale=bundle.at("requestedLocale").get<std::string>();
    const auto direction=bundle.at("requested").value("direction",
        locale.starts_with("ar")||locale.starts_with("he")?std::string("rtl"):std::string("ltr"));
    document["textDirection"]=direction;
    const auto required_script=locale.starts_with("zh")?"Han":locale.starts_with("ja")?"Japanese":
        locale.starts_with("ko")?"Hangul":locale.starts_with("ar")?"Arabic":locale.starts_with("he")?"Hebrew":"Latin";
    const auto fallback_ready=required_script==std::string_view("Latin");
    document["localizationDiagnostics"]={{"localizedCount",localized},{"fallbackCount",fallback_count},
        {"unresolvedCount",unresolved},{"unresolvedKeys",std::move(unresolved_keys)},{"fallbackChain",Json::array({bundle.at("requestedLocale"),"en-US","inline"})},
        {"requiredScript",required_script},{"activeFontFamily","LatoLatin"},{"activeFontCoverage","Latin"},
        {"fontFallbackReady",fallback_ready},{"missingGlyphRisk",!fallback_ready},
        {"textDirection",direction},{"bidirectionalLayoutRequired",direction=="rtl"},
        {"rendererNeutralShapingPlanReady",true},{"retainedGlyphRunConsumerReady",true},
        {"fontResolutionScope","runtime-platform-plus-bundled"},{"runtimeCapabilityTool","ui.text.inspect"}};
    document["capabilities"]["localizationDiagnostics"]=true;
}

Json make_node(std::string id, std::string parent_id, std::string role, std::string label,
               const std::string_view locale) {
    Json node{{"id", std::move(id)}, {"parentId", parent_id.empty() ? Json(nullptr) : Json(std::move(parent_id))},
              {"role", std::move(role)}, {"label", label},
              {"state", {{"visible", true}, {"enabled", true}, {"editable", false}}},
              {"actions", Json::array()}};
    node["text"] = text_descriptor(node.at("id").get<std::string>(), label, locale);
    return node;
}

Json validation(const Json& document) {
    Json errors = Json::array();
    if (!document.is_object() || document.value("schemaVersion", "") != "noemancer.ui-document/0.1")
        errors.push_back({{"code", "ui.invalid-schema"}, {"detail", "Expected noemancer.ui-document/0.1."}});
    if (!document.contains("documentId") || !document.at("documentId").is_string() || document.at("documentId").get<std::string>().empty())
        errors.push_back({{"code", "ui.invalid-document-id"}, {"detail", "documentId must be a non-empty string."}});
    if (!document.contains("nodes") || !document.at("nodes").is_array())
        errors.push_back({{"code", "ui.invalid-nodes"}, {"detail", "nodes must be an array."}});

    std::unordered_set<std::string> ids;
    if (document.contains("nodes") && document.at("nodes").is_array()) {
        for (const auto& node : document.at("nodes")) {
            const auto id = node.is_object() ? node.value("id", "") : std::string{};
            if (id.empty()) errors.push_back({{"code", "ui.invalid-node-id"}, {"detail", "Every node needs a non-empty id."}});
            else if (!ids.insert(id).second) errors.push_back({{"code", "ui.duplicate-node-id"}, {"nodeId", id}});
            if (!node.is_object() || node.value("role", "").empty())
                errors.push_back({{"code", "ui.invalid-node-role"}, {"nodeId", id}});
        }
        for (const auto& node : document.at("nodes")) {
            if (!node.is_object() || !node.contains("parentId") || node.at("parentId").is_null()) continue;
            const auto parent = node.at("parentId").is_string() ? node.at("parentId").get<std::string>() : std::string{};
            if (parent.empty() || !ids.contains(parent))
                errors.push_back({{"code", "ui.parent-not-found"}, {"nodeId", node.value("id", "")}, {"parentId", parent}});
        }
    }
    return {{"schemaVersion", "noemancer.ui-validation/0.1"}, {"valid", errors.empty()},
            {"code", errors.empty() ? "ok" : "ui.invalid-document"},
            {"nodeCount", document.contains("nodes") && document.at("nodes").is_array() ? document.at("nodes").size() : 0},
            {"errors", std::move(errors)}};
}

bool contains(const std::vector<std::string>& values, const std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

} // namespace

std::string semantic_ui_document_from_inspector(const std::string_view source_json, const std::string_view locale) {
    const auto source = Json::parse(source_json, nullptr, false);
    if (source.is_discarded() || !source.is_object())
        return Json{{"schemaVersion", "noemancer.ui-document/0.1"}, {"valid", false},
                    {"code", "ui.invalid-source-document"}, {"documentId", "invalid"}, {"nodes", Json::array()}}.dump();

    const auto valid = source.value("valid", false);
    const auto entity = source.value("entity", Json::object());
    const auto panel = source.value("panel", Json::object());
    const auto panel_id = panel.value("id", "editor.panel.inspector");
    const auto entity_id = entity.value("id", "missing");
    Json nodes = Json::array();
    auto panel_node = make_node(panel_id, "", panel.value("role", "inspector"), panel.value("label", "Inspector"), locale);
    panel_node["source"] = {{"documentSchema", source.value("schemaVersion", "unknown")}};
    panel_node["fingerprint"] = fingerprint(panel_node);
    nodes.push_back(std::move(panel_node));

    if (source.contains("sections") && source.at("sections").is_array()) {
        for (const auto& section : source.at("sections")) {
            const auto section_id = section.value("id", "");
            if (section_id.empty()) continue;
            auto section_node = make_node(section_id, panel_id, section.value("role", "group"), section.value("label", ""), locale);
            section_node["component"] = section.value("component", "");
            section_node["state"]["expanded"] = section.value("defaultExpanded", false);
            section_node["fingerprint"] = fingerprint(section_node);
            nodes.push_back(std::move(section_node));

            for (const auto& property : section.value("properties", Json::array())) {
                const auto property_id = property.value("id", "");
                if (property_id.empty()) continue;
                auto node = make_node(property_id, section_id, property.value("role", "property"), property.value("label", ""), locale);
                const auto editable = property.value("editable", false);
                node["state"]["editable"] = editable;
                node["value"] = property.value("value", Json(nullptr));
                node["binding"] = {{"kind", "world-property"}, {"entityId", entity_id},
                                   {"property", property.value("property", "")}, {"component", property.value("component", "")},
                                   {"field", property.value("field", "")}, {"valueType", property.value("valueType", "unknown")},
                                   {"revision",source.value("revision",0ULL)}};
                node["presentation"] = {{"control", property.value("control", "label")},
                                        {"constraints", property.value("constraints", Json::object())}};
                const auto action = property.value("action", "");
                if (editable && !action.empty())
                    node["actions"].push_back({{"id", action}, {"mode", "plan-apply-receipt"}, {"revisionBound", true}, {"undoable", true}});
                if (entity.contains("source")) node["source"] = entity.at("source");
                node["fingerprint"] = fingerprint(node);
                nodes.push_back(std::move(node));
            }
        }
    }

    Json document{{"schemaVersion", "noemancer.ui-document/0.1"}, {"valid", valid},
                  {"code", valid ? "ok" : source.value("code", "ui.source-invalid")},
                  {"documentId", "editor.inspector." + entity_id}, {"surface", "editor"}, {"kind", "inspector"},
                  {"revision", source.value("revision", 0ULL)}, {"locale", locale}, {"roots", Json::array({panel_id})},
                  {"capabilities", {{"semanticQuery", true}, {"transactionActions", true}, {"layoutEvidence", false},
                                      {"localizationDiagnostics", false}, {"accessibilityBridge", false}}},
                  {"nodes", std::move(nodes)}};
    apply_resources(document);
    document["validation"] = validation(document);
    return document.dump();
}

std::string semantic_ui_game_hud_document(const std::string_view state_json, const std::string_view entity_id,
                                          const std::string_view locale) {
    const auto state = Json::parse(state_json, nullptr, false);
    if (state.is_discarded() || !state.is_object())
        return Json{{"schemaVersion","noemancer.ui-document/0.1"},{"valid",false},{"code","ui.invalid-game-state"},
                    {"documentId","game.hud.invalid"},{"nodes",Json::array()}}.dump();
    const auto actors = state.value("actors", Json::array());
    const auto actor = std::ranges::find_if(actors, [&](const Json& candidate) { return candidate.value("entityId", "") == entity_id; });
    if (actor == actors.end())
        return Json{{"schemaVersion","noemancer.ui-document/0.1"},{"valid",false},{"code","ui.actor-not-found"},
                    {"documentId","game.hud."+std::string(entity_id)},{"nodes",Json::array()}}.dump();

    const auto root_id="game.hud."+std::string(entity_id);
    Json nodes=Json::array();
    auto root=make_node(root_id,"","hud","Player status",locale);
    root["fingerprint"]=fingerprint(root); nodes.push_back(std::move(root));
    const auto attributes=actor->value("attributes",Json::object());
    const auto add_meter=[&](const std::string& name,const std::string& label,const float value,const float maximum) {
        auto node=make_node(root_id+"."+name,root_id,"meter",label,locale);
        node["value"]={{"current",value},{"maximum",maximum},{"normalized",maximum>0.0F?value/maximum:0.0F}};
        node["binding"]={{"kind","gameplay-attribute"},{"entityId",entity_id},{"attribute",name},{"valueType","f32"}};
        node["presentation"]={{"control","progress"},{"minimum",0.0F},{"maximum",maximum}};
        node["fingerprint"]=fingerprint(node); nodes.push_back(std::move(node));
    };
    add_meter("health","Health",attributes.value("health",0.0F),attributes.value("maximumHealth",1.0F));
    add_meter("stamina","Stamina",attributes.value("stamina",0.0F),attributes.value("maximumStamina",1.0F));
    auto status=make_node(root_id+".status",root_id,"status","Status",locale);
    status["value"]={{"tags",actor->value("tags",Json::array())},{"activeEffectCount",state.value("activeEffects",Json::array()).size()}};
    status["binding"]={{"kind","gameplay-tags"},{"entityId",entity_id},{"valueType","tag-set"}};
    status["fingerprint"]=fingerprint(status); nodes.push_back(std::move(status));
    for (const auto& ability:actor->value("grantedAbilities",Json::array())) {
        if (!ability.is_string()) continue;
        const auto ability_id=ability.get<std::string>();
        auto slot=make_node(root_id+".ability."+ability_id,root_id,"ability-slot",ability_id,locale);
        slot["value"]={{"abilityId",ability_id},{"cooldownSeconds",actor->value("cooldowns",Json::object()).value(ability_id,0.0F)}};
        slot["actions"].push_back({{"id","gameplay.ability.activate"},{"mode","command-receipt"},{"revisionBound",true},{"undoable",false}});
        slot["fingerprint"]=fingerprint(slot); nodes.push_back(std::move(slot));
    }
    Json document{{"schemaVersion","noemancer.ui-document/0.1"},{"valid",true},{"code","ok"},
        {"documentId",root_id},{"surface","game"},{"kind","hud"},{"revision",state.value("revision",0ULL)},
        {"locale",locale},{"roots",Json::array({root_id})},
        {"designTokens",{{"surfaceColor","#0b1018dc"},{"groupColor","#182334e8"},{"textColor","#e8edf5"},
                         {"accentColor","#62d7ff"},{"dangerColor","#ff5876"},{"surfaceWidthPx",320}}},
        {"capabilities",{{"semanticQuery",true},{"transactionActions",true},{"layoutEvidence",true},
                         {"localizationDiagnostics",false},{"accessibilityBridge",false}}},{"nodes",std::move(nodes)}};
    apply_resources(document);
    document["validation"]=validation(document);
    return document.dump();
}

std::string semantic_ui_project_runtime_document(const std::string_view project_document_json,
                                                  const std::string_view scripting_state_json,
                                                  const std::string_view input_state_json,
                                                  const std::string_view gameplay_state_json,
                                                  const std::string_view locale) {
    auto document=Json::parse(project_ui_resolved_document_json(project_document_json),nullptr,false);
    const auto scripting=Json::parse(scripting_state_json,nullptr,false);
    const auto input=Json::parse(input_state_json,nullptr,false);
    const auto gameplay=Json::parse(gameplay_state_json,nullptr,false);
    if(document.is_discarded()||!document.is_object())return Json{{"schemaVersion","noemancer.ui-document/0.1"},
        {"valid",false},{"code","ui.invalid-project-document"},{"documentId","project.hud.invalid"},{"nodes",Json::array()}}.dump();
    if(!document.value("valid",false)) {
        document["sourceCode"]=document.value("code",std::string{"ui.invalid-document"});
        document["code"]="ui.invalid-project-document";
        return document.dump();
    }
    document["locale"]=std::string(locale);document["surface"]="game";document["kind"]="hud";
    const auto script_instances=scripting.is_object()?scripting.value("instances",Json::array()):Json::array();
    const auto input_actions=input.is_object()?input.value("actions",Json::array()):Json::array();
    const auto gameplay_actors=gameplay.is_object()?gameplay.value("actors",Json::array()):Json::array();
    const auto revision=[](const Json& source){return source.is_object()?source.value("revision",0ULL):0ULL;};
    const auto source_document_revision=document.value("revision",0ULL);
    const auto source_revision=std::max({revision(scripting),revision(input),revision(gameplay)});
    document["sourceDocumentRevision"]=source_document_revision;
    document["revision"]=source_revision;
    if(!document.contains("nodes")||!document["nodes"].is_array())document["nodes"]=Json::array();
    for(auto& node:document["nodes"]) {
        if(!node.is_object()||!node.contains("binding")||!node["binding"].is_object())continue;
        const auto& binding=node["binding"];const auto kind=binding.value("kind",std::string{});bool resolved{};Json value;
        if(kind=="script-state") {
            const auto instance_id=binding.value("instanceId",std::string{});const auto member=binding.value("member",std::string{});
            const auto found=std::ranges::find_if(script_instances,[&](const Json& candidate){return candidate.value("id",std::string{})==instance_id;});
            if(found!=script_instances.end()) {const auto state=found->value("publicState",Json::object());
                if(state.is_object()&&state.contains(member)){value=state.at(member);resolved=true;}}
        } else if(kind=="input-action") {
            const auto action_id=binding.value("actionId",std::string{});const auto field=binding.value("field",std::string{"value"});
            const auto found=std::ranges::find_if(input_actions,[&](const Json& candidate){return candidate.value("id",std::string{})==action_id;});
            if(found!=input_actions.end()&&found->contains(field)){value=found->at(field);resolved=true;}
        } else if(kind=="gameplay-attribute") {
            const auto entity_id=binding.value("entityId",std::string{});const auto attribute=binding.value("attribute",std::string{});
            const auto found=std::ranges::find_if(gameplay_actors,[&](const Json& candidate){return candidate.value("entityId",std::string{})==entity_id;});
            if(found!=gameplay_actors.end()) {const auto attributes=found->value("attributes",Json::object());
                if(attributes.is_object()&&attributes.contains(attribute)){value=attributes.at(attribute);resolved=true;}}
        }
        if(resolved) {node["value"]=std::move(value);node["bindingState"]={{"resolved",true},{"sourceRevision",source_revision}};
            if(node.contains("state")&&node["state"].is_object())node["state"].erase("error");}
        else {if(!node.contains("value")&&binding.contains("fallback"))node["value"]=binding["fallback"];
            node["bindingState"]={{"resolved",false},{"sourceRevision",source_revision}};
            node["state"]["error"]="Binding source is not available.";}
        node["fingerprint"]=fingerprint(node);
    }
    apply_resources(document);document["validation"]=validation(document);
    document["valid"]=document["validation"].value("valid",false);document["code"]=document["valid"].get<bool>()?"ok":"ui.invalid-project-document";
    return document.dump();
}

std::string semantic_ui_query_json(const std::string_view document_json, const SemanticUiQuery& query) {
    const auto document = Json::parse(document_json, nullptr, false);
    if (document.is_discarded() || !document.is_object())
        return Json{{"schemaVersion", "noemancer.ui-observation/0.1"}, {"valid", false},
                    {"code", "ui.invalid-document"}, {"nodes", Json::array()}}.dump();
    const auto checked = validation(document);
    if (!checked.at("valid").get<bool>())
        return Json{{"schemaVersion", "noemancer.ui-observation/0.1"}, {"valid", false},
                    {"code", "ui.invalid-document"}, {"validation", checked}, {"nodes", Json::array()}}.dump();

    auto semantic_content = document;
    semantic_content.erase("revision");
    semantic_content.erase("validation");
    const auto content_fingerprint = fingerprint(semantic_content);

    const auto& source_nodes = document.at("nodes");
    std::unordered_map<std::string, std::size_t> by_id;
    std::unordered_multimap<std::string, std::size_t> by_parent;
    for (std::size_t index = 0; index < source_nodes.size(); ++index) {
        by_id.emplace(source_nodes[index].at("id").get<std::string>(), index);
        if (source_nodes[index].contains("parentId") && source_nodes[index].at("parentId").is_string())
            by_parent.emplace(source_nodes[index].at("parentId").get<std::string>(), index);
    }

    std::vector<bool> selected(source_nodes.size(), false);
    std::vector<std::pair<std::size_t, std::size_t>> frontier;
    for (std::size_t index = 0; index < source_nodes.size(); ++index) {
        const auto& node = source_nodes[index];
        const auto id_match = query.node_ids.empty() || contains(query.node_ids, node.at("id").get<std::string>());
        const auto role_match = query.roles.empty() || contains(query.roles, node.at("role").get<std::string>());
        const auto root_default = query.node_ids.empty() && query.roles.empty() && node.at("parentId").is_null();
        if ((id_match && role_match && (!query.node_ids.empty() || !query.roles.empty())) || root_default) {
            selected[index] = true;
            frontier.emplace_back(index, 0);
        }
    }
    for (std::size_t cursor = 0; cursor < frontier.size(); ++cursor) {
        const auto [index, depth] = frontier[cursor];
        if (depth >= query.depth) continue;
        const auto range = by_parent.equal_range(source_nodes[index].at("id").get<std::string>());
        for (auto child = range.first; child != range.second; ++child) {
            if (!selected[child->second]) { selected[child->second] = true; frontier.emplace_back(child->second, depth + 1); }
        }
    }
    for (std::size_t index = 0; index < source_nodes.size(); ++index) {
        if (!selected[index]) continue;
        auto parent = source_nodes[index].contains("parentId") && source_nodes[index].at("parentId").is_string()
            ? source_nodes[index].at("parentId").get<std::string>() : std::string{};
        while (!parent.empty()) {
            const auto found = by_id.find(parent); if (found == by_id.end() || selected[found->second]) break;
            selected[found->second] = true;
            parent = source_nodes[found->second].contains("parentId") && source_nodes[found->second].at("parentId").is_string()
                ? source_nodes[found->second].at("parentId").get<std::string>() : std::string{};
        }
    }

    std::vector<Json> candidates;
    for (std::size_t index = 0; index < source_nodes.size(); ++index) if (selected[index]) {
        auto node = source_nodes[index];
        if (!query.include_values) node.erase("value");
        candidates.push_back(std::move(node));
    }
    const auto start = std::min(query.cursor, candidates.size());
    Json returned = Json::array();
    std::size_t next = start;
    const auto budget = std::max<std::size_t>(query.byte_budget, 512);
    for (; next < candidates.size(); ++next) {
        returned.push_back(candidates[next]);
        Json probe{{"schemaVersion", "noemancer.ui-observation/0.1"}, {"nodes", returned}};
        if (probe.dump().size() + 512 > budget) { returned.erase(returned.end() - 1); break; }
    }
    const auto truncated = next < candidates.size();
    const auto minimum_required = returned.empty() && truncated ? candidates[next].dump().size() + 512 : 0;
    return Json{{"schemaVersion", "noemancer.ui-observation/0.1"}, {"valid", true},
                {"code", minimum_required > 0 ? "ui.byte-budget-too-small" : "ok"},
                {"document", {{"id", document.at("documentId")}, {"revision", document.value("revision", 0ULL)},
                               {"sourceDocumentRevision",document.value("sourceDocumentRevision",document.value("revision",0ULL))},
                               {"surface", document.value("surface", "unknown")}, {"kind", document.value("kind", "unknown")},
                               {"locale", document.value("locale", "und")},{"themeId",document.value("themeId","unknown")},
                               {"contentFingerprint", content_fingerprint},
                               {"resourceRevision",document.value("resourceRevision","unknown")},
                               {"localizationDiagnostics",document.value("localizationDiagnostics",Json::object())}}},
                {"query", {{"depth", query.depth}, {"byteBudget", budget}, {"cursor", start},
                            {"includeValues", query.include_values}, {"nodeIds", query.node_ids}, {"roles", query.roles}}},
                {"totalNodes", candidates.size()}, {"returnedNodes", returned.size()}, {"truncated", truncated},
                {"nextCursor", truncated && !returned.empty() ? Json(start + returned.size()) : Json(nullptr)},
                {"minimumRequiredBytes", minimum_required}, {"nodes", std::move(returned)}}.dump();
}

std::string semantic_ui_delta_json(const std::string_view document_json, const std::string_view world_delta_json,
                                   const std::string_view entity_id, const SemanticUiDeltaQuery& query) {
    const auto document = Json::parse(document_json, nullptr, false);
    const auto world_delta = Json::parse(world_delta_json, nullptr, false);
    if (document.is_discarded() || !document.is_object() || world_delta.is_discarded() || !world_delta.is_object())
        return Json{{"schemaVersion", "noemancer.ui-delta/0.1"}, {"valid", false},
                    {"code", "ui.invalid-delta-source"}, {"document", Json::object()},
                    {"resyncRequired", true}, {"changes", Json::array()}}.dump();

    const auto checked = validation(document);
    if (!checked.at("valid").get<bool>())
        return Json{{"schemaVersion", "noemancer.ui-delta/0.1"}, {"valid", false},
                    {"code", "ui.invalid-document"}, {"document", Json::object()}, {"validation", checked},
                    {"resyncRequired", true}, {"changes", Json::array()}}.dump();
    if (!document.value("valid", false))
        return Json{{"schemaVersion", "noemancer.ui-delta/0.1"}, {"valid", false},
                    {"code", document.value("code", "ui.invalid-document")}, {"document", Json::object()},
                    {"resyncRequired", true}, {"changes", Json::array()}}.dump();

    auto content = document;
    content.erase("revision");
    content.erase("validation");
    const auto content_fingerprint = fingerprint(content);
    const auto from_revision = world_delta.value("fromRevision", 0ULL);
    const auto to_revision = world_delta.value("toRevision", document.value("revision", 0ULL));
    const auto resync_required = world_delta.value("resyncRequired", true);
    const auto document_summary = Json{{"id", document.value("documentId", "unknown")},
                                       {"entityId", entity_id},
                                       {"fromRevision", from_revision},
                                       {"toRevision", to_revision},
                                       {"contentFingerprint", content_fingerprint},
                                       {"surface", document.value("surface", "unknown")},
                                       {"kind", document.value("kind", "unknown")},
                                       {"locale", document.value("locale", "und")}};
    const auto resync_hint = Json{{"command", "ui.observe"},
                                  {"arguments", {{"entityId", entity_id}, {"depth", 2},
                                                  {"byteBudget", query.byte_budget},
                                                  {"includeValues", query.include_values},
                                                  {"locale", document.value("locale", "en-US")}}}};
    if (resync_required)
        return Json{{"schemaVersion", "noemancer.ui-delta/0.1"}, {"valid", true},
                    {"code", "ui.resync-required"}, {"document", document_summary},
                    {"resyncRequired", true}, {"resync", resync_hint}, {"totalChanges", 0},
                    {"returnedChanges", 0}, {"truncated", false}, {"nextCursor", nullptr},
                    {"changes", Json::array()}}.dump();

    std::unordered_map<std::string, Json> nodes_by_property;
    for (const auto& node : document.at("nodes")) {
        if (!node.is_object() || !node.contains("binding") || !node.at("binding").is_object()) continue;
        const auto& binding = node.at("binding");
        if (binding.value("kind", "") != "world-property" || binding.value("entityId", "") != entity_id) continue;
        const auto property = binding.value("property", "");
        if (!property.empty()) nodes_by_property.emplace(property, node);
    }

    std::vector<Json> coalesced;
    std::unordered_map<std::string, std::size_t> by_node;
    std::size_t source_change_count{};
    std::size_t ignored_change_count{};
    for (const auto& source : world_delta.value("changes", Json::array())) {
        if (!source.is_object() || source.value("entityId", "") != entity_id) continue;
        ++source_change_count;
        const auto node_found = nodes_by_property.find(source.value("field", ""));
        if (node_found == nodes_by_property.end()) { ++ignored_change_count; continue; }
        const auto& node = node_found->second;
        const auto node_id = node.at("id").get<std::string>();
        const auto existing = by_node.find(node_id);
        if (existing == by_node.end()) {
            Json change{{"operation", "update"}, {"nodeId", node_id},
                        {"role", node.value("role", "unknown")}, {"label", node.value("label", "")},
                        {"binding", node.at("binding")},
                        {"revisionBefore", source.value("revisionBefore", from_revision)},
                        {"revisionAfter", source.value("revisionAfter", to_revision)},
                        {"currentFingerprint", node.value("fingerprint", "unknown")},
                        {"managers", Json::array({source.value("manager", "unknown")})}};
            if (query.include_values) {
                change["before"] = source.value("before", Json(nullptr));
                change["after"] = source.value("after", node.value("value", Json(nullptr)));
                change["currentValue"] = node.value("value", Json(nullptr));
            }
            by_node.emplace(node_id, coalesced.size());
            coalesced.push_back(std::move(change));
        } else {
            auto& change = coalesced[existing->second];
            change["revisionAfter"] = source.value("revisionAfter", to_revision);
            change["currentFingerprint"] = node.value("fingerprint", "unknown");
            const auto manager = source.value("manager", "unknown");
            const auto& managers = change.at("managers");
            if (std::ranges::none_of(managers, [&](const Json& value) { return value == manager; }))
                change["managers"].push_back(manager);
            if (query.include_values) {
                change["after"] = source.value("after", node.value("value", Json(nullptr)));
                change["currentValue"] = node.value("value", Json(nullptr));
            }
        }
    }

    if (!query.base_fingerprint.empty() && query.base_fingerprint != content_fingerprint && coalesced.empty())
        return Json{{"schemaVersion", "noemancer.ui-delta/0.1"}, {"valid", true},
                    {"code", "ui.resync-required"}, {"document", document_summary},
                    {"resyncRequired", true}, {"resyncReason", "content-fingerprint-mismatch"},
                    {"resync", resync_hint}, {"totalChanges", 0}, {"returnedChanges", 0},
                    {"truncated", false}, {"nextCursor", nullptr}, {"changes", Json::array()}}.dump();

    const auto start = std::min(query.cursor, coalesced.size());
    const auto budget = std::max<std::size_t>(query.byte_budget, 512);
    Json returned = Json::array();
    std::size_t next = start;
    const auto make_result = [&](const Json& changes) {
        return Json{{"schemaVersion", "noemancer.ui-delta/0.1"}, {"valid", true}, {"code", "ok"},
                    {"document", document_summary}, {"resyncRequired", false},
                    {"query", {{"byteBudget", budget}, {"cursor", start},
                               {"includeValues", query.include_values},
                               {"baseFingerprint", query.base_fingerprint.empty() ? Json(nullptr) : Json(query.base_fingerprint)}}},
                    {"compression", {{"sourceChanges", source_change_count},
                                     {"coalescedNodeChanges", coalesced.size()},
                                     {"ignoredNonUiChanges", ignored_change_count}}},
                    {"totalChanges", coalesced.size()}, {"returnedChanges", changes.size()},
                    {"changes", changes}};
    };
    for (; next < coalesced.size(); ++next) {
        returned.push_back(coalesced[next]);
        if (make_result(returned).dump().size() + 128 > budget) {
            returned.erase(returned.end() - 1);
            break;
        }
    }
    const auto truncated = next < coalesced.size();
    auto result = make_result(returned);
    const auto minimum_required = returned.empty() && truncated ? make_result(Json::array({coalesced[next]})).dump().size() : 0;
    result["code"] = minimum_required > budget ? "ui.byte-budget-too-small" : "ok";
    result["truncated"] = truncated;
    result["nextCursor"] = truncated && !returned.empty() ? Json(start + returned.size()) : Json(nullptr);
    result["minimumRequiredBytes"] = minimum_required;
    if (minimum_required > budget) result["resync"] = resync_hint;
    return result.dump();
}

std::string semantic_ui_validation_json(const std::string_view document_json) {
    const auto document = Json::parse(document_json, nullptr, false);
    if (document.is_discarded())
        return Json{{"schemaVersion", "noemancer.ui-validation/0.1"}, {"valid", false},
                    {"code", "ui.invalid-json"}, {"nodeCount", 0},
                    {"errors", Json::array({{{"code", "ui.invalid-json"}, {"detail", "Document is not valid JSON."}}})}}.dump();
    return validation(document).dump();
}

std::string semantic_ui_resource_status_json(const std::string_view locale) {
    const auto bundle=resource_bundle(locale); const auto theme=bundle.at("theme"); const auto requested=bundle.at("requested");
    const auto fallback=bundle.at("fallback");
    const auto stylesheet_loaded=!bundle.at("stylesheet").get<std::string>().empty();
    const auto valid_theme=theme.is_object()&&theme.value("schemaVersion","")=="noemancer.ui-design-tokens/0.1";
    const auto valid_requested=requested.is_object()&&requested.value("schemaVersion","")=="noemancer.ui-messages/0.1";
    const auto valid_fallback=fallback.is_object()&&fallback.value("schemaVersion","")=="noemancer.ui-messages/0.1";
    const auto valid=valid_theme&&valid_fallback&&stylesheet_loaded;
    return Json{{"schemaVersion","noemancer.ui-resource-status/0.1"},{"valid",valid},
        {"code",valid?"ok":"ui.resource-missing"},{"locale",bundle.at("requestedLocale")},
        {"themeLoaded",valid_theme},{"stylesheetLoaded",stylesheet_loaded},{"requestedMessagesLoaded",valid_requested},{"fallbackMessagesLoaded",valid_fallback},
        {"resourceRevision",fingerprint(bundle)},{"reloadModel","poll-fingerprint-and-reload-document"}}.dump();
}

} // namespace noemancer
