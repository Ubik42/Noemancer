#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noemancer {

// These values are deliberately owned by the Runtime adapter.  SDL enums and
// handles stay at the event-loop boundary and never become part of a saved
// binding or semantic observation.
enum class InputDeviceKind : std::uint8_t {
    keyboard,
    mouse,
    gamepad,
};

enum class InputControlKind : std::uint8_t {
    button,
    axis,
    wheel,
};

enum class InputPhysicalKind : std::uint8_t {
    keyboard_scancode,
    mouse_button,
    mouse_axis,
    gamepad_button,
    gamepad_axis,
};

enum class InputEventPhase : std::uint8_t {
    released,
    pressed,
    changed,
};

// SDL's mouse motion and wheel events use no stable enum of their own.  The
// adapter gives them an explicit, small domain so callers do not pass SDL
// event types through the Runtime contract.
enum class MouseAxis : std::uint8_t {
    x,
    y,
    wheel_x,
    wheel_y,
};

struct NormalizedInputSource final {
    InputDeviceKind device{InputDeviceKind::keyboard};
    InputControlKind control{InputControlKind::button};
    std::string id;
    std::uint32_t physical_code{};
};

struct InputSample final {
    InputPhysicalKind physical_kind{InputPhysicalKind::keyboard_scancode};
    std::uint32_t physical_code{};
    float value{};
    InputEventPhase phase{InputEventPhase::changed};
    // A zero sequence asks the adapter to assign the next local sequence.
    // Non-zero values must be strictly increasing, which makes replay and
    // semantic observations deterministic and rejects stale events.
    std::uint64_t sequence{};
    // SDL joystick instance ID when this is a gamepad event.  It is observed
    // as device metadata only; it is never included in source IDs.
    std::uint64_t device_instance{};
    bool repeat{};
};

struct InputDeviceDescriptor final {
    InputDeviceKind kind{InputDeviceKind::keyboard};
    std::uint64_t instance_id{};
    std::string stable_id;
    std::string name;
    bool connected{true};
};

struct InputCaptureOptions final {
    bool keyboard{true};
    bool mouse{true};
    bool gamepad{true};
    bool axes{true};
    bool mouse_motion{};
    bool repeats{};
    float axis_threshold{0.5F};
};

enum class InputCaptureState : std::uint8_t {
    idle,
    armed,
    captured,
    cancelled,
};

struct CapturedInput final {
    NormalizedInputSource source;
    std::uint64_t sequence{};
    std::uint64_t device_instance{};
    float value{};
};

// Converts the SDL physical values used by the event loop into the source
// IDs consumed by InputActionRuntime.  Unknown/reserved values fail closed.
[[nodiscard]] std::optional<NormalizedInputSource> normalize_keyboard_scancode(std::uint32_t scancode);
[[nodiscard]] std::optional<NormalizedInputSource> normalize_mouse_button(std::uint32_t button);
[[nodiscard]] std::optional<NormalizedInputSource> normalize_mouse_axis(MouseAxis axis);
[[nodiscard]] std::optional<NormalizedInputSource> normalize_gamepad_button(std::uint32_t button);
[[nodiscard]] std::optional<NormalizedInputSource> normalize_gamepad_axis(std::uint32_t axis);
[[nodiscard]] std::optional<NormalizedInputSource> normalize_input(const InputSample& sample);

class InputSourceAdapter final {
public:
    static constexpr std::size_t max_devices = 16;
    static constexpr std::size_t max_sources = 128;
    static constexpr std::size_t max_device_text = 64;
    static constexpr std::size_t max_observation_bytes = 64U * 1024U;

    InputSourceAdapter() = default;

    [[nodiscard]] bool connect_device(InputDeviceDescriptor descriptor);
    [[nodiscard]] bool disconnect_device(InputDeviceKind kind, std::uint64_t instance_id);

    [[nodiscard]] bool begin_capture(std::uint64_t request_id, InputCaptureOptions options = {});
    [[nodiscard]] bool cancel_capture(std::uint64_t request_id);

    // Returns false for an unknown physical source, invalid phase/value,
    // stale sequence, or a source emitted by a known disconnected device.
    // A valid event that does not satisfy the current capture filter still
    // returns true and updates the source observation.
    [[nodiscard]] bool ingest(const InputSample& sample);

    [[nodiscard]] InputCaptureState capture_state() const noexcept { return capture_state_; }
    [[nodiscard]] std::uint64_t capture_request_id() const noexcept { return capture_request_id_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
    [[nodiscard]] const std::optional<CapturedInput>& captured_input() const noexcept { return captured_; }

    // The result is valid JSON, has stable key/order-independent contents,
    // and is capped at max_observation_bytes (a caller budget below 256 is
    // raised to that minimum so the status envelope remains useful).
    [[nodiscard]] std::string observe_json(std::size_t byte_budget = 16U * 1024U) const;

private:
    struct DeviceState final {
        InputDeviceDescriptor descriptor;
    };

    struct SourceState final {
        NormalizedInputSource source;
        std::uint64_t device_instance{};
        std::uint64_t sequence{};
        float value{};
        bool active{};
    };

    [[nodiscard]] bool capture_accepts(const InputSample& sample,
                                       const NormalizedInputSource& source) const;
    [[nodiscard]] bool device_is_disconnected(InputDeviceKind kind, std::uint64_t instance_id) const;

    std::vector<DeviceState> devices_;
    std::vector<SourceState> sources_;
    InputCaptureState capture_state_{InputCaptureState::idle};
    std::uint64_t capture_request_id_{};
    InputCaptureOptions capture_options_{};
    std::optional<CapturedInput> captured_;
    std::uint64_t revision_{1};
    std::uint64_t last_sequence_{};
};

} // namespace noemancer
