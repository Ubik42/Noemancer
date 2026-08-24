#include "runtime/input_source_adapter.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <string>

namespace {

using noemancer::InputCaptureState;
using noemancer::InputDeviceDescriptor;
using noemancer::InputDeviceKind;
using noemancer::InputEventPhase;
using noemancer::InputPhysicalKind;
using noemancer::InputSample;
using noemancer::InputSourceAdapter;
using noemancer::MouseAxis;

bool check(const bool condition, const char* message, const int code) {
    static_cast<void>(code);
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

InputSample key(const std::uint32_t scancode, const InputEventPhase phase, const std::uint64_t sequence,
                const bool repeat = false) {
    return {InputPhysicalKind::keyboard_scancode, scancode, phase == InputEventPhase::pressed ? 1.0F : 0.0F,
            phase, sequence, 0, repeat};
}

} // namespace

int main() {
    const auto space = noemancer::normalize_keyboard_scancode(SDL_SCANCODE_SPACE);
    if (!check(space && space->id == "keyboard.space" && space->physical_code == SDL_SCANCODE_SPACE,
               "Space scancode was not normalized to the stable source ID", 1)) return 1;
    const auto left_shift = noemancer::normalize_keyboard_scancode(SDL_SCANCODE_LSHIFT);
    if (!check(left_shift && left_shift->id == "keyboard.left-shift", "Modifier source ID was not canonical", 2)) return 2;
    if (!check(!noemancer::normalize_keyboard_scancode(SDL_SCANCODE_UNKNOWN), "Unknown scancode did not fail closed", 3)) return 3;

    const auto mouse_button = noemancer::normalize_mouse_button(SDL_BUTTON_X2);
    const auto mouse_wheel = noemancer::normalize_mouse_axis(MouseAxis::wheel_y);
    const auto gamepad_button = noemancer::normalize_gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH);
    const auto gamepad_axis = noemancer::normalize_gamepad_axis(SDL_GAMEPAD_AXIS_LEFTX);
    if (!check(mouse_button && mouse_button->id == "mouse.button.x2" &&
                   mouse_wheel && mouse_wheel->id == "mouse.wheel.y" &&
                   gamepad_button && gamepad_button->id == "gamepad.south" &&
                   gamepad_axis && gamepad_axis->id == "gamepad.left.x",
               "SDL mouse/gamepad controls were not normalized", 4)) return 4;

    InputSourceAdapter adapter;
    if (!check(adapter.connect_device({InputDeviceKind::keyboard, 0, "", "Keyboard", true}),
               "Keyboard device registration failed", 5) ||
        !check(adapter.connect_device({InputDeviceKind::mouse, 0, "", "Mouse", true}),
               "Mouse device registration failed", 6) ||
        !check(adapter.connect_device({InputDeviceKind::gamepad, 42, "030000005e0400008e02000000000000", "Pad", true}),
               "Gamepad device registration failed", 7)) return 7;

    noemancer::InputCaptureOptions capture_options;
    capture_options.axis_threshold = 0.5F;
    if (!check(adapter.begin_capture(1001, capture_options), "Capture did not arm", 8)) return 8;
    if (!check(adapter.ingest(key(SDL_SCANCODE_SPACE, InputEventPhase::pressed, 1, true)),
               "A repeated key event was rejected instead of being observed", 9) ||
        !check(adapter.capture_state() == InputCaptureState::armed,
               "Repeated key event incorrectly satisfied capture", 10)) return 10;
    if (!check(adapter.ingest(key(SDL_SCANCODE_SPACE, InputEventPhase::pressed, 2)),
               "Key press was not ingested", 11) ||
        !check(adapter.capture_state() == InputCaptureState::captured,
               "The next eligible key was not captured", 12) ||
        !check(adapter.captured_input() && adapter.captured_input()->source.id == "keyboard.space" &&
                   adapter.captured_input()->sequence == 2,
               "Captured source did not retain stable identity and sequence", 13)) return 13;

    const auto captured_json = nlohmann::json::parse(adapter.observe_json());
    if (!check(captured_json.at("schemaVersion") == "noemancer.input-sources/0.1" &&
                   captured_json.at("capture").at("state") == "captured" &&
                   captured_json.at("capture").at("captured").at("id") == "keyboard.space" &&
                   captured_json.at("sourceCount") == 1U,
               "Capture semantic observation was incomplete", 14)) return 14;

