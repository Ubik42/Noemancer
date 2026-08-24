#include "engine/play_world_apply.hpp"

#include "engine/scene_document.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace noemancer {
namespace {
using Json = nlohmann::json;

Json parse_canonical(const std::string_view source) {
    const auto parsed=SceneDocumentCodec::parse_json(source,"play://apply-back");
    if(!parsed)return Json();
    return Json::parse(SceneDocumentCodec::write_canonical_json(*parsed.document),nullptr,false);
}

std::map<std::string,Json> entities_by_id(const Json& document) {
    std::map<std::string,Json> result;
    if(!document.is_object()||!document.contains("entities")||!document["entities"].is_array())return result;
    for(const auto& entity:document["entities"])
        if(entity.is_object()&&entity.contains("guid")&&entity["guid"].is_string())
            result.emplace(entity["guid"].get<std::string>(),entity);
    return result;
}

std::uint64_t fnv1a64(const std::string_view value) {
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto character:value){hash^=static_cast<unsigned char>(character);hash*=1099511628211ULL;}
    return hash;
}

std::string stable_change_id(const std::string_view operation,const std::string_view entity_id,const std::string_view field) {
    std::ostringstream output;output<<"play-change-"<<std::hex<<std::setfill('0')<<std::setw(16)
        <<fnv1a64(std::string(operation)+"\n"+std::string(entity_id)+"\n"+std::string(field));return output.str();
}

PlayWorldChange make_change(std::string entity_id,std::string field,Json before,Json after,std::string operation) {
    PlayWorldChange result{.entity_id=std::move(entity_id),.field=std::move(field),
        .before_json=before.dump(),.after_json=after.dump(),.operation=std::move(operation)};
    result.change_id=stable_change_id(result.operation,result.entity_id,result.field);return result;
}

Json encoded_change(const PlayWorldChange& change) {
    return {{"changeId",change.change_id},{"operation",change.operation},{"entityId",change.entity_id},{"field",change.field},
        {"before",Json::parse(change.before_json,nullptr,false)},{"after",Json::parse(change.after_json,nullptr,false)}};
}

Json* find_entity(Json& document,const std::string_view id) {
    if(!document.contains("entities")||!document["entities"].is_array())return nullptr;
    for(auto& entity:document["entities"])
        if(entity.is_object()&&entity.value("guid",std::string{})==id)return &entity;
    return nullptr;
}

bool apply_selected_change(Json& candidate,const PlayWorldChange& change,std::string& code,std::string& detail) {
    const auto after=Json::parse(change.after_json,nullptr,false);
    if(after.is_discarded()){code="play.apply.selection-invalid-payload";detail="A selected change has invalid canonical JSON.";return false;}
    if(change.operation=="add-entity") {
        if(find_entity(candidate,change.entity_id)!=nullptr||!after.is_object()) {
            code="play.apply.selection-add-conflict";detail="A selected entity addition conflicts with the baseline.";return false;
        }
        candidate["entities"].push_back(after);return true;
    }
    if(change.operation=="remove-entity") {
        auto& entities=candidate["entities"];
        const auto found=std::find_if(entities.begin(),entities.end(),[&](const Json& entity){return entity.value("guid",std::string{})==change.entity_id;});
        if(found==entities.end()){code="play.apply.selection-remove-missing";detail="A selected entity removal no longer exists in the baseline.";return false;}
        entities.erase(found);
        return true;
    }
    auto* entity=find_entity(candidate,change.entity_id);
    if(entity==nullptr){code="play.apply.selection-entity-missing";detail="A selected field change targets an entity outside the baseline.";return false;}
    if(change.operation=="set-name"){(*entity)["name"]=after;return true;}
    if(change.operation=="set-parent") {
        if(after.is_null()||(after.is_string()&&after.get<std::string>().empty()))entity->erase("parentGuid");
        else (*entity)["parentGuid"]=after;
        return true;
    }
    constexpr std::string_view prefix="engine.scene.component.";
    if((change.operation=="add-component"||change.operation=="remove-component"||change.operation=="replace-component")&&
       change.field.starts_with(prefix)) {
        const auto component=change.field.substr(prefix.size());
        auto& components=(*entity)["components"];
        if(change.operation=="remove-component"||after.is_null())components.erase(std::string(component));
        else components[std::string(component)]=after;
        return true;
    }
    code="play.apply.selection-operation-unsupported";detail="A selected change uses an unsupported authoring operation.";return false;
}
} // namespace

