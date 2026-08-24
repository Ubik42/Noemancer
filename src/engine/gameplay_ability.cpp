#include "engine/gameplay_ability.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace noemancer {
namespace {

using Json = nlohmann::json;

bool has_tag(const GameplayAbilityActor& actor, const std::string_view tag) {
    return std::ranges::find(actor.tags, tag) != actor.tags.end();
}

bool add_tag(GameplayAbilityActor& actor, const std::string_view tag) {
    if (has_tag(actor, tag)) return false;
    actor.tags.emplace_back(tag);
    std::ranges::sort(actor.tags);
    return true;
}

bool remove_tag(GameplayAbilityActor& actor, const std::string_view tag) {
    const auto found = std::ranges::find(actor.tags, tag);
    if (found == actor.tags.end()) return false;
    actor.tags.erase(found);
    return true;
}

struct ModifierExecution final { Json changes=Json::array(); bool changed{}; };

ModifierExecution execute_modifiers(GameplayAbilityActor& target, const GameplayEffectDefinition& definition,
                                    const std::uint32_t stack_count) {
    ModifierExecution result;
    for (const auto& modifier : definition.modifiers) {
        float* attribute = nullptr;
        float maximum = 0.0F;
        if (modifier.attribute == "health") { attribute=&target.health; maximum=target.maximum_health; }
        else if (modifier.attribute == "stamina") { attribute=&target.stamina; maximum=target.maximum_stamina; }
        if (!attribute || modifier.operation != "add") continue;
        const auto before=*attribute;
        const auto requested=modifier.magnitude*static_cast<float>(stack_count);
        float resistance{};
        if (modifier.attribute=="health" && requested<0.0F && definition.damage_type!="none") {
            if (const auto found=target.resistances.find(definition.damage_type); found!=target.resistances.end())
                resistance=std::clamp(found->second,0.0F,0.95F);
        }
        const auto applied=requested*(1.0F-resistance);
        *attribute=std::clamp(before+applied,0.0F,maximum);
        result.changes.push_back({{"attribute",modifier.attribute},{"operation",modifier.operation},
            {"requestedMagnitude",requested},{"appliedMagnitude",applied},{"stackCount",stack_count},
            {"damageType",definition.damage_type},{"resistance",resistance},{"before",before},{"after",*attribute}});
        result.changed=result.changed||before!=*attribute;
    }
    if (target.health<=0.0F) {
        result.changed=remove_tag(target,"state.alive")||result.changed;
        result.changed=add_tag(target,"state.dead")||result.changed;
    } else if (has_tag(target,"state.dead")) {
        result.changed=remove_tag(target,"state.dead")||result.changed;
        result.changed=add_tag(target,"state.alive")||result.changed;
    }
    return result;
}

} // namespace

GameplayAbilityRuntime::GameplayAbilityRuntime()
    : definitions_{
          {"ability.combat.impact", "Impact", 15.0F, 0.35F, {"state.alive"},
           {"state.stunned", "state.dead"}, "combat.hit",
           {"effect.damage.impact", "effect.state.hit-react"}},
          {"ability.movement.dash", "Dash", 10.0F, 0.50F, {"state.alive"},
           {"state.rooted", "state.dead"}, "movement.dash", {"effect.state.dashing"}},
          {"ability.combat.ignite", "Ignite", 20.0F, 1.0F, {"state.alive"},
           {"state.stunned", "state.dead"}, "combat.ignite", {"effect.damage.fire-dot"}}},
      effect_definitions_{
          {"effect.damage.impact", "Impact Damage", 0.0F,
           {{"health", "add", -25.0F}}, {}, "combat.damage.applied", "",0.0F,1,true,"physical"},
          {"effect.recovery.minor", "Minor Recovery", 0.0F,
           {{"health", "add", 20.0F}}, {}, "combat.healing.applied", "recover",0.0F,1,true,"none"},
          {"effect.state.hit-react", "Hit React", 0.35F, {}, {"state.hit-react"},
           "combat.hit-react.started", "hit-react",0.0F,1,true,"none"},
          {"effect.state.dashing", "Dashing", 0.20F, {}, {"state.dashing"},
           "movement.dash.started", "dash",0.0F,1,true,"none"},
          {"effect.damage.fire-dot", "Stacking Fire", 3.0F, {{"health","add",-4.0F}}, {"state.burning"},
           "combat.fire.applied", "burn",0.5F,3,false,"fire"}} {}

