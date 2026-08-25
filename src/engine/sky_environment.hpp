#pragma once

#include "engine/sky_atmosphere.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// The Environment Authority owns the authored clock and aerosol state.  It
// deliberately does not own a second set of planetary/scattering constants:
// project/render code projects this value into the existing
// SkyAtmosphereSettings contract.
inline constexpr std::string_view sky_environment_schema =
    "noemancer.sky-environment/0.1";
inline constexpr std::size_t sky_environment_max_source_bytes = 64U * 1024U;
inline constexpr float sky_environment_max_time_scale = 1000.0F;
inline constexpr float sky_environment_max_advance_delta_seconds = 604'800.0F;

enum class SkyAerosolPreset : std::uint8_t {
    clear,
    hazy,
    dusty,
    Clear = clear,
    Hazy = hazy,
    Dusty = dusty,
};

[[nodiscard]] std::string_view sky_aerosol_preset_name(
    SkyAerosolPreset preset) noexcept;
[[nodiscard]] bool sky_aerosol_preset_valid(
    SkyAerosolPreset preset) noexcept;
[[nodiscard]] SkyAerosolPreset sky_aerosol_preset_from_string(
    std::string_view value) noexcept;

// Solar-clock coordinates are deliberately explicit and deterministic:
// timeOfDayHours is local solar time (12:00 is the solar meridian),
// dayOfYear uses a fixed 366-day authoring cycle, latitude is geographic, and
// northOffsetDegrees rotates geographic north around +Y toward +X.  World
// space is the engine's right-handed +Y-up convention: +X east, +Y up, +Z
// north.  The resolved sun direction points from the planet toward the sun.
struct SkySolarClock final {
    float time_of_day_hours{12.0F};
    std::uint16_t day_of_year{172U};
    float latitude_degrees{0.0F};
    float north_offset_degrees{0.0F};
    bool running{false};
    float time_scale{1.0F};
};

// Aerosol values are normalized authoring controls, not cloud or precipitation
// simulation.  They affect only the existing Mie terms when projected into a
// SkyAtmosphereSettings value.
struct SkyAtmosphereWeather final {
    SkyAerosolPreset aerosol_preset{SkyAerosolPreset::clear};
    float aerosol_density{0.20F};
    float aerosol_absorption{0.05F};
    float sun_irradiance_scale{1.0F};
};

struct SkyEnvironmentSettings final {
    std::string schema{std::string(sky_environment_schema)};
    std::string profile_id{"default"};
    SkySolarClock solar{};
    SkyAtmosphereWeather weather{};
};

struct SkyEnvironmentDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct SkyEnvironmentParseResult final {
    std::optional<SkyEnvironmentSettings> document;
    std::vector<SkyEnvironmentDiagnostic> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return document.has_value() && errors.empty();
    }
};

struct SkyEnvironmentAdvanceResult final {
    bool success{};
    bool changed{};
    std::string code;
    std::string detail;
    std::vector<SkyEnvironmentDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

// This codec is intentionally JSON-facing only at the .cpp boundary.  The
// public types remain engine-owned plain data, so Editor/CLI/Agent adapters do
// not need to expose nlohmann::json or any other third-party type.
class SkyEnvironmentCodec final {
public:
    [[nodiscard]] static SkyEnvironmentParseResult parse_json(std::string_view source);
    [[nodiscard]] static std::vector<SkyEnvironmentDiagnostic> validate(
        const SkyEnvironmentSettings& settings);
    [[nodiscard]] static std::string write_canonical_json(
        const SkyEnvironmentSettings& settings);
    [[nodiscard]] static std::string fingerprint(
        const SkyEnvironmentSettings& settings);
};

[[nodiscard]] SkyEnvironmentSettings make_sky_environment_settings(
    SkyAerosolPreset preset = SkyAerosolPreset::clear);

[[nodiscard]] std::vector<SkyEnvironmentDiagnostic> validate_sky_environment(
    const SkyEnvironmentSettings& settings);

// Advances only when solar.running is true.  It never samples a system clock;
// delta_seconds is the complete and sole progression input.  A delta is
// bounded to one week to keep an accidental editor/Agent command from
// creating an unreviewable simulation jump.
[[nodiscard]] SkyEnvironmentAdvanceResult advance_sky_environment(
    SkyEnvironmentSettings& settings, float delta_seconds);

// Simplified solar position: declination is a sinusoid over the fixed 366-day
// cycle and the hour angle is 15 degrees per hour from local solar noon.  The
// result is normalized and follows the coordinate convention above.
[[nodiscard]] std::array<float, 3> sky_environment_solar_direction(
    const SkyEnvironmentSettings& settings) noexcept;

// Copies the existing atmosphere and changes only its solar direction,
// irradiance and Mie/aerosol terms.  Rayleigh, ozone, planet, ground, quality,
// and profile fields remain owned by the supplied SkyAtmosphereSettings.
[[nodiscard]] SkyAtmosphereSettings project_sky_environment(
    const SkyEnvironmentSettings& environment,
    const SkyAtmosphereSettings& atmosphere);

// Canonical authored JSON excludes derived evidence.  Evidence adds the
// resolved direction and effective aerosol scales while remaining
// deterministic plain JSON.  Fingerprint includes the complete authored
// state; history identity includes only values that change current sky
// appearance (running/timeScale are future-control state and are excluded).
[[nodiscard]] std::string sky_environment_canonical_json(
    const SkyEnvironmentSettings& settings);
[[nodiscard]] std::string sky_environment_canonical_evidence(
    const SkyEnvironmentSettings& settings);
[[nodiscard]] std::string sky_environment_fingerprint(
    const SkyEnvironmentSettings& settings);
[[nodiscard]] std::string sky_environment_history_reset_identity(
    const SkyEnvironmentSettings& settings);

} // namespace noemancer
