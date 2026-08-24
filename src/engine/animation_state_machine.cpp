#include "animation_state_machine.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace noemancer {
namespace {

using Json=nlohmann::json;

AnimationStateMachineParseResult failure(std::string code,std::string detail) {
    return {std::nullopt,std::move(code),std::move(detail)};
}

bool finite(const float value) { return std::isfinite(value); }

bool condition_matches(const AnimationTransitionCondition& condition,
    const std::unordered_map<std::string,float>& parameters,const std::string_view cue,const float elapsed) {
    if(condition.source=="cue")return !condition.cue.empty()&&cue==condition.cue;
    float value{};
    if(condition.source=="elapsed")value=elapsed;
    else {
        const auto found=parameters.find(condition.parameter);if(found==parameters.end())return false;value=found->second;
    }
    if(condition.comparison=="greater")return value>condition.threshold;
    if(condition.comparison=="greater-or-equal")return value>=condition.threshold;
    if(condition.comparison=="less")return value<condition.threshold;
    if(condition.comparison=="less-or-equal")return value<=condition.threshold;
    if(condition.comparison=="equal")return std::abs(value-condition.threshold)<=0.0001F;
    if(condition.comparison=="not-equal")return std::abs(value-condition.threshold)>0.0001F;
    return false;
}

Json document_json(const AnimationStateMachineDocument& document) {
    Json parameters=Json::array();for(const auto& parameter:document.parameters)
        parameters.push_back({{"id",parameter.id},{"type",parameter.type},{"default",parameter.default_value}});
    Json states=Json::array();for(const auto& state:document.states) {
        Json value{{"id",state.id},{"looping",state.looping}};if(!state.clip_asset.empty())value["clipAsset"]=state.clip_asset;
        states.push_back(std::move(value));
    }
    Json transitions=Json::array();for(const auto& transition:document.transitions) {
        Json conditions=Json::array();for(const auto& source:transition.conditions) {
            Json condition{{"source",source.source}};
            if(source.source=="cue")condition["cue"]=source.cue;
            else {
                if(source.source=="parameter")condition["parameter"]=source.parameter;
                condition["comparison"]=source.comparison;condition["threshold"]=source.threshold;
            }
            conditions.push_back(std::move(condition));
        }
        transitions.push_back({{"id",transition.id},{"from",transition.from},{"to",transition.to},
            {"conditions",std::move(conditions)},{"durationSeconds",transition.duration_seconds},{"priority",transition.priority}});
    }
    return {{"schemaVersion","noemancer.animation-state-machine/0.2"},{"assetId",document.asset_id},
        {"initialState",document.initial_state},{"parameters",std::move(parameters)},
        {"states",std::move(states)},{"transitions",std::move(transitions)}};
}

AnimationStateMachineDocument built_in_machine() {
    AnimationStateMachineDocument document;
    document.asset_id="animation.machine.basic-locomotion";document.initial_state="idle";
    document.parameters={{"speed","float",0.0F},{"grounded","bool",1.0F}};
    document.states={{"idle","",true},{"locomotion","",true},{"airborne","",true},{"hit-react","",false}};
    document.transitions={
        {"cue.hit-react","*","hit-react",{{"cue","","equal",0.0F,"hit-react"}},0.08F,100},
        {"airborne.enter","*","airborne",{{"parameter","grounded","less",0.5F,""}},0.12F,80},
        {"airborne.exit-moving","airborne","locomotion",{{"parameter","grounded","greater-or-equal",0.5F,""},{"parameter","speed","greater",0.1F,""}},0.12F,70},
        {"airborne.exit-idle","airborne","idle",{{"parameter","grounded","greater-or-equal",0.5F,""},{"parameter","speed","less-or-equal",0.1F,""}},0.12F,60},
        {"locomotion.enter","idle","locomotion",{{"parameter","speed","greater",0.1F,""}},0.15F,50},
        {"locomotion.exit","locomotion","idle",{{"parameter","speed","less-or-equal",0.1F,""}},0.15F,50},
        {"hit-react.exit","hit-react","idle",{{"elapsed","","greater-or-equal",0.2F,""}},0.1F,10}};
    return document;
}

} // namespace