GameplayAbilityActor& GameplayAbilityRuntime::ensure_actor(const std::string_view entity_id) {
    const auto found = std::ranges::find(actors_, entity_id, &GameplayAbilityActor::entity_id);
    if (found != actors_.end()) return *found;
    actors_.push_back({.entity_id = std::string(entity_id)});
    return actors_.back();
}

bool GameplayAbilityRuntime::grant(const std::string_view entity_id, const std::string_view ability_id) {
    if (entity_id.empty() ||
        std::ranges::find(definitions_, ability_id, &GameplayAbilityDefinition::id) == definitions_.end()) return false;
    auto& actor = ensure_actor(entity_id);
    if (std::ranges::find(actor.granted_abilities, ability_id) != actor.granted_abilities.end()) return true;
    actor.granted_abilities.emplace_back(ability_id);
    std::ranges::sort(actor.granted_abilities);
    ++revision_;
    return true;
}

std::string GameplayAbilityRuntime::apply_effect(const std::string_view source_entity_id,
                                                 const std::string_view target_entity_id,
                                                 const GameplayEffectDefinition& definition,
                                                 GameplayRuntime& events) {
    const auto revision_before = revision_;
    const bool actor_existed = std::ranges::find(actors_, target_entity_id, &GameplayAbilityActor::entity_id) != actors_.end();
    auto& target = ensure_actor(target_entity_id);
    std::uint64_t instance_id{};
    bool changed = !actor_existed;
    ActiveGameplayEffect* active{};
    std::uint32_t stack_count=1;
    if (definition.duration_seconds > 0.0F) {
        for (const auto& tag : definition.granted_tags) changed = add_tag(target, tag) || changed;
        const auto existing=std::ranges::find_if(active_effects_,[&](const auto& value) {
            return value.effect_id==definition.id&&value.source_entity_id==source_entity_id&&value.target_entity_id==target_entity_id;
        });
        if (existing!=active_effects_.end()) {
            active=&*existing; instance_id=active->instance_id;
            active->stack_count=std::min(definition.maximum_stacks,active->stack_count+1U);
            ++active->applications; active->remaining_seconds=definition.duration_seconds;
        } else {
            instance_id=next_effect_instance_id_++;
            active_effects_.push_back({instance_id,definition.id,std::string(source_entity_id),std::string(target_entity_id),
                definition.duration_seconds,definition.granted_tags,definition.period_seconds,0.0F,1,1,0});
            active=&active_effects_.back();
        }
        stack_count=active->stack_count;
        changed = true;
    } else instance_id=next_effect_instance_id_++;
    auto execution=definition.duration_seconds<=0.0F||definition.execute_on_application
        ? execute_modifiers(target,definition,stack_count) : ModifierExecution{};
    changed=execution.changed||changed;
    if (changed) ++revision_;
    const auto event_sequence = events.emit(definition.event_type, std::string(source_entity_id),
        std::string(target_entity_id), Json{{"effectInstanceId", instance_id}, {"effectId", definition.id},
        {"durationSeconds", definition.duration_seconds}, {"periodSeconds",definition.period_seconds},
        {"stackCount",stack_count},{"damageType",definition.damage_type},{"animationCue", definition.animation_cue},
        {"changes", execution.changes}}.dump());
    return Json{{"schemaVersion", "noemancer.gameplay-effect-receipt/0.2"}, {"success", true}, {"code", "ok"},
        {"effectInstanceId", instance_id}, {"effectId", definition.id}, {"sourceEntityId", source_entity_id},
        {"targetEntityId", target_entity_id}, {"durationSeconds", definition.duration_seconds},
        {"periodSeconds",definition.period_seconds},{"stackCount",stack_count},{"maximumStacks",definition.maximum_stacks},
        {"damageType",definition.damage_type},{"executeOnApplication",definition.execute_on_application},
        {"grantedTags", definition.granted_tags}, {"changes", std::move(execution.changes)},
        {"eventType", definition.event_type}, {"eventSequence", event_sequence},
        {"animationCue", definition.animation_cue}, {"revisionBefore", revision_before}, {"revisionAfter", revision_}}.dump();
}

