#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <span>
#include <array>
#include "engine/audio_asset.hpp"

namespace noemancer {

enum class InputActionKind { button, axis_1d };

struct InputBinding final {
    std::string source;
    float scale{1.0F};
    float dead_zone{};
};

struct InputActionDefinition final {
    std::string id;
    InputActionKind kind{InputActionKind::button};
    std::vector<InputBinding> bindings;
};

struct InputActionState final {
    std::string id;
    InputActionKind kind{InputActionKind::button};
    std::vector<InputBinding> bindings;
    float value{};
    float previous_value{};
    bool pressed{};
    bool released{};
};

class InputActionRuntime final {
public:
    InputActionRuntime();
    [[nodiscard]] bool define(std::string id, InputActionKind kind, std::vector<InputBinding> bindings);
    [[nodiscard]] bool configure(std::span<const InputActionDefinition> definitions);
    [[nodiscard]] bool reset_defaults();
    [[nodiscard]] bool set_source_value(std::string_view source, float value);
    void evaluate();
    [[nodiscard]] const std::vector<InputActionState>& actions() const noexcept { return actions_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::string observe_json() const;

private:
    std::vector<InputActionState> actions_;
    std::unordered_map<std::string, float> source_values_;
    std::uint64_t revision_{1};
};

[[nodiscard]] std::vector<InputActionDefinition> default_input_action_definitions();

struct AudioBusState final {
    std::string id;
    std::string parent;
    float gain{1.0F};
    bool muted{};
};

struct AudioVoiceState final {
    std::uint64_t id{};
    std::string asset_id;
    std::string bus_id;
    float gain{1.0F};
    float pitch{1.0F};
    bool looping{};
    bool playing{true};
    double phase{};
    double clip_cursor{};
    std::uint64_t rendered_frames{};
    bool spatial{};
    std::array<float,3> position{};
    float minimum_distance{1.0F};
    float maximum_distance{50.0F};
    float rolloff{1.0F};
};

enum class AudioAssetStorage : std::uint8_t { resident, stream };

// Stable engine-domain description of an authored audio asset. Source paths,
// VFS handles and miniaudio objects belong to the Runtime adapter catalog.
struct AudioAssetDescriptor final {
    std::string asset_id;
    std::string content_hash;
    AudioAssetStorage storage{AudioAssetStorage::resident};
};

// Immutable publication contract between authored World state and an audio
// backend. It contains engine-domain values only; backend/node-graph types stay
// behind their adapters.
struct AudioRenderSnapshot final {
    std::uint64_t revision{};
    std::vector<AudioBusState> buses;
    std::vector<AudioVoiceState> voices;
    std::vector<AudioAssetDescriptor> assets;
    std::vector<std::shared_ptr<const AudioClip>> clips;
    std::array<float,3> listener_position{};
    std::array<float,3> listener_forward{0.0F,0.0F,-1.0F};
    std::array<float,3> listener_up{0.0F,1.0F,0.0F};
};

class AudioMixerRuntime final {
public:
    AudioMixerRuntime();
    [[nodiscard]] bool set_bus(std::string_view id, float gain, bool muted);
    [[nodiscard]] std::uint64_t play(std::string asset_id, std::string bus_id, float gain, float pitch, bool looping);
    [[nodiscard]] bool stop(std::uint64_t voice_id);
    [[nodiscard]] bool set_listener(std::array<float,3> position,std::array<float,3> forward,std::array<float,3> up);
    [[nodiscard]] bool set_voice_spatial(std::uint64_t voice_id,bool spatial,std::array<float,3> position,
                                         float minimum_distance,float maximum_distance,float rolloff);
    [[nodiscard]] bool register_clip(AudioClip clip);
    [[nodiscard]] bool register_asset(AudioAssetDescriptor descriptor);
    // Reconciles authored state while preserving the destination's real-time
    // transport cursors. This lets an audio worker consume immutable snapshots
    // without reading or mutating the gameplay World concurrently.
    [[nodiscard]] AudioRenderSnapshot render_snapshot() const;
    void synchronize_from(const AudioRenderSnapshot& authored);
    void mix_stereo(std::span<float> interleaved_stereo, std::uint32_t sample_rate);
    [[nodiscard]] std::string observe_json() const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    std::vector<AudioBusState> buses_;
    std::vector<AudioVoiceState> voices_;
    std::unordered_map<std::string,std::shared_ptr<const AudioClip>> clips_;
    std::unordered_map<std::string,AudioAssetDescriptor> assets_;
    std::uint64_t next_voice_id_{1};
    std::uint64_t revision_{1};
    std::uint64_t mixed_frames_{};
    std::array<float,3> listener_position_{};
    std::array<float,3> listener_forward_{0.0F,0.0F,-1.0F};
    std::array<float,3> listener_up_{0.0F,1.0F,0.0F};
};

struct GameplayEvent final {
    std::uint64_t sequence{};
    std::string type;
    std::string source;
    std::string target;
    std::string payload_json;
};

class GameplayRuntime final {
public:
    void update_from_input(const InputActionRuntime& input);
    std::uint64_t emit(std::string type, std::string source, std::string target, std::string payload_json = "{}");
    [[nodiscard]] std::string observe_json(std::size_t max_events = 32) const;
    [[nodiscard]] std::span<const GameplayEvent> events() const noexcept { return events_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
    std::vector<GameplayEvent> events_;
    std::uint64_t next_sequence_{1};
    std::uint64_t revision_{1};
};

} // namespace noemancer
