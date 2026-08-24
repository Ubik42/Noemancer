#include "runtime/input_source_adapter.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace noemancer {

namespace {

using Json = nlohmann::json;

constexpr std::size_t min_observation_bytes = 256U;
constexpr float minimum_axis_threshold = 0.01F;

NormalizedInputSource make_source(const InputDeviceKind device, const InputControlKind control,
                                  const std::string_view id, const std::uint32_t physical_code) {
    return {device, control, std::string(id), physical_code};
}

std::optional<std::string> keyboard_name(const std::uint32_t scancode) {
    // Letters, number row, function keys and keypad digits are contiguous in
    // SDL's USB scancode table.  The generated IDs do not depend on a layout
    // or on SDL_GetScancodeName's locale/display spelling.
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return std::string("keyboard.") + static_cast<char>('a' + (scancode - SDL_SCANCODE_A));
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return "keyboard." + std::to_string(scancode - SDL_SCANCODE_1 + 1U);
    }
    if (scancode == SDL_SCANCODE_0) return "keyboard.0";
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F24) {
        return "keyboard.f" + std::to_string(scancode - SDL_SCANCODE_F1 + 1U);
    }
    if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) {
        return "keyboard.kp-" + std::to_string(scancode - SDL_SCANCODE_KP_1 + 1U);
    }
    if (scancode == SDL_SCANCODE_KP_0) return "keyboard.kp-0";

    switch (static_cast<SDL_Scancode>(scancode)) {
    case SDL_SCANCODE_RETURN: return "keyboard.return";
    case SDL_SCANCODE_ESCAPE: return "keyboard.escape";
    case SDL_SCANCODE_BACKSPACE: return "keyboard.backspace";
    case SDL_SCANCODE_TAB: return "keyboard.tab";
    case SDL_SCANCODE_SPACE: return "keyboard.space";
    case SDL_SCANCODE_MINUS: return "keyboard.minus";
    case SDL_SCANCODE_EQUALS: return "keyboard.equals";
    case SDL_SCANCODE_LEFTBRACKET: return "keyboard.left-bracket";
    case SDL_SCANCODE_RIGHTBRACKET: return "keyboard.right-bracket";
    case SDL_SCANCODE_BACKSLASH: return "keyboard.backslash";
    case SDL_SCANCODE_NONUSHASH: return "keyboard.non-us-hash";
    case SDL_SCANCODE_SEMICOLON: return "keyboard.semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "keyboard.apostrophe";
    case SDL_SCANCODE_GRAVE: return "keyboard.grave";
    case SDL_SCANCODE_COMMA: return "keyboard.comma";
    case SDL_SCANCODE_PERIOD: return "keyboard.period";
    case SDL_SCANCODE_SLASH: return "keyboard.slash";
    case SDL_SCANCODE_CAPSLOCK: return "keyboard.caps-lock";
    case SDL_SCANCODE_PRINTSCREEN: return "keyboard.print-screen";
    case SDL_SCANCODE_SCROLLLOCK: return "keyboard.scroll-lock";
    case SDL_SCANCODE_PAUSE: return "keyboard.pause";
    case SDL_SCANCODE_INSERT: return "keyboard.insert";
    case SDL_SCANCODE_HOME: return "keyboard.home";
    case SDL_SCANCODE_PAGEUP: return "keyboard.page-up";
    case SDL_SCANCODE_DELETE: return "keyboard.delete";
    case SDL_SCANCODE_END: return "keyboard.end";
    case SDL_SCANCODE_PAGEDOWN: return "keyboard.page-down";
    case SDL_SCANCODE_RIGHT: return "keyboard.right";
    case SDL_SCANCODE_LEFT: return "keyboard.left";
    case SDL_SCANCODE_DOWN: return "keyboard.down";
    case SDL_SCANCODE_UP: return "keyboard.up";
    case SDL_SCANCODE_NUMLOCKCLEAR: return "keyboard.num-lock";
    case SDL_SCANCODE_KP_DIVIDE: return "keyboard.kp-divide";
    case SDL_SCANCODE_KP_MULTIPLY: return "keyboard.kp-multiply";
    case SDL_SCANCODE_KP_MINUS: return "keyboard.kp-minus";
    case SDL_SCANCODE_KP_PLUS: return "keyboard.kp-plus";
    case SDL_SCANCODE_KP_ENTER: return "keyboard.kp-enter";
    case SDL_SCANCODE_KP_PERIOD: return "keyboard.kp-period";
    case SDL_SCANCODE_NONUSBACKSLASH: return "keyboard.non-us-backslash";
    case SDL_SCANCODE_APPLICATION: return "keyboard.application";
    case SDL_SCANCODE_POWER: return "keyboard.power";
    case SDL_SCANCODE_KP_EQUALS: return "keyboard.kp-equals";
    case SDL_SCANCODE_EXECUTE: return "keyboard.execute";
    case SDL_SCANCODE_HELP: return "keyboard.help";
    case SDL_SCANCODE_MENU: return "keyboard.menu";
    case SDL_SCANCODE_SELECT: return "keyboard.select";
    case SDL_SCANCODE_STOP: return "keyboard.stop";
    case SDL_SCANCODE_AGAIN: return "keyboard.again";
    case SDL_SCANCODE_UNDO: return "keyboard.undo";
    case SDL_SCANCODE_CUT: return "keyboard.cut";
    case SDL_SCANCODE_COPY: return "keyboard.copy";
    case SDL_SCANCODE_PASTE: return "keyboard.paste";
    case SDL_SCANCODE_FIND: return "keyboard.find";
    case SDL_SCANCODE_MUTE: return "keyboard.mute";
    case SDL_SCANCODE_VOLUMEUP: return "keyboard.volume-up";
    case SDL_SCANCODE_VOLUMEDOWN: return "keyboard.volume-down";
    case SDL_SCANCODE_KP_COMMA: return "keyboard.kp-comma";
    case SDL_SCANCODE_KP_EQUALSAS400: return "keyboard.kp-equals-as400";
    case SDL_SCANCODE_ALTERASE: return "keyboard.alt-erase";
    case SDL_SCANCODE_SYSREQ: return "keyboard.sys-req";
    case SDL_SCANCODE_CANCEL: return "keyboard.cancel";
    case SDL_SCANCODE_CLEAR: return "keyboard.clear";
    case SDL_SCANCODE_PRIOR: return "keyboard.prior";
    case SDL_SCANCODE_RETURN2: return "keyboard.return2";
    case SDL_SCANCODE_SEPARATOR: return "keyboard.separator";
    case SDL_SCANCODE_OUT: return "keyboard.out";
    case SDL_SCANCODE_OPER: return "keyboard.oper";
    case SDL_SCANCODE_CLEARAGAIN: return "keyboard.clear-again";
    case SDL_SCANCODE_CRSEL: return "keyboard.crsel";
    case SDL_SCANCODE_EXSEL: return "keyboard.exsel";
    case SDL_SCANCODE_KP_00: return "keyboard.kp-00";
    case SDL_SCANCODE_KP_000: return "keyboard.kp-000";
    case SDL_SCANCODE_KP_LEFTPAREN: return "keyboard.kp-left-paren";
    case SDL_SCANCODE_KP_RIGHTPAREN: return "keyboard.kp-right-paren";
    case SDL_SCANCODE_KP_LEFTBRACE: return "keyboard.kp-left-brace";
    case SDL_SCANCODE_KP_RIGHTBRACE: return "keyboard.kp-right-brace";
    case SDL_SCANCODE_KP_TAB: return "keyboard.kp-tab";
    case SDL_SCANCODE_KP_BACKSPACE: return "keyboard.kp-backspace";
    case SDL_SCANCODE_KP_A: return "keyboard.kp-a";
    case SDL_SCANCODE_KP_B: return "keyboard.kp-b";
    case SDL_SCANCODE_KP_C: return "keyboard.kp-c";
    case SDL_SCANCODE_KP_D: return "keyboard.kp-d";
    case SDL_SCANCODE_KP_E: return "keyboard.kp-e";
    case SDL_SCANCODE_KP_F: return "keyboard.kp-f";
    case SDL_SCANCODE_KP_XOR: return "keyboard.kp-xor";
    case SDL_SCANCODE_KP_POWER: return "keyboard.kp-power";
    case SDL_SCANCODE_KP_PERCENT: return "keyboard.kp-percent";
    case SDL_SCANCODE_KP_LESS: return "keyboard.kp-less";
    case SDL_SCANCODE_KP_GREATER: return "keyboard.kp-greater";
    case SDL_SCANCODE_KP_AMPERSAND: return "keyboard.kp-ampersand";
    case SDL_SCANCODE_KP_DBLAMPERSAND: return "keyboard.kp-double-ampersand";
    case SDL_SCANCODE_KP_VERTICALBAR: return "keyboard.kp-vertical-bar";
    case SDL_SCANCODE_KP_DBLVERTICALBAR: return "keyboard.kp-double-vertical-bar";
    case SDL_SCANCODE_KP_COLON: return "keyboard.kp-colon";
    case SDL_SCANCODE_KP_HASH: return "keyboard.kp-hash";
    case SDL_SCANCODE_KP_SPACE: return "keyboard.kp-space";
    case SDL_SCANCODE_KP_AT: return "keyboard.kp-at";
    case SDL_SCANCODE_KP_EXCLAM: return "keyboard.kp-exclam";
    case SDL_SCANCODE_KP_MEMSTORE: return "keyboard.kp-mem-store";
    case SDL_SCANCODE_KP_MEMRECALL: return "keyboard.kp-mem-recall";
    case SDL_SCANCODE_KP_MEMCLEAR: return "keyboard.kp-mem-clear";
    case SDL_SCANCODE_KP_MEMADD: return "keyboard.kp-mem-add";
    case SDL_SCANCODE_KP_MEMSUBTRACT: return "keyboard.kp-mem-subtract";
    case SDL_SCANCODE_KP_MEMMULTIPLY: return "keyboard.kp-mem-multiply";
    case SDL_SCANCODE_KP_MEMDIVIDE: return "keyboard.kp-mem-divide";
    case SDL_SCANCODE_KP_PLUSMINUS: return "keyboard.kp-plus-minus";
    case SDL_SCANCODE_KP_CLEAR: return "keyboard.kp-clear";
    case SDL_SCANCODE_KP_CLEARENTRY: return "keyboard.kp-clear-entry";
    case SDL_SCANCODE_KP_BINARY: return "keyboard.kp-binary";
    case SDL_SCANCODE_KP_OCTAL: return "keyboard.kp-octal";
    case SDL_SCANCODE_KP_DECIMAL: return "keyboard.kp-decimal";
    case SDL_SCANCODE_KP_HEXADECIMAL: return "keyboard.kp-hexadecimal";
    case SDL_SCANCODE_LCTRL: return "keyboard.left-control";
    case SDL_SCANCODE_LSHIFT: return "keyboard.left-shift";
    case SDL_SCANCODE_LALT: return "keyboard.left-alt";
    case SDL_SCANCODE_LGUI: return "keyboard.left-gui";
    case SDL_SCANCODE_RCTRL: return "keyboard.right-control";
    case SDL_SCANCODE_RSHIFT: return "keyboard.right-shift";
    case SDL_SCANCODE_RALT: return "keyboard.right-alt";
    case SDL_SCANCODE_RGUI: return "keyboard.right-gui";
    case SDL_SCANCODE_MODE: return "keyboard.mode";
    case SDL_SCANCODE_MEDIA_NEXT_TRACK: return "keyboard.media-next";
    case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK: return "keyboard.media-previous";
    case SDL_SCANCODE_MEDIA_STOP: return "keyboard.media-stop";
    case SDL_SCANCODE_MEDIA_PLAY: return "keyboard.media-play";
    case SDL_SCANCODE_MEDIA_SELECT: return "keyboard.media-select";
    case SDL_SCANCODE_MEDIA_EJECT: return "keyboard.media-eject";
    case SDL_SCANCODE_MEDIA_REWIND: return "keyboard.media-rewind";
    case SDL_SCANCODE_MEDIA_FAST_FORWARD: return "keyboard.media-fast-forward";
    case SDL_SCANCODE_MEDIA_PAUSE: return "keyboard.media-pause";
    case SDL_SCANCODE_MEDIA_PLAY_PAUSE: return "keyboard.media-play-pause";
    case SDL_SCANCODE_MEDIA_RECORD: return "keyboard.media-record";
    case SDL_SCANCODE_CHANNEL_INCREMENT: return "keyboard.channel-up";
    case SDL_SCANCODE_CHANNEL_DECREMENT: return "keyboard.channel-down";
    case SDL_SCANCODE_AC_SEARCH: return "keyboard.ac-search";
    case SDL_SCANCODE_AC_HOME: return "keyboard.ac-home";
    case SDL_SCANCODE_AC_BACK: return "keyboard.ac-back";
    case SDL_SCANCODE_AC_FORWARD: return "keyboard.ac-forward";
    case SDL_SCANCODE_AC_STOP: return "keyboard.ac-stop";
    case SDL_SCANCODE_AC_REFRESH: return "keyboard.ac-refresh";
    case SDL_SCANCODE_AC_BOOKMARKS: return "keyboard.ac-bookmarks";
    case SDL_SCANCODE_SOFTLEFT: return "keyboard.soft-left";
    case SDL_SCANCODE_SOFTRIGHT: return "keyboard.soft-right";
    case SDL_SCANCODE_CALL: return "keyboard.call";
    case SDL_SCANCODE_ENDCALL: return "keyboard.end-call";
    case SDL_SCANCODE_RESERVED: return std::nullopt;
    case SDL_SCANCODE_UNKNOWN: return std::nullopt;
    default: break;
    }

    // Language and international keys are valid physical controls, but the
    // numeric suffix is the stable SDL enum identity rather than a localized
    // display label.
    if (scancode >= SDL_SCANCODE_INTERNATIONAL1 && scancode <= SDL_SCANCODE_INTERNATIONAL9) {
        return "keyboard.international-" + std::to_string(scancode - SDL_SCANCODE_INTERNATIONAL1 + 1U);
    }
    if (scancode >= SDL_SCANCODE_LANG1 && scancode <= SDL_SCANCODE_LANG9) {
        return "keyboard.lang-" + std::to_string(scancode - SDL_SCANCODE_LANG1 + 1U);
    }
    return std::nullopt;
}