std::string GameplayAbilityRuntime::apply_effect_json(const std::string_view source_entity_id,
                                                       const std::string_view target_entity_id,
                                                       const std::string_view effect_id,
                                                       GameplayRuntime& events) {
    const auto definition = std::ranges::find(effect_definitions_, effect_id, &GameplayEffectDefinition::id);
    if (source_entity_id.empty() || target_entity_id.empty() || definition == effect_definitions_.end()) {
        return Json{{"schemaVersion", "noemancer.gameplay-effect-receipt/0.2"}, {"success", false},
            {"code", definition == effect_definitions_.end() ? "gameplay.effect.not-found" : "gameplay.effect.invalid-target"},
            {"effectId", effect_id}, {"sourceEntityId", source_entity_id}, {"targetEntityId", target_entity_id},
            {"revisionBefore", revision_}, {"revisionAfter", revision_}}.dump();
    }
    return apply_effect(source_entity_id, target_entity_id, *definition, events);
}

std::string GameplayAbilityRuntime::activate_json(const std::string_view entity_id,
                                                   const std::string_view ability_id,
                                                   const std::string_view target_id,
                                                   GameplayRuntime& events) {
    const auto revision_before = revision_;
    const auto activation_id = next_activation_id_++;
    auto actor = std::ranges::find(actors_, entity_id, &GameplayAbilityActor::entity_id);
    const auto definition = std::ranges::find(definitions_, ability_id, &GameplayAbilityDefinition::id);
    std::string code = "ok";
    if (actor == actors_.end()) code = "gameplay.ability.actor-not-found";
    else if (definition == definitions_.end()) code = "gameplay.ability.not-found";
    else if (std::ranges::find(actor->granted_abilities, ability_id) == actor->granted_abilities.end())
        code = "gameplay.ability.not-granted";
    else if (const auto cooldown = actor->cooldowns.find(std::string(ability_id));
             cooldown != actor->cooldowns.end() && cooldown->second > 0.0F)
        code = "gameplay.ability.on-cooldown";
    else if (std::ranges::any_of(definition->required_tags, [&](const auto& tag) { return !has_tag(*actor, tag); }))
        code = "gameplay.ability.missing-required-tag";
    else if (std::ranges::any_of(definition->blocked_tags, [&](const auto& tag) { return has_tag(*actor, tag); }))
        code = "gameplay.ability.blocked-by-tag";
    else if (actor->stamina < definition->stamina_cost) code = "gameplay.ability.insufficient-stamina";
    if (code != "ok") {
        return Json{{"schemaVersion", "noemancer.ability-activation/0.2"}, {"success", false}, {"code", code},
            {"activationId", activation_id}, {"entityId", entity_id}, {"abilityId", ability_id},
            {"targetId", target_id}, {"revisionBefore", revision_before}, {"revisionAfter", revision_}}.dump();
    }

    const auto stamina_before = actor->stamina;
    actor->stamina -= definition->stamina_cost;
    const auto stamina_after = actor->stamina;
    actor->cooldowns[definition->id] = definition->cooldown_seconds;
    ++revision_;
    const auto resolved_target = target_id.empty() ? entity_id : target_id;
    const auto event_sequence = events.emit(definition->event_type, std::string(entity_id),
        std::string(resolved_target), Json{{"activationId", activation_id}, {"abilityId", definition->id},
        {"staminaBefore", stamina_before}, {"staminaAfter", stamina_after}}.dump());
    Json effects = Json::array();
    for (const auto& effect_id : definition->effects) {
        const auto effect = std::ranges::find(effect_definitions_, effect_id, &GameplayEffectDefinition::id);
        if (effect != effect_definitions_.end())
            effects.push_back(Json::parse(apply_effect(entity_id, resolved_target, *effect, events)));
    }
    return Json{{"schemaVersion", "noemancer.ability-activation/0.2"}, {"success", true}, {"code", "ok"},
        {"activationId", activation_id}, {"entityId", entity_id}, {"abilityId", ability_id},
        {"targetId", resolved_target}, {"eventType", definition->event_type}, {"eventSequence", event_sequence},
        {"staminaBefore", stamina_before}, {"staminaAfter", stamina_after},
        {"cooldownSeconds", definition->cooldown_seconds}, {"effects", std::move(effects)},
        {"revisionBefore", revision_before}, {"revisionAfter", revision_}}.dump();
}

