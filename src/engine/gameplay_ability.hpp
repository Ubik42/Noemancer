#pragma once

#include "engine/gameplay_runtime.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noemancer {

struct GameplayAbilityDefinition final {
    std::string id;
    std::string display_name;
    float stamina_cost{};
    float cooldown_seconds{};
    std::vector<std::string> required_tags;
    std::vector<std::string> blocked_tags;
    std::string event_type;
    std::vector<std::string> effects;
};

struct GameplayEffectModifier final {
    std::string attribute;
    std::string operation;
    float magnitude{};
};

struct GameplayEffectDefinition final {
    std::string id;
    std::string display_name;
    float duration_seconds{};
    std::vector<GameplayEffectModifier> modifiers;
    std::vector<std::string> granted_tags;
    std::string event_type;
    std::string animation_cue;
    float period_seconds{};
    std::uint32_t maximum_stacks{1};
    bool execute_on_application{true};
    std::string damage_type{"none"};
};

struct ActiveGameplayEffect final {
    std::uint64_t instance_id{};
    std::string effect_id;
    std::string source_entity_id;
    std::string target_entity_id;
    float remaining_seconds{};
    std::vector<std::string> granted_tags;
    float period_seconds{};
    float period_accumulator{};
    std::uint32_t stack_count{1};
    std::uint32_t applications{1};
    std::uint32_t ticks_executed{};
};

struct GameplayAbilityActor final {
    std::string entity_id;
    float health{100.0F};
    float maximum_health{100.0F};
    float stamina{100.0F};
    float maximum_stamina{100.0F};
    std::vector<std::string> tags{"state.alive"};
    std::vector<std::string> granted_abilities;
    std::unordered_map<std::string,float> cooldowns;
    std::unordered_map<std::string,float> resistances{{"physical",0.20F},{"fire",0.25F},{"arcane",0.10F}};
};

class GameplayAbilityRuntime final {
public:
    GameplayAbilityRuntime();
    [[nodiscard]] bool grant(std::string_view entity_id,std::string_view ability_id);
    [[nodiscard]] std::string activate_json(std::string_view entity_id,std::string_view ability_id,
                                            std::string_view target_id,GameplayRuntime& events);
    [[nodiscard]] std::string apply_effect_json(std::string_view source_entity_id,std::string_view target_entity_id,
                                                std::string_view effect_id,GameplayRuntime& events);
    void tick(float delta_seconds,GameplayRuntime& events);
    [[nodiscard]] std::string catalog_json() const;
    [[nodiscard]] std::string effect_catalog_json() const;
    [[nodiscard]] std::string observe_json(std::string_view entity_id = {}) const;
    [[nodiscard]] std::vector<std::string> tags(std::string_view entity_id) const;
    [[nodiscard]] std::string set_tag_json(std::string_view entity_id,std::string_view tag,bool present,
                                           GameplayRuntime& events);
    void forget_entity(std::string_view entity_id);
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    [[nodiscard]] GameplayAbilityActor& ensure_actor(std::string_view entity_id);
    [[nodiscard]] std::string apply_effect(std::string_view source_entity_id,std::string_view target_entity_id,
                                           const GameplayEffectDefinition& definition,GameplayRuntime& events);
    std::vector<GameplayAbilityDefinition> definitions_;
    std::vector<GameplayEffectDefinition> effect_definitions_;
    std::vector<GameplayAbilityActor> actors_;
    std::vector<ActiveGameplayEffect> active_effects_;
    std::uint64_t next_activation_id_{1};
    std::uint64_t next_effect_instance_id_{1};
    std::uint64_t revision_{1};
};

} // namespace noemancer