AnimationStateMachineParseResult AnimationStateMachineCodec::parse_json(const std::string_view source) {
    try {
    const auto root=Json::parse(source,nullptr,false);
    if(root.is_discarded()||!root.is_object())return failure("animation.machine.invalid-json","Animation State Machine must be a JSON object.");
    if(root.value("schemaVersion",std::string{})!="noemancer.animation-state-machine/0.2")
        return failure("animation.machine.unsupported-schema","Expected noemancer.animation-state-machine/0.2.");
    AnimationStateMachineDocument document;
    document.asset_id=root.value("assetId",std::string{});document.initial_state=root.value("initialState",std::string{});
    if(document.asset_id.empty()||document.initial_state.empty())
        return failure("animation.machine.identity-missing","assetId and initialState are required.");
    const auto parameter_values=root.value("parameters",Json::array());const auto state_values=root.value("states",Json::array());
    const auto transition_values=root.value("transitions",Json::array());
    if(!parameter_values.is_array()||!state_values.is_array()||!transition_values.is_array()||state_values.empty())
        return failure("animation.machine.structure-invalid","parameters, non-empty states, and transitions arrays are required.");
    std::unordered_set<std::string> parameter_ids,state_ids,transition_ids;
    for(const auto& value:parameter_values) {
        if(!value.is_object())return failure("animation.machine.parameter-invalid","Every parameter must be an object.");
        AnimationStateParameterDefinition parameter{value.value("id",std::string{}),value.value("type",std::string{"float"}),
            value.value("default",0.0F)};
        if(parameter.id.empty()||(parameter.type!="float"&&parameter.type!="bool")||!finite(parameter.default_value)||
           !parameter_ids.insert(parameter.id).second)
            return failure("animation.machine.parameter-invalid","Parameter IDs must be unique and values must be finite float/bool definitions.");
        if(parameter.type=="bool")parameter.default_value=parameter.default_value>=0.5F?1.0F:0.0F;
        document.parameters.push_back(std::move(parameter));
    }
    for(const auto& value:state_values) {
        if(!value.is_object())return failure("animation.machine.state-invalid","Every state must be an object.");
        AnimationStateDefinition state{value.value("id",std::string{}),value.value("clipAsset",std::string{}),value.value("looping",true)};
        if(state.id.empty()||!state_ids.insert(state.id).second)
            return failure("animation.machine.state-invalid","State IDs must be non-empty and unique.");
        document.states.push_back(std::move(state));
    }
    if(!state_ids.contains(document.initial_state))return failure("animation.machine.initial-state-invalid","initialState must reference a declared state.");
    const std::unordered_set<std::string> comparisons{"greater","greater-or-equal","less","less-or-equal","equal","not-equal"};
    for(const auto& value:transition_values) {
        if(!value.is_object()||!value.contains("conditions")||!value.at("conditions").is_array()||value.at("conditions").empty())
            return failure("animation.machine.transition-invalid","Every transition requires a non-empty conditions array.");
        AnimationTransitionDefinition transition;
        transition.id=value.value("id",std::string{});transition.from=value.value("from",std::string{});
        transition.to=value.value("to",std::string{});transition.duration_seconds=value.value("durationSeconds",0.15F);
        transition.priority=value.value("priority",0);
        if(transition.id.empty()||!transition_ids.insert(transition.id).second||
           (transition.from!="*"&&!state_ids.contains(transition.from))||!state_ids.contains(transition.to)||
           !finite(transition.duration_seconds)||transition.duration_seconds<0.0F||transition.duration_seconds>10.0F)
            return failure("animation.machine.transition-invalid","Transition identity, endpoints, duration, and priority are invalid.");
        for(const auto& condition_value:value.at("conditions")) {
            if(!condition_value.is_object())return failure("animation.machine.condition-invalid","Every condition must be an object.");
            AnimationTransitionCondition condition;
            condition.source=condition_value.value("source",std::string{"parameter"});
            condition.parameter=condition_value.value("parameter",std::string{});
            condition.comparison=condition_value.value("comparison",std::string{"greater"});
            condition.threshold=condition_value.value("threshold",0.0F);condition.cue=condition_value.value("cue",std::string{});
            if(condition.source=="parameter") {
                if(!parameter_ids.contains(condition.parameter)||!comparisons.contains(condition.comparison)||!finite(condition.threshold))
                    return failure("animation.machine.condition-invalid","Parameter condition is invalid.");
            } else if(condition.source=="elapsed") {
                if(!comparisons.contains(condition.comparison)||!finite(condition.threshold)||condition.threshold<0.0F)
                    return failure("animation.machine.condition-invalid","Elapsed condition is invalid.");
            } else if(condition.source=="cue") {
                if(condition.cue.empty())return failure("animation.machine.condition-invalid","Cue condition requires a cue ID.");
            } else return failure("animation.machine.condition-invalid","Condition source must be parameter, elapsed, or cue.");
            transition.conditions.push_back(std::move(condition));
        }
        document.transitions.push_back(std::move(transition));
    }
    std::ranges::stable_sort(document.transitions,[](const auto& left,const auto& right) {
        if(left.priority!=right.priority)return left.priority>right.priority;return left.id<right.id;
    });
    return {std::move(document),"ok","Animation State Machine parsed and normalized."};
    } catch(const Json::exception&) {
        return failure("animation.machine.type-invalid",
            "Animation State Machine fields must use the types required by noemancer.animation-state-machine/0.2.");
    }
}