void GameplayAbilityRuntime::tick(const float delta_seconds, GameplayRuntime& events) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) return;
    bool changed = false;
    for (auto& actor : actors_) {
        const auto stamina = actor.stamina;
        actor.stamina = std::min(actor.maximum_stamina, actor.stamina + 10.0F * delta_seconds);
        changed = changed || actor.stamina != stamina;
        for (auto& [unused, cooldown] : actor.cooldowns) {
            static_cast<void>(unused);
            const auto before = cooldown;
            cooldown = std::max(0.0F, cooldown - delta_seconds);
            changed = changed || cooldown != before;
        }
    }
    for (auto& effect : active_effects_) {
        const auto active_delta=std::min(delta_seconds,effect.remaining_seconds);
        effect.remaining_seconds=std::max(0.0F,effect.remaining_seconds-delta_seconds);
        if (effect.period_seconds<=0.0F) continue;
        effect.period_accumulator+=active_delta;
        const auto definition=std::ranges::find(effect_definitions_,effect.effect_id,&GameplayEffectDefinition::id);
        auto actor=std::ranges::find(actors_,effect.target_entity_id,&GameplayAbilityActor::entity_id);
        if (definition==effect_definitions_.end()||actor==actors_.end()) continue;
        std::uint32_t tick_count{};
        while (effect.period_accumulator+0.000001F>=effect.period_seconds && tick_count<64U) {
            effect.period_accumulator-=effect.period_seconds;
            auto execution=execute_modifiers(*actor,*definition,effect.stack_count);
            changed=execution.changed||changed;
            ++tick_count; ++effect.ticks_executed;
            events.emit("gameplay.effect.periodic",effect.source_entity_id,effect.target_entity_id,
                Json{{"effectInstanceId",effect.instance_id},{"effectId",effect.effect_id},{"tick",effect.ticks_executed},
                     {"stackCount",effect.stack_count},{"damageType",definition->damage_type},{"changes",execution.changes}}.dump());
        }
    }
    for (const auto& effect : active_effects_) {
        if (effect.remaining_seconds > 0.0F) continue;
        auto actor = std::ranges::find(actors_, effect.target_entity_id, &GameplayAbilityActor::entity_id);
        if (actor == actors_.end()) continue;
        for (const auto& tag : effect.granted_tags) {
            const bool retained = std::ranges::any_of(active_effects_, [&](const auto& other) {
                return other.instance_id != effect.instance_id && other.target_entity_id == effect.target_entity_id &&
                       other.remaining_seconds > 0.0F && std::ranges::find(other.granted_tags, tag) != other.granted_tags.end();
            });
            if (!retained) changed = remove_tag(*actor, tag) || changed;
        }
        events.emit("gameplay.effect.expired", effect.source_entity_id, effect.target_entity_id,
                    Json{{"effectInstanceId", effect.instance_id}, {"effectId", effect.effect_id}}.dump());
        changed = true;
    }
    std::erase_if(active_effects_, [](const auto& effect) { return effect.remaining_seconds <= 0.0F; });
    if (changed) ++revision_;
}

std::string GameplayAbilityRuntime::catalog_json() const {
    Json definitions = Json::array();
    for (const auto& value : definitions_) {
        definitions.push_back({{"id", value.id}, {"displayName", value.display_name},
            {"staminaCost", value.stamina_cost}, {"cooldownSeconds", value.cooldown_seconds},
            {"requiredTags", value.required_tags}, {"blockedTags", value.blocked_tags},
            {"eventType", value.event_type}, {"effects", value.effects}});
    }
    return Json{{"schemaVersion", "noemancer.ability-catalog/0.2"}, {"definitions", std::move(definitions)}}.dump();
}