std::string PlayWorldApplyPlan::to_json() const {
    Json encoded=Json::array();for(const auto& change:changes)encoded.push_back(encoded_change(change));
    return Json{{"schemaVersion","noemancer.play-world-apply-plan/0.2"},{"valid",valid},{"code",code},{"detail",detail},
        {"baseRevision",base_revision},{"changeCount",changes.size()},{"changes",std::move(encoded)}}.dump();
}

std::string PlayWorldSelectionPlan::to_json() const {
    Json encoded=Json::array();for(const auto& change:changes)encoded.push_back(encoded_change(change));
    return Json{{"schemaVersion","noemancer.play-world-selection-plan/0.1"},{"valid",valid},{"code",code},{"detail",detail},
        {"baseRevision",base_revision},{"selectedChangeIds",selected_change_ids},{"changeCount",changes.size()},
        {"changes",std::move(encoded)}}.dump();
}

PlayWorldApplyPlan plan_play_world_apply(const std::string_view base_scene_json,
    const std::string_view runtime_scene_json,const std::uint64_t base_revision) {
    PlayWorldApplyPlan plan{.base_revision=base_revision,.base_scene_json=std::string(base_scene_json)};
    const auto before=parse_canonical(base_scene_json);const auto after=parse_canonical(runtime_scene_json);
    if(before.is_discarded()||before.is_null()){plan.code="play.apply.invalid-base";plan.detail="The edit-world baseline is not a valid scene document.";return plan;}
    if(after.is_discarded()||after.is_null()){plan.code="play.apply.invalid-runtime";plan.detail="The play-world authoring snapshot is not a valid scene document.";return plan;}
    if(before.value("sceneGuid",std::string{})!=after.value("sceneGuid",std::string{})) {
        plan.code="play.apply.scene-mismatch";plan.detail="The play world does not belong to the edit-world scene.";return plan;
    }
    const auto before_entities=entities_by_id(before);const auto after_entities=entities_by_id(after);
    for(const auto& [id,entity]:before_entities) {
        if(!after_entities.contains(id)){plan.changes.push_back(make_change(id,"engine.scene.entity.removed",entity,nullptr,"remove-entity"));continue;}
        const auto& next=after_entities.at(id);
        if(entity.value("name",std::string{})!=next.value("name",std::string{}))
            plan.changes.push_back(make_change(id,"engine.entity.name",entity.value("name",std::string{}),next.value("name",std::string{}),"set-name"));
        if(entity.value("parentGuid",std::string{})!=next.value("parentGuid",std::string{}))
            plan.changes.push_back(make_change(id,"engine.entity.parent",entity.value("parentGuid",std::string{}),next.value("parentGuid",std::string{}),"set-parent"));
        const auto old_components=entity.value("components",Json::object());const auto new_components=next.value("components",Json::object());
        std::set<std::string> names;for(const auto& [name,value]:old_components.items()){static_cast<void>(value);names.insert(name);}
        for(const auto& [name,value]:new_components.items()){static_cast<void>(value);names.insert(name);}
        for(const auto& name:names) {
            const auto old_value=old_components.value(name,Json(nullptr));const auto new_value=new_components.value(name,Json(nullptr));
            if(old_value==new_value)continue;
            const auto operation=old_value.is_null()?"add-component":new_value.is_null()?"remove-component":"replace-component";
            plan.changes.push_back(make_change(id,"engine.scene.component."+name,old_value,new_value,operation));
        }
    }
    for(const auto& [id,entity]:after_entities)if(!before_entities.contains(id))
        plan.changes.push_back(make_change(id,"engine.scene.entity.added",nullptr,entity,"add-entity"));
    std::ranges::sort(plan.changes,[](const PlayWorldChange& left,const PlayWorldChange& right){
        return std::tie(left.entity_id,left.field,left.operation)<std::tie(right.entity_id,right.field,right.operation);});
    const auto candidate=SceneDocumentCodec::parse_json(after.dump(),"play://apply-back");
    if(!candidate){plan.code="play.apply.invalid-candidate";plan.detail="The generated Apply Back candidate is not a valid scene document.";return plan;}
    plan.candidate_scene_json=SceneDocumentCodec::write_canonical_json(*candidate.document);plan.valid=true;
    plan.code=plan.changes.empty()?"play.apply.no-change":"ok";plan.detail=plan.changes.empty()?"The play world has no authorable changes.":
        "Play-world changes are valid and ready for selective atomic apply.";return plan;
}