const char* device_kind_name(const InputDeviceKind kind) noexcept {
    switch (kind) {
    case InputDeviceKind::keyboard: return "keyboard";
    case InputDeviceKind::mouse: return "mouse";
    case InputDeviceKind::gamepad: return "gamepad";
    }
    return "unknown";
}

const char* control_kind_name(const InputControlKind kind) noexcept {
    switch (kind) {
    case InputControlKind::button: return "button";
    case InputControlKind::axis: return "axis";
    case InputControlKind::wheel: return "wheel";
    }
    return "unknown";
}

const char* capture_state_name(const InputCaptureState state) noexcept {
    switch (state) {
    case InputCaptureState::idle: return "idle";
    case InputCaptureState::armed: return "armed";
    case InputCaptureState::captured: return "captured";
    case InputCaptureState::cancelled: return "cancelled";
    }
    return "unknown";
}

bool same_device(const InputDeviceDescriptor& descriptor, const InputDeviceKind kind,
                 const std::uint64_t instance_id) noexcept {
    return descriptor.kind == kind && descriptor.instance_id == instance_id;
}

bool has_control_char(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](const char character) {
        return static_cast<unsigned char>(character) < 0x20U;
    });
}

std::string stable_gamepad_id(std::string value) {
    std::string result;
    result.reserve(std::min(value.size(), InputSourceAdapter::max_device_text));
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool ascii_alphanumeric = (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
            (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) ||
            (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9'));
        if (ascii_alphanumeric || character == '-' || character == '_' || character == '.') {
            result.push_back(byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')
                ? static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a')) : character);
        }
        if (result.size() == InputSourceAdapter::max_device_text) break;
    }
    return result.empty() ? "gamepad" : result;
}

std::string canonical_device_id(const InputDeviceKind kind, std::string stable_id) {
    if (kind == InputDeviceKind::keyboard) return "keyboard";
    if (kind == InputDeviceKind::mouse) return "mouse";
    return stable_gamepad_id(std::move(stable_id));
}

float normalized_value(const InputSample& sample, const InputControlKind control) {
    if (control == InputControlKind::button) {
        return sample.phase == InputEventPhase::pressed && sample.value > 0.5F ? 1.0F : 0.0F;
    }
    return std::clamp(sample.value, -1.0F, 1.0F);
}

bool is_mouse_motion_axis(const InputSample& sample) {
    if (sample.physical_kind != InputPhysicalKind::mouse_axis) return false;
    const auto axis = static_cast<MouseAxis>(sample.physical_code);
    return axis == MouseAxis::x || axis == MouseAxis::y;
}

} // namespace

std::optional<NormalizedInputSource> normalize_keyboard_scancode(const std::uint32_t scancode) {
    const auto name = keyboard_name(scancode);
    if (!name) return std::nullopt;
    return make_source(InputDeviceKind::keyboard, InputControlKind::button, *name, scancode);
}

std::optional<NormalizedInputSource> normalize_mouse_button(const std::uint32_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return make_source(InputDeviceKind::mouse, InputControlKind::button, "mouse.button.left", button);
    case SDL_BUTTON_RIGHT: return make_source(InputDeviceKind::mouse, InputControlKind::button, "mouse.button.right", button);
    case SDL_BUTTON_MIDDLE: return make_source(InputDeviceKind::mouse, InputControlKind::button, "mouse.button.middle", button);
    case SDL_BUTTON_X1: return make_source(InputDeviceKind::mouse, InputControlKind::button, "mouse.button.x1", button);
    case SDL_BUTTON_X2: return make_source(InputDeviceKind::mouse, InputControlKind::button, "mouse.button.x2", button);
    default: return std::nullopt;
    }
}