std::string GameplayAbilityRuntime::effect_catalog_json() const {
    Json definitions = Json::array();
    for (const auto& value : effect_definitions_) {
        Json modifiers = Json::array();
        for (const auto& modifier : value.modifiers)
            modifiers.push_back({{"attribute", modifier.attribute}, {"operation", modifier.operation},
                                 {"magnitude", modifier.magnitude}});
        definitions.push_back({{"id", value.id}, {"displayName", value.display_name},
            {"durationSeconds", value.duration_seconds}, {"modifiers", std::move(modifiers)},
            {"grantedTags", value.granted_tags}, {"eventType", value.event_type},
            {"animationCue", value.animation_cue},{"periodSeconds",value.period_seconds},
            {"maximumStacks",value.maximum_stacks},{"executeOnApplication",value.execute_on_application},
            {"damageType",value.damage_type}});
    }
    return Json{{"schemaVersion", "noemancer.gameplay-effect-catalog/0.2"},
                {"definitions", std::move(definitions)}}.dump();
}

std::string GameplayAbilityRuntime::observe_json(const std::string_view entity_id) const {
    Json actors = Json::array();
    for (const auto& actor : actors_) {
        if (!entity_id.empty() && actor.entity_id != entity_id) continue;
        Json cooldowns = Json::object();
        for (const auto& [id, value] : actor.cooldowns) cooldowns[id] = value;
        actors.push_back({{"entityId", actor.entity_id},
            {"attributes", {{"health", actor.health}, {"maximumHealth", actor.maximum_health},
                            {"stamina", actor.stamina}, {"maximumStamina", actor.maximum_stamina}}},
            {"resistances",actor.resistances},
            {"tags", actor.tags}, {"grantedAbilities", actor.granted_abilities},
            {"cooldowns", std::move(cooldowns)}});
    }
    Json active_effects = Json::array();
    for (const auto& effect : active_effects_) {
        if (!entity_id.empty() && effect.target_entity_id != entity_id && effect.source_entity_id != entity_id) continue;
        active_effects.push_back({{"instanceId", effect.instance_id}, {"effectId", effect.effect_id},
            {"sourceEntityId", effect.source_entity_id}, {"targetEntityId", effect.target_entity_id},
            {"remainingSeconds", effect.remaining_seconds}, {"grantedTags", effect.granted_tags},
            {"periodSeconds",effect.period_seconds},{"secondsUntilNextTick",std::max(0.0F,effect.period_seconds-effect.period_accumulator)},
            {"stackCount",effect.stack_count},{"applications",effect.applications},{"ticksExecuted",effect.ticks_executed}});
    }
    return Json{{"schemaVersion", "noemancer.ability-state/0.3"}, {"revision", revision_},
                {"actors", std::move(actors)}, {"activeEffects", std::move(active_effects)}}.dump();
}

std::vector<std::string> GameplayAbilityRuntime::tags(const std::string_view entity_id) const {
    const auto actor=std::ranges::find(actors_,entity_id,&GameplayAbilityActor::entity_id);
    return actor==actors_.end()?std::vector<std::string>{}:actor->tags;
}

void GameplayAbilityRuntime::forget_entity(const std::string_view entity_id) {
    const auto previous_actor_count=actors_.size();
    const auto previous_effect_count=active_effects_.size();
    std::erase_if(actors_,[&](const GameplayAbilityActor& actor){return actor.entity_id==entity_id;});
    std::erase_if(active_effects_,[&](const ActiveGameplayEffect& effect){
        return effect.source_entity_id==entity_id||effect.target_entity_id==entity_id;
    });
    if(actors_.size()!=previous_actor_count||active_effects_.size()!=previous_effect_count)++revision_;
}

std::string GameplayAbilityRuntime::set_tag_json(const std::string_view entity_id,const std::string_view tag,
                                                 const bool present,GameplayRuntime& events) {
    const auto before=revision_;
    auto& actor=ensure_actor(entity_id);
    const auto changed=present?add_tag(actor,tag):remove_tag(actor,tag);
    if(changed) {
        ++revision_;
        events.emit(present?"gameplay.tag.added":"gameplay.tag.removed",std::string(entity_id),std::string(entity_id),
            Json{{"tag",tag}}.dump());
    }
    return Json{{"schemaVersion","noemancer.gameplay-tag-receipt/0.1"},{"success",true},{"code",changed?"ok":"gameplay.tag.no-change"},
        {"entityId",entity_id},{"tag",tag},{"present",present},{"changed",changed},{"revisionBefore",before},{"revisionAfter",revision_}}.dump();
}

} // namespace noemancer
