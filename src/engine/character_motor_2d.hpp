#pragma once

#include <cstdint>
#include <string>

namespace noemancer {

struct CharacterMotor2DConfig final {
    float maximum_speed{7.0F};
    float ground_acceleration{55.0F};
    float air_acceleration{24.0F};
    float ground_deceleration{70.0F};
    float jump_speed{10.5F};
    float maximum_fall_speed{24.0F};
    float coyote_time_seconds{0.10F};
    float jump_buffer_seconds{0.12F};
    float ground_probe_distance{0.16F};
    float minimum_ground_normal_y{0.65F};
    float jump_release_velocity_factor{0.45F};
};

struct CharacterMotor2DState final {
    bool grounded{};
    bool jump_was_held{};
    float move_input{};
    float coyote_remaining{};
    float jump_buffer_remaining{};
    std::string ground_entity_id;
    std::string wall_entity_id;
    float ground_normal_x{};
    float ground_normal_y{1.0F};
    float wall_normal_x{};
    std::string decision{"idle"};
    std::string reason{"no-input"};
    std::uint64_t jump_count{};
    std::uint64_t landing_count{};
};

struct CharacterMotor2DInput final {
    float move{};
    bool jump_held{};
    bool ground_hit{};
    float ground_normal_x{};
    float ground_normal_y{1.0F};
    std::string ground_entity_id;
    bool wall_hit{};
    float wall_normal_x{};
    std::string wall_entity_id;
    float ground_velocity_x{};
    float ground_velocity_y{};
};

struct CharacterMotor2DResult final {
    float velocity_x{};
    float velocity_y{};
    CharacterMotor2DState state;
};

[[nodiscard]] CharacterMotor2DResult update_character_motor_2d(
    const CharacterMotor2DConfig& config,
    const CharacterMotor2DState& previous,
    const CharacterMotor2DInput& input,
    float current_velocity_x,
    float current_velocity_y,
    float delta_seconds);

} // namespace noemancer