std::optional<NormalizedInputSource> normalize_mouse_axis(const MouseAxis axis) {
    switch (axis) {
    case MouseAxis::x: return make_source(InputDeviceKind::mouse, InputControlKind::axis, "mouse.axis.x", 0U);
    case MouseAxis::y: return make_source(InputDeviceKind::mouse, InputControlKind::axis, "mouse.axis.y", 1U);
    case MouseAxis::wheel_x: return make_source(InputDeviceKind::mouse, InputControlKind::wheel, "mouse.wheel.x", 2U);
    case MouseAxis::wheel_y: return make_source(InputDeviceKind::mouse, InputControlKind::wheel, "mouse.wheel.y", 3U);
    }
    return std::nullopt;
}

std::optional<NormalizedInputSource> normalize_gamepad_button(const std::uint32_t button) {
    switch (static_cast<SDL_GamepadButton>(button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.south", button);
    case SDL_GAMEPAD_BUTTON_EAST: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.east", button);
    case SDL_GAMEPAD_BUTTON_WEST: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.west", button);
    case SDL_GAMEPAD_BUTTON_NORTH: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.north", button);
    case SDL_GAMEPAD_BUTTON_BACK: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.back", button);
    case SDL_GAMEPAD_BUTTON_GUIDE: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.guide", button);
    case SDL_GAMEPAD_BUTTON_START: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.start", button);
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.left-stick", button);
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.right-stick", button);
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.left-shoulder", button);
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.right-shoulder", button);
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.dpad-up", button);
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.dpad-down", button);
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.dpad-left", button);
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.dpad-right", button);
    case SDL_GAMEPAD_BUTTON_MISC1: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.misc1", button);
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.right-paddle1", button);
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.left-paddle1", button);
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.right-paddle2", button);
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.left-paddle2", button);
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.touchpad", button);
    case SDL_GAMEPAD_BUTTON_MISC2: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.misc2", button);
    case SDL_GAMEPAD_BUTTON_MISC3: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.misc3", button);
    case SDL_GAMEPAD_BUTTON_MISC4: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.misc4", button);
    case SDL_GAMEPAD_BUTTON_MISC5: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.misc5", button);
    case SDL_GAMEPAD_BUTTON_MISC6: return make_source(InputDeviceKind::gamepad, InputControlKind::button, "gamepad.misc6", button);
    default: return std::nullopt;
    }
}

std::optional<NormalizedInputSource> normalize_gamepad_axis(const std::uint32_t axis) {
    switch (static_cast<SDL_GamepadAxis>(axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX: return make_source(InputDeviceKind::gamepad, InputControlKind::axis, "gamepad.left.x", axis);
    case SDL_GAMEPAD_AXIS_LEFTY: return make_source(InputDeviceKind::gamepad, InputControlKind::axis, "gamepad.left.y", axis);
    case SDL_GAMEPAD_AXIS_RIGHTX: return make_source(InputDeviceKind::gamepad, InputControlKind::axis, "gamepad.right.x", axis);
    case SDL_GAMEPAD_AXIS_RIGHTY: return make_source(InputDeviceKind::gamepad, InputControlKind::axis, "gamepad.right.y", axis);
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return make_source(InputDeviceKind::gamepad, InputControlKind::axis, "gamepad.left-trigger", axis);
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return make_source(InputDeviceKind::gamepad, InputControlKind::axis, "gamepad.right-trigger", axis);
    default: return std::nullopt;
    }
}

std::optional<NormalizedInputSource> normalize_input(const InputSample& sample) {
    switch (sample.physical_kind) {
    case InputPhysicalKind::keyboard_scancode: return normalize_keyboard_scancode(sample.physical_code);
    case InputPhysicalKind::mouse_button: return normalize_mouse_button(sample.physical_code);
    case InputPhysicalKind::mouse_axis: return normalize_mouse_axis(static_cast<MouseAxis>(sample.physical_code));
    case InputPhysicalKind::gamepad_button: return normalize_gamepad_button(sample.physical_code);
    case InputPhysicalKind::gamepad_axis: return normalize_gamepad_axis(sample.physical_code);
    }
    return std::nullopt;
}

bool InputSourceAdapter::connect_device(InputDeviceDescriptor descriptor) {
    if (descriptor.name.size() > max_device_text || descriptor.stable_id.size() > max_device_text ||
        has_control_char(descriptor.name) || has_control_char(descriptor.stable_id)) return false;
    if ((descriptor.kind == InputDeviceKind::keyboard || descriptor.kind == InputDeviceKind::mouse) &&
        descriptor.instance_id != 0U) return false;

    descriptor.stable_id = canonical_device_id(descriptor.kind, std::move(descriptor.stable_id));
    if (descriptor.name.empty()) descriptor.name = descriptor.stable_id;
    const auto found = std::ranges::find_if(devices_, [&](const DeviceState& state) {
        return same_device(state.descriptor, descriptor.kind, descriptor.instance_id);
    });
    if (found != devices_.end()) {
        found->descriptor = std::move(descriptor);
        ++revision_;
        return true;
    }
    if (devices_.size() >= max_devices) return false;
    devices_.push_back({std::move(descriptor)});
    ++revision_;
    return true;
}

bool InputSourceAdapter::disconnect_device(const InputDeviceKind kind, const std::uint64_t instance_id) {
    const auto found = std::ranges::find_if(devices_, [&](const DeviceState& state) {
        return same_device(state.descriptor, kind, instance_id);
    });
    if (found == devices_.end() || !found->descriptor.connected) return false;
    found->descriptor.connected = false;
    ++revision_;
    return true;
}

bool InputSourceAdapter::begin_capture(const std::uint64_t request_id, const InputCaptureOptions options) {
    if (request_id == 0U || !std::isfinite(options.axis_threshold) ||
        options.axis_threshold < minimum_axis_threshold || options.axis_threshold > 1.0F) return false;
    capture_state_ = InputCaptureState::armed;
    capture_request_id_ = request_id;
    capture_options_ = options;
    captured_.reset();
    ++revision_;
    return true;
}

bool InputSourceAdapter::cancel_capture(const std::uint64_t request_id) {
    if (request_id == 0U || request_id != capture_request_id_ || capture_state_ != InputCaptureState::armed) return false;
    capture_state_ = InputCaptureState::cancelled;
    captured_.reset();
    ++revision_;
    return true;
}

bool InputSourceAdapter::device_is_disconnected(const InputDeviceKind kind, const std::uint64_t instance_id) const {
    if (kind != InputDeviceKind::gamepad || instance_id == 0U) return false;
    return std::ranges::any_of(devices_, [&](const DeviceState& state) {
        return same_device(state.descriptor, kind, instance_id) && !state.descriptor.connected;
    });
}

bool InputSourceAdapter::capture_accepts(const InputSample& sample, const NormalizedInputSource& source) const {
    if (capture_state_ != InputCaptureState::armed || (sample.repeat && !capture_options_.repeats)) return false;
    if (source.device == InputDeviceKind::keyboard && !capture_options_.keyboard) return false;
    if (source.device == InputDeviceKind::mouse && !capture_options_.mouse) return false;
    if (source.device == InputDeviceKind::gamepad && !capture_options_.gamepad) return false;
    if (source.control == InputControlKind::button) {
        return sample.phase == InputEventPhase::pressed && sample.value > 0.5F;
    }
    if (!capture_options_.axes || sample.phase != InputEventPhase::changed ||
        std::abs(sample.value) < capture_options_.axis_threshold) return false;
    return !is_mouse_motion_axis(sample) || capture_options_.mouse_motion;
}

bool InputSourceAdapter::ingest(const InputSample& sample) {
    if (!std::isfinite(sample.value)) return false;
    const auto source = normalize_input(sample);
    if (!source || device_is_disconnected(source->device, sample.device_instance)) return false;
    if (source->control == InputControlKind::button) {
        if (sample.phase != InputEventPhase::pressed && sample.phase != InputEventPhase::released) return false;
    } else if (sample.phase != InputEventPhase::changed) {
        return false;
    }

    const auto sequence = sample.sequence == 0U ?
        (last_sequence_ == std::numeric_limits<std::uint64_t>::max() ? 0U : last_sequence_ + 1U) : sample.sequence;
    if (sequence == 0U || sequence <= last_sequence_) return false;
    const auto value = normalized_value(sample, source->control);

    const auto found = std::ranges::find_if(sources_, [&](const SourceState& state) {
        return state.source.id == source->id;
    });
    if (found == sources_.end()) {
        if (sources_.size() >= max_sources) return false;
        sources_.push_back({*source, sample.device_instance, sequence, value,
                            source->control == InputControlKind::button ? value > 0.5F : std::abs(value) > 0.000001F});
    } else {
        found->device_instance = sample.device_instance;
        found->sequence = sequence;
        found->value = value;
        found->active = source->control == InputControlKind::button ? value > 0.5F : std::abs(value) > 0.000001F;
    }
    last_sequence_ = sequence;
    ++revision_;

    if (capture_accepts(sample, *source)) {
        capture_state_ = InputCaptureState::captured;
        captured_ = CapturedInput{*source, sequence, sample.device_instance, value};
        ++revision_;
    }
    return true;
}

std::string InputSourceAdapter::observe_json(const std::size_t byte_budget) const {
    const auto budget = std::clamp(byte_budget, min_observation_bytes, max_observation_bytes);
    std::vector<DeviceState> devices = devices_;
    std::ranges::sort(devices, [](const DeviceState& left, const DeviceState& right) {
        if (left.descriptor.kind != right.descriptor.kind)
            return static_cast<unsigned>(left.descriptor.kind) < static_cast<unsigned>(right.descriptor.kind);
        if (left.descriptor.instance_id != right.descriptor.instance_id)
            return left.descriptor.instance_id < right.descriptor.instance_id;
        return left.descriptor.stable_id < right.descriptor.stable_id;
    });
    std::vector<SourceState> sources = sources_;
    std::ranges::sort(sources, [](const SourceState& left, const SourceState& right) {
        if (left.source.id != right.source.id) return left.source.id < right.source.id;
        return left.device_instance < right.device_instance;
    });

    Json device_json = Json::array();
    for (const auto& state : devices) {
        std::size_t source_count{};
        for (const auto& source : sources) if (source.source.device == state.descriptor.kind) ++source_count;
        device_json.push_back({
            {"kind", device_kind_name(state.descriptor.kind)},
            {"stableId", state.descriptor.stable_id},
            {"instanceId", state.descriptor.instance_id},
            {"name", state.descriptor.name},
            {"connected", state.descriptor.connected},
            {"sourceCount", source_count},
        });
    }

    Json source_json = Json::array();
    for (const auto& state : sources) {
        source_json.push_back({
            {"id", state.source.id},
            {"device", device_kind_name(state.source.device)},
            {"control", control_kind_name(state.source.control)},
            {"physicalCode", state.source.physical_code},
            {"value", state.value},
            {"active", state.active},
            {"sequence", state.sequence},
            {"deviceInstance", state.device_instance},
        });
    }

    Json capture = {
        {"state", capture_state_name(capture_state_)},
        {"requestId", capture_request_id_},
        {"options", {
            {"keyboard", capture_options_.keyboard},
            {"mouse", capture_options_.mouse},
            {"gamepad", capture_options_.gamepad},
            {"axes", capture_options_.axes},
            {"mouseMotion", capture_options_.mouse_motion},
            {"repeats", capture_options_.repeats},
            {"axisThreshold", capture_options_.axis_threshold},
        }},
        {"captured", Json(nullptr)},
    };
    if (captured_) {
        capture["captured"] = {
            {"id", captured_->source.id},
            {"device", device_kind_name(captured_->source.device)},
            {"control", control_kind_name(captured_->source.control)},
            {"sequence", captured_->sequence},
            {"deviceInstance", captured_->device_instance},
            {"value", captured_->value},
        };
    }

    Json result = {
        {"schemaVersion", "noemancer.input-sources/0.1"},
        {"revision", revision_},
        {"lastSequence", last_sequence_},
        {"deviceCount", devices.size()},
        {"sourceCount", sources.size()},
        {"devices", std::move(device_json)},
        {"sources", std::move(source_json)},
        {"capture", std::move(capture)},
        {"limits", {
            {"maxDevices", max_devices},
            {"maxSources", max_sources},
            {"maxDeviceText", max_device_text},
            {"maxObservationBytes", max_observation_bytes},
        }},
        {"truncated", false},
        {"omittedDevices", 0U},
        {"omittedSources", 0U},
    };

    auto serialized = result.dump();
    if (serialized.size() <= budget) return serialized;

    result["truncated"] = true;
    result["omittedDevices"] = devices.size();
    result["omittedSources"] = sources.size();
    result["devices"] = Json::array();
    result["sources"] = Json::array();
    // Preserve the canonical prefix of the sorted catalog when there is room;
    // dropping the tail makes the result independent of insertion order.
    for (const auto& state : devices) {
        Json item = {
            {"kind", device_kind_name(state.descriptor.kind)},
            {"stableId", state.descriptor.stable_id},
            {"instanceId", state.descriptor.instance_id},
            {"name", state.descriptor.name},
            {"connected", state.descriptor.connected},
        };
        result["devices"].push_back(std::move(item));
        serialized = result.dump();
        if (serialized.size() > budget) {
            result["devices"].erase(result["devices"].end() - 1);
            break;
        }
        result["omittedDevices"] = devices.size() - result["devices"].size();
    }
    for (const auto& state : sources) {
        Json item = {
            {"id", state.source.id},
            {"device", device_kind_name(state.source.device)},
            {"control", control_kind_name(state.source.control)},
            {"physicalCode", state.source.physical_code},
            {"value", state.value},
            {"active", state.active},
            {"sequence", state.sequence},
            {"deviceInstance", state.device_instance},
        };
        result["sources"].push_back(std::move(item));
        serialized = result.dump();
        if (serialized.size() > budget) {
            result["sources"].erase(result["sources"].end() - 1);
            break;
        }
        result["omittedSources"] = sources.size() - result["sources"].size();
    }
    serialized = result.dump();
    if (serialized.size() <= budget) return serialized;

    // The envelope remains valid even for a deliberately tiny caller budget.
    // The normal minimum budget is large enough for this status-only form.
    const Json minimal = {
        {"schemaVersion", "noemancer.input-sources/0.1"},
        {"revision", revision_},
        {"lastSequence", last_sequence_},
        {"deviceCount", devices.size()},
        {"sourceCount", sources.size()},
        {"captureState", capture_state_name(capture_state_)},
        {"captureRequestId", capture_request_id_},
        {"truncated", true},
    };
    return minimal.dump();
}

} // namespace noemancer