PlayWorldSelectionPlan plan_play_world_apply_selection(const PlayWorldApplyPlan& complete,
    const std::vector<std::string>& requested_ids) {
    PlayWorldSelectionPlan result{.base_revision=complete.base_revision,.base_scene_json=complete.base_scene_json};
    if(!complete.valid){result.code="play.apply.selection-source-invalid";result.detail="The complete Play World diff is invalid.";return result;}
    std::set<std::string> requested;
    for(const auto& id:requested_ids)if(id.empty()||!requested.insert(id).second) {
        result.code="play.apply.selection-id-invalid";result.detail="Selected change IDs must be non-empty and unique.";return result;
    }
    std::unordered_map<std::string,const PlayWorldChange*> by_id;
    for(const auto& change:complete.changes)if(change.change_id.empty()||!by_id.emplace(change.change_id,&change).second) {
        result.code="play.apply.selection-source-duplicate";result.detail="The complete diff contains an invalid change identity.";return result;
    }
    for(const auto& id:requested)if(!by_id.contains(id)) {
        result.code="play.apply.selection-unknown-change";result.detail="A selected change ID is not present in the current Play World diff.";return result;
    }
    std::map<std::string,std::size_t> entity_counts;std::set<std::string> whole_entity_operations;
    for(const auto& change:complete.changes)if(requested.contains(change.change_id)) {
        ++entity_counts[change.entity_id];if(change.operation=="add-entity"||change.operation=="remove-entity")whole_entity_operations.insert(change.entity_id);
    }
    for(const auto& id:whole_entity_operations)if(entity_counts[id]>1U) {
        result.code="play.apply.selection-entity-conflict";result.detail="An entity add/remove cannot be combined with field changes for the same entity.";return result;
    }
    auto candidate=parse_canonical(complete.base_scene_json);
    if(candidate.is_discarded()||candidate.is_null()){result.code="play.apply.selection-base-invalid";result.detail="The selection baseline is invalid.";return result;}
    for(const auto& change:complete.changes)if(requested.contains(change.change_id)) {
        std::string code,detail;if(!apply_selected_change(candidate,change,code,detail)){result.code=std::move(code);result.detail=std::move(detail);return result;}
        result.changes.push_back(change);result.selected_change_ids.push_back(change.change_id);
    }
    const auto parsed=SceneDocumentCodec::parse_json(candidate.dump(),"play://apply-back-selection");
    if(!parsed){result.code="play.apply.selection-candidate-invalid";result.detail="The selected changes do not form a valid scene document.";return result;}
    result.candidate_scene_json=SceneDocumentCodec::write_canonical_json(*parsed.document);result.valid=true;
    result.code=result.changes.empty()?"play.apply.selection-empty":"ok";result.detail=result.changes.empty()?"No Play World changes are selected.":
        "Selected Play World changes are valid and ready for one atomic transaction.";return result;
}

} // namespace noemancer
