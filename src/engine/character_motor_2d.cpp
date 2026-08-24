#include "engine/character_motor_2d.hpp"

#include <algorithm>
#include <cmath>

namespace noemancer {
namespace {
float approach(const float value, const float target, const float amount) {
    return value < target ? std::min(value + amount, target) : std::max(value - amount, target);
}
}

CharacterMotor2DResult update_character_motor_2d(
    const CharacterMotor2DConfig& config,
    const CharacterMotor2DState& previous,
    const CharacterMotor2DInput& input,
    const float current_velocity_x,
    const float current_velocity_y,
    const float delta_seconds) {
    const auto dt = std::max(delta_seconds, 0.0F);
    auto state = previous;
    const auto was_grounded = state.grounded;
    state.grounded = input.ground_hit && input.ground_normal_y >= config.minimum_ground_normal_y && current_velocity_y <= 0.5F;
    state.ground_entity_id = state.grounded ? input.ground_entity_id : std::string{};
    state.ground_normal_x = state.grounded ? input.ground_normal_x : 0.0F;
    state.ground_normal_y = state.grounded ? input.ground_normal_y : 1.0F;
    state.wall_entity_id = input.wall_hit ? input.wall_entity_id : std::string{};
    state.wall_normal_x = input.wall_hit ? input.wall_normal_x : 0.0F;
    state.move_input = std::clamp(input.move, -1.0F, 1.0F);
    state.coyote_remaining = state.grounded ? config.coyote_time_seconds : std::max(0.0F, state.coyote_remaining - dt);
    state.jump_buffer_remaining = input.jump_held && !previous.jump_was_held ? config.jump_buffer_seconds :
        std::max(0.0F, state.jump_buffer_remaining - dt);
    state.jump_was_held = input.jump_held;
    if (state.grounded && !was_grounded) ++state.landing_count;

    const auto tangent_x = state.grounded ? std::max(state.ground_normal_y, 0.0F) : 1.0F;
    const auto target_x = state.move_input * config.maximum_speed * tangent_x + (state.grounded ? input.ground_velocity_x : 0.0F);
    const auto acceleration = std::abs(state.move_input) > 0.001F ?
        (state.grounded ? config.ground_acceleration : config.air_acceleration) :
        (state.grounded ? config.ground_deceleration : config.air_acceleration);
    auto velocity_x = approach(current_velocity_x, target_x, acceleration * dt);
    auto velocity_y = std::max(current_velocity_y, -config.maximum_fall_speed);

    if (state.jump_buffer_remaining > 0.0F && state.coyote_remaining > 0.0F) {
        velocity_y = config.jump_speed;
        state.grounded = false;
        state.ground_entity_id.clear();
        state.coyote_remaining = 0.0F;
        state.jump_buffer_remaining = 0.0F;
        state.decision = "jump";
        state.reason = was_grounded ? "buffered-input-on-ground" : "coyote-window";
        ++state.jump_count;
    } else if (!input.jump_held && previous.jump_was_held && velocity_y > 0.0F) {
        velocity_y *= std::clamp(config.jump_release_velocity_factor, 0.0F, 1.0F);
        state.decision = "jump-cut";
        state.reason = "jump-released-during-ascent";
    } else if (input.wall_hit && state.move_input * input.wall_normal_x < 0.0F) {
        velocity_x = 0.0F;
        state.decision = "wall-blocked";
        state.reason = "move-input-into-wall";
    } else if (std::abs(state.move_input) > 0.001F) {
        state.decision = state.grounded ? "ground-move" : "air-control";
        state.reason = state.grounded ? "move-input-and-walkable-ground" : "move-input-while-airborne";
    } else {
        state.decision = state.grounded ? "ground-brake" : "airborne";
        state.reason = state.grounded ? "no-move-input" : "no-walkable-ground";
    }
    if (state.grounded && state.decision != "jump") {
        velocity_y = input.ground_velocity_y - state.move_input * config.maximum_speed * state.ground_normal_x;
    }
    return {velocity_x, velocity_y, std::move(state)};
}

} // namespace noemancer
