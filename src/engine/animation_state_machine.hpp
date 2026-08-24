#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noemancer {

struct AnimationStateParameterDefinition final {
    std::string id;
    std::string type{"float"};
    float default_value{};
};

struct AnimationStateDefinition final {
    std::string id;
    std::string clip_asset;
    bool looping{true};
};

struct AnimationTransitionCondition final {
    std::string source{"parameter"};
    std::string parameter;
    std::string comparison{"greater"};
    float threshold{};
    std::string cue;
};

struct AnimationTransitionDefinition final {
    std::string id;
    std::string from;
    std::string to;
    std::vector<AnimationTransitionCondition> conditions;
    float duration_seconds{0.15F};
    std::int32_t priority{};
};

struct AnimationStateMachineDocument final {
    std::string asset_id;
    std::string initial_state;
    std::vector<AnimationStateParameterDefinition> parameters;
    std::vector<AnimationStateDefinition> states;
    std::vector<AnimationTransitionDefinition> transitions;
};

struct AnimationStateMachineParseResult final {
    std::optional<AnimationStateMachineDocument> document;
    std::string code;
    std::string detail;
    explicit operator bool() const noexcept { return document.has_value(); }
};

struct AnimationStateMachineEvaluation final {
    bool valid{};
    bool transitioned{};
    std::string code;
    std::string from;
    std::string to;
    std::string transition_id;
    std::string clip_asset;
    bool looping{true};
    float duration_seconds{};
};

class AnimationStateMachineCodec final {
public:
    [[nodiscard]] static AnimationStateMachineParseResult parse_json(std::string_view source);
    [[nodiscard]] static std::string write_canonical_json(const AnimationStateMachineDocument& document);
    [[nodiscard]] static std::vector<std::string> asset_dependencies(const AnimationStateMachineDocument& document);
};

class AnimationStateMachineLibrary final {
public:
    AnimationStateMachineLibrary();
    [[nodiscard]] bool register_document(AnimationStateMachineDocument document);
    [[nodiscard]] const AnimationStateMachineDocument* find(std::string_view asset_id) const;
    [[nodiscard]] AnimationStateMachineEvaluation evaluate(std::string_view asset_id,std::string_view active_state,
        const std::unordered_map<std::string,float>& parameters,std::string_view gameplay_cue,float state_elapsed_seconds) const;
    [[nodiscard]] std::string inspect_json(std::string_view asset_id) const;

private:
    std::unordered_map<std::string,AnimationStateMachineDocument> documents_;
};

} // namespace noemancer