    noemancer::InputCaptureOptions axis_options;
    axis_options.axis_threshold = 0.5F;
    axis_options.mouse_motion = true;
    if (!check(adapter.begin_capture(1002, axis_options), "Axis capture did not re-arm", 15)) return 15;
    if (!check(adapter.ingest({InputPhysicalKind::mouse_axis, static_cast<std::uint32_t>(MouseAxis::x), 0.25F,
                               InputEventPhase::changed, 3, 0, false}) &&
                   adapter.capture_state() == InputCaptureState::armed,
               "Sub-threshold mouse motion incorrectly captured", 16)) return 16;
    if (!check(adapter.ingest({InputPhysicalKind::mouse_axis, static_cast<std::uint32_t>(MouseAxis::x), 0.75F,
                               InputEventPhase::changed, 4, 0, false}) &&
                   adapter.capture_state() == InputCaptureState::captured &&
                   adapter.captured_input() && adapter.captured_input()->source.id == "mouse.axis.x",
               "Eligible mouse axis was not captured", 17)) return 17;

    if (!check(!adapter.ingest(key(SDL_SCANCODE_A, InputEventPhase::pressed, 3)),
               "Stale input sequence was accepted", 18)) return 18;
    if (!check(adapter.begin_capture(1003) && !adapter.cancel_capture(1002) && adapter.cancel_capture(1003) &&
                   adapter.capture_state() == InputCaptureState::cancelled,
               "Capture cancellation did not enforce request identity", 19)) return 19;

    if (!check(adapter.disconnect_device(InputDeviceKind::gamepad, 42), "Gamepad disconnect was not observed", 20) ||
        !check(!adapter.ingest({InputPhysicalKind::gamepad_button, SDL_GAMEPAD_BUTTON_SOUTH, 1.0F,
                                InputEventPhase::pressed, 5, 42, false}),
               "Input from a known disconnected device was accepted", 21)) return 21;

    InputSourceAdapter first;
    InputSourceAdapter second;
    const InputDeviceDescriptor keyboard{InputDeviceKind::keyboard, 0, "", "Keyboard", true};
    const InputDeviceDescriptor mouse{InputDeviceKind::mouse, 0, "", "Mouse", true};
    const InputDeviceDescriptor pad{InputDeviceKind::gamepad, 7, "pad-guid", "Pad", true};
    if (!first.connect_device(keyboard) || !first.connect_device(mouse) || !first.connect_device(pad) ||
        !second.connect_device(pad) || !second.connect_device(mouse) || !second.connect_device(keyboard) ||
        !first.ingest(key(SDL_SCANCODE_A, InputEventPhase::pressed, 1)) ||
        !second.ingest(key(SDL_SCANCODE_A, InputEventPhase::pressed, 1)) ||
        !first.ingest({InputPhysicalKind::gamepad_axis, SDL_GAMEPAD_AXIS_RIGHTY, -0.8F,
                       InputEventPhase::changed, 2, 7, false}) ||
        !second.ingest({InputPhysicalKind::gamepad_axis, SDL_GAMEPAD_AXIS_RIGHTY, -0.8F,
                        InputEventPhase::changed, 2, 7, false})) return 22;
    if (!check(first.observe_json() == second.observe_json(),
               "Equivalent input histories did not produce deterministic JSON", 23)) return 23;

    for (std::uint32_t code = SDL_SCANCODE_A; code <= SDL_SCANCODE_Z; ++code) {
        if (!adapter.ingest(key(code, InputEventPhase::pressed, adapter.last_sequence() + 1U))) return 24;
    }
    const auto bounded = adapter.observe_json(512);
    if (!check(bounded.size() <= 512U, "Bounded semantic JSON exceeded its caller budget", 25)) return 25;
    const auto bounded_json = nlohmann::json::parse(bounded);
    if (!check(bounded_json.at("truncated").get<bool>(), "Bounded observation did not disclose truncation", 26)) return 26;

    if (!check(std::isfinite(bounded_json.at("revision").get<double>()), "Observation revision was not finite", 27)) return 27;
    return 0;
}