std::string AnimationStateMachineCodec::write_canonical_json(const AnimationStateMachineDocument& document) {
    return document_json(document).dump(2);
}

std::vector<std::string> AnimationStateMachineCodec::asset_dependencies(const AnimationStateMachineDocument& document) {
    std::vector<std::string> result;
    for(const auto& state:document.states)if(!state.clip_asset.empty())result.push_back(state.clip_asset);
    std::ranges::sort(result);result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}

AnimationStateMachineLibrary::AnimationStateMachineLibrary() { static_cast<void>(register_document(built_in_machine())); }

bool AnimationStateMachineLibrary::register_document(AnimationStateMachineDocument document) {
    if(document.asset_id.empty()||document.initial_state.empty())return false;
    documents_.insert_or_assign(document.asset_id,std::move(document));return true;
}

const AnimationStateMachineDocument* AnimationStateMachineLibrary::find(const std::string_view asset_id) const {
    const auto found=documents_.find(std::string(asset_id));return found==documents_.end()?nullptr:&found->second;
}

AnimationStateMachineEvaluation AnimationStateMachineLibrary::evaluate(const std::string_view asset_id,
    const std::string_view active_state,const std::unordered_map<std::string,float>& parameters,
    const std::string_view gameplay_cue,const float state_elapsed_seconds) const {
    AnimationStateMachineEvaluation result;const auto* document=find(asset_id);
    if(!document){result.code="animation.machine-not-found";return result;}
    const auto current=active_state.empty()?document->initial_state:std::string(active_state);
    const auto current_state=std::ranges::find(document->states,current,&AnimationStateDefinition::id);
    if(current_state==document->states.end()){result.code="animation.state-not-found";return result;}
    result.valid=true;result.code="ok";result.from=current;result.to=current;
    result.clip_asset=current_state->clip_asset;result.looping=current_state->looping;
    for(const auto& transition:document->transitions) {
        if(transition.to==current||(transition.from!="*"&&transition.from!=current))continue;
        if(!std::ranges::all_of(transition.conditions,[&](const auto& condition) {
            return condition_matches(condition,parameters,gameplay_cue,state_elapsed_seconds);}))continue;
        const auto target=std::ranges::find(document->states,transition.to,&AnimationStateDefinition::id);
        if(target==document->states.end())continue;
        result.transitioned=true;result.to=transition.to;result.transition_id=transition.id;
        result.clip_asset=target->clip_asset;result.looping=target->looping;result.duration_seconds=transition.duration_seconds;break;
    }
    return result;
}

std::string AnimationStateMachineLibrary::inspect_json(const std::string_view asset_id) const {
    const auto* document=find(asset_id);
    if(!document)return Json{{"schemaVersion","noemancer.animation-state-machine-inspection/0.1"},{"valid",false},
        {"code","animation.machine-not-found"},{"assetId",asset_id},{"definition",nullptr}}.dump();
    return Json{{"schemaVersion","noemancer.animation-state-machine-inspection/0.1"},{"valid",true},{"code","ok"},
        {"assetId",document->asset_id},{"definition",document_json(*document)}}.dump();
}

} // namespace noemancer
