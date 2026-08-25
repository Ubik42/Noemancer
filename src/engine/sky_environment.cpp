#include "engine/sky_environment.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double degrees_to_radians = pi / 180.0;
constexpr double solar_declination_amplitude_degrees = 23.44;
constexpr std::uint16_t first_day_of_year = 1U;
constexpr std::uint16_t last_day_of_year = 366U;
constexpr float min_north_offset_degrees = -360.0F;
constexpr float max_north_offset_degrees = 360.0F;
constexpr float max_aerosol_density = 1.0F;
constexpr float max_aerosol_absorption = 1.0F;
constexpr float max_sun_irradiance_scale = 4.0F;

constexpr std::uint64_t fnv1a_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv1a_prime = 1099511628211ULL;

void add_error(std::vector<SkyEnvironmentDiagnostic>& diagnostics,
               std::string code, std::string path, std::string message) {
    diagnostics.push_back({std::move(code), std::move(path), std::move(message)});
}

bool has_non_whitespace(const std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte > 0x20U;
    });
}

bool finite_in_range(const float value, const float minimum, const float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool finite_in_half_open_range(const float value, const float minimum, const float maximum) {
    return std::isfinite(value) && value >= minimum && value < maximum;
}

void check_fields(const Json& value, const std::initializer_list<std::string_view> allowed,
                  const std::string_view path,
                  std::vector<SkyEnvironmentDiagnostic>& diagnostics) {
    for (const auto& item : value.items()) {
        const auto known = std::find(allowed.begin(), allowed.end(), item.key());
        if (known == allowed.end()) {
            const auto item_path = path.empty() ? "/" + item.key()
                                                : std::string(path) + "/" + item.key();
            add_error(diagnostics, "sky-environment.unknown-field", item_path,
                      "Unknown field; use the published sky-environment/0.1 schema.");
        }
    }
}

std::string path_for(const std::string_view parent, const std::string_view key) {
    return parent.empty() ? "/" + std::string(key)
                          : std::string(parent) + "/" + std::string(key);
}

bool parse_string_field(const Json& value, const char* name, const std::string_view path,
                        std::string& destination,
                        std::vector<SkyEnvironmentDiagnostic>& diagnostics,
                        const bool required = false) {
    if (!value.contains(name)) {
        if (required) {
            add_error(diagnostics, "sky-environment.missing-field", path_for(path, name),
                      "Required field is missing.");
        }
        return false;
    }
    const auto& field = value.at(name);
    if (!field.is_string()) {
        add_error(diagnostics, "sky-environment.invalid-type", path_for(path, name),
                  "Expected a string.");
        return false;
    }
    destination = field.get<std::string>();
    return true;
}

bool parse_bool_field(const Json& value, const char* name, const std::string_view path,
                      bool& destination,
                      std::vector<SkyEnvironmentDiagnostic>& diagnostics) {
    if (!value.contains(name)) return false;
    const auto& field = value.at(name);
    if (!field.is_boolean()) {
        add_error(diagnostics, "sky-environment.invalid-type", path_for(path, name),
                  "Expected a boolean.");
        return false;
    }
    destination = field.get<bool>();
    return true;
}

bool parse_float_field(const Json& value, const char* name, const std::string_view path,
                       float& destination,
                       std::vector<SkyEnvironmentDiagnostic>& diagnostics) {
    if (!value.contains(name)) return false;
    const auto& field = value.at(name);
    if (!field.is_number()) {
        add_error(diagnostics, "sky-environment.invalid-type", path_for(path, name),
                  "Expected a finite number.");
        return false;
    }
    const auto parsed = field.get<double>();
    if (!std::isfinite(parsed) || parsed < -static_cast<double>(std::numeric_limits<float>::max()) ||
        parsed > static_cast<double>(std::numeric_limits<float>::max())) {
        add_error(diagnostics, "sky-environment.invalid-number", path_for(path, name),
                  "Expected a finite number representable by a 32-bit float.");
        return false;
    }
    destination = static_cast<float>(parsed);
    return true;
}

bool parse_day_field(const Json& value, const char* name, const std::string_view path,
                     std::uint16_t& destination,
                     std::vector<SkyEnvironmentDiagnostic>& diagnostics) {
    if (!value.contains(name)) return false;
    const auto& field = value.at(name);
    if (!field.is_number_integer()) {
        add_error(diagnostics, "sky-environment.invalid-type", path_for(path, name),
                  "Expected an integer day in [1,366].");
        return false;
    }
    const auto parsed = field.get<std::int64_t>();
    if (parsed < first_day_of_year || parsed > last_day_of_year) {
        add_error(diagnostics, "sky-environment.day-of-year-range", path_for(path, name),
                  "dayOfYear must be an integer in [1,366].");
        return false;
    }
    destination = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_object(const Json& value, const char* name, const std::string_view path,
                  const Json*& destination,
                  std::vector<SkyEnvironmentDiagnostic>& diagnostics) {
    if (!value.contains(name)) {
        add_error(diagnostics, "sky-environment.missing-field", path_for(path, name),
                  "Required object is missing.");
        return false;
    }
    const auto& field = value.at(name);
    if (!field.is_object()) {
        add_error(diagnostics, "sky-environment.invalid-type", path_for(path, name),
                  "Expected an object.");
        return false;
    }
    destination = &field;
    return true;
}

void append_json_string(std::string& output, const std::string_view value) {
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(hex[(byte >> 4U) & 0x0FU]);
                output.push_back(hex[byte & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_number(std::string& output, const float value) {
    if (!std::isfinite(value)) {
        output += "null";
        return;
    }
    if (value == 0.0F) {
        output.push_back('0');
        return;
    }
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), static_cast<double>(value),
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (converted.ec == std::errc{}) {
        output.append(buffer.data(), converted.ptr);
    } else {
        output += "0";
    }
}

void append_integer(std::string& output, const std::uint16_t value) {
    output += std::to_string(value);
}

void append_key(std::string& output, const std::string_view key, bool& first) {
    if (!first) output.push_back(',');
    append_json_string(output, key);
    output.push_back(':');
    first = false;
}

void append_bool(std::string& output, const bool value) {
    output += value ? "true" : "false";
}

void append_direction(std::string& output, const std::array<float, 3>& direction) {
    output.push_back('[');
    append_number(output, direction[0]);
    output.push_back(',');
    append_number(output, direction[1]);
    output.push_back(',');
    append_number(output, direction[2]);
    output.push_back(']');
}

struct AerosolProjection final {
    float scattering_scale{};
    float absorption_scale{};
    float phase_g{};
};

AerosolProjection aerosol_projection(const SkyAtmosphereWeather& weather) noexcept {
    // Presets provide a stable semantic baseline while the normalized authored
    // controls remain the final knobs.  This is intentionally an adapter over
    // existing Mie coefficients, not a second atmosphere/planet model.
    float preset_scattering = 0.75F;
    float preset_absorption = 0.35F;
    float preset_phase = 0.72F;
    switch (weather.aerosol_preset) {
    case SkyAerosolPreset::clear:
        break;
    case SkyAerosolPreset::hazy:
        preset_scattering = 1.25F;
        preset_absorption = 0.75F;
        preset_phase = 0.80F;
        break;
    case SkyAerosolPreset::dusty:
        preset_scattering = 1.75F;
        preset_absorption = 1.35F;
        preset_phase = 0.64F;
        break;
    }
    return {
        preset_scattering * (0.50F + weather.aerosol_density),
        preset_absorption * (0.25F + weather.aerosol_absorption),
        std::clamp(preset_phase + (weather.aerosol_density - 0.5F) * 0.12F -
                       weather.aerosol_absorption * 0.04F,
                   -0.99F, 0.99F),
    };
}

std::string canonical_source(const SkyEnvironmentSettings& settings,
                             const bool include_control_fields = true) {
    std::string output;
    output.reserve(640U);
    output.push_back('{');
    bool first = true;
    append_key(output, "schema", first);
    append_json_string(output, settings.schema);
    append_key(output, "profileId", first);
    append_json_string(output, settings.profile_id);
    append_key(output, "solar", first);
    output.push_back('{');
    bool solar_first = true;
    append_key(output, "timeOfDayHours", solar_first);
    append_number(output, settings.solar.time_of_day_hours);
    append_key(output, "dayOfYear", solar_first);
    append_integer(output, settings.solar.day_of_year);
    append_key(output, "latitudeDegrees", solar_first);
    append_number(output, settings.solar.latitude_degrees);
    append_key(output, "northOffsetDegrees", solar_first);
    append_number(output, settings.solar.north_offset_degrees);
    if (include_control_fields) {
        append_key(output, "running", solar_first);
        append_bool(output, settings.solar.running);
        append_key(output, "timeScale", solar_first);
        append_number(output, settings.solar.time_scale);
    }
    output.push_back('}');
    append_key(output, "weather", first);
    output.push_back('{');
    bool weather_first = true;
    append_key(output, "aerosolPreset", weather_first);
    append_json_string(output, sky_aerosol_preset_name(settings.weather.aerosol_preset));
    append_key(output, "aerosolDensity", weather_first);
    append_number(output, settings.weather.aerosol_density);
    append_key(output, "aerosolAbsorption", weather_first);
    append_number(output, settings.weather.aerosol_absorption);
    append_key(output, "sunIrradianceScale", weather_first);
    append_number(output, settings.weather.sun_irradiance_scale);
    output.push_back('}');
    output.push_back('}');
    return output;
}

std::string history_source(const SkyEnvironmentSettings& settings) {
    return canonical_source(settings, false);
}

std::string fnv1a_hex(const std::string_view value) {
    std::uint64_t hash = fnv1a_offset_basis;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= fnv1a_prime;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string output(16U, '0');
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - 1U - index) * 4U);
        output[index] = hex[(hash >> shift) & 0x0FU];
    }
    return output;
}

} // namespace

std::string_view sky_aerosol_preset_name(const SkyAerosolPreset preset) noexcept {
    switch (preset) {
    case SkyAerosolPreset::clear: return "clear";
    case SkyAerosolPreset::hazy: return "hazy";
    case SkyAerosolPreset::dusty: return "dusty";
    }
    return {};
}

bool sky_aerosol_preset_valid(const SkyAerosolPreset preset) noexcept {
    return preset >= SkyAerosolPreset::clear && preset <= SkyAerosolPreset::dusty;
}

SkyAerosolPreset sky_aerosol_preset_from_string(const std::string_view value) noexcept {
    if (value == "clear") return SkyAerosolPreset::clear;
    if (value == "hazy") return SkyAerosolPreset::hazy;
    if (value == "dusty") return SkyAerosolPreset::dusty;
    return SkyAerosolPreset::clear;
}

SkyEnvironmentSettings make_sky_environment_settings(const SkyAerosolPreset preset) {
    SkyEnvironmentSettings settings;
    settings.weather.aerosol_preset = preset;
    switch (preset) {
    case SkyAerosolPreset::clear:
        settings.weather.aerosol_density = 0.20F;
        settings.weather.aerosol_absorption = 0.05F;
        settings.weather.sun_irradiance_scale = 1.0F;
        break;
    case SkyAerosolPreset::hazy:
        settings.weather.aerosol_density = 0.55F;
        settings.weather.aerosol_absorption = 0.20F;
        settings.weather.sun_irradiance_scale = 0.92F;
        break;
    case SkyAerosolPreset::dusty:
        settings.weather.aerosol_density = 0.85F;
        settings.weather.aerosol_absorption = 0.45F;
        settings.weather.sun_irradiance_scale = 0.78F;
        break;
    }
    return settings;
}

std::vector<SkyEnvironmentDiagnostic> validate_sky_environment(
    const SkyEnvironmentSettings& settings) {
    std::vector<SkyEnvironmentDiagnostic> diagnostics;
    if (settings.schema != sky_environment_schema) {
        add_error(diagnostics, "sky-environment.unsupported-schema", "/schema",
                  "Expected noemancer.sky-environment/0.1; migrate the document first.");
    }
    if (!has_non_whitespace(settings.profile_id)) {
        add_error(diagnostics, "sky-environment.empty-profile-id", "/profileId",
                  "profileId must contain a non-whitespace stable identity.");
    } else if (settings.profile_id.size() > 128U) {
        add_error(diagnostics, "sky-environment.profile-id-range", "/profileId",
                  "profileId must be at most 128 UTF-8 bytes.");
    }
    if (!finite_in_half_open_range(settings.solar.time_of_day_hours, 0.0F, 24.0F)) {
        add_error(diagnostics, "sky-environment.time-of-day-range", "/solar/timeOfDayHours",
                  "timeOfDayHours must be finite in [0,24).");
    }
    if (settings.solar.day_of_year < first_day_of_year ||
        settings.solar.day_of_year > last_day_of_year) {
        add_error(diagnostics, "sky-environment.day-of-year-range", "/solar/dayOfYear",
                  "dayOfYear must be in [1,366].");
    }
    if (!finite_in_range(settings.solar.latitude_degrees, -90.0F, 90.0F)) {
        add_error(diagnostics, "sky-environment.latitude-range", "/solar/latitudeDegrees",
                  "latitudeDegrees must be finite in [-90,90].");
    }
    if (!finite_in_range(settings.solar.north_offset_degrees,
                         min_north_offset_degrees, max_north_offset_degrees)) {
        add_error(diagnostics, "sky-environment.north-offset-range",
                  "/solar/northOffsetDegrees",
                  "northOffsetDegrees must be finite in [-360,360].");
    }
    if (!finite_in_range(settings.solar.time_scale, 0.0F, sky_environment_max_time_scale)) {
        add_error(diagnostics, "sky-environment.time-scale-range", "/solar/timeScale",
                  "timeScale must be finite in [0,1000].");
    }
    if (!sky_aerosol_preset_valid(settings.weather.aerosol_preset)) {
        add_error(diagnostics, "sky-environment.invalid-aerosol-preset",
                  "/weather/aerosolPreset",
                  "aerosolPreset must be clear, hazy or dusty.");
    }
    if (!finite_in_range(settings.weather.aerosol_density, 0.0F, max_aerosol_density)) {
        add_error(diagnostics, "sky-environment.aerosol-density-range",
                  "/weather/aerosolDensity",
                  "aerosolDensity must be finite in [0,1].");
    }
    if (!finite_in_range(settings.weather.aerosol_absorption, 0.0F, max_aerosol_absorption)) {
        add_error(diagnostics, "sky-environment.aerosol-absorption-range",
                  "/weather/aerosolAbsorption",
                  "aerosolAbsorption must be finite in [0,1].");
    }
    if (!finite_in_range(settings.weather.sun_irradiance_scale,
                         0.0F, max_sun_irradiance_scale)) {
        add_error(diagnostics, "sky-environment.sun-irradiance-scale-range",
                  "/weather/sunIrradianceScale",
                  "sunIrradianceScale must be finite in [0,4].");
    }
    return diagnostics;
}

SkyEnvironmentParseResult SkyEnvironmentCodec::parse_json(const std::string_view source) {
    SkyEnvironmentParseResult result;
    if (source.size() > sky_environment_max_source_bytes) {
        add_error(result.errors, "sky-environment.source-too-large", "/",
                  "Sky environment source exceeds the 64 KiB authoring budget.");
        return result;
    }
    const auto input = Json::parse(source, nullptr, false);
    if (input.is_discarded() || !input.is_object()) {
        add_error(result.errors, "sky-environment.invalid-json", "/",
                  "Sky environment settings must be a JSON object.");
        return result;
    }
    check_fields(input, {"schema", "profileId", "solar", "weather"}, "", result.errors);

    SkyEnvironmentSettings settings;
    parse_string_field(input, "schema", "", settings.schema, result.errors, true);
    parse_string_field(input, "profileId", "", settings.profile_id, result.errors);
    const Json* solar = nullptr;
    const Json* weather = nullptr;
    const bool has_solar = parse_object(input, "solar", "", solar, result.errors);
    const bool has_weather = parse_object(input, "weather", "", weather, result.errors);
    if (has_solar) {
        check_fields(*solar, {"timeOfDayHours", "dayOfYear", "latitudeDegrees",
                              "northOffsetDegrees", "running", "timeScale"},
                     "/solar", result.errors);
        parse_float_field(*solar, "timeOfDayHours", "/solar",
                          settings.solar.time_of_day_hours, result.errors);
        parse_day_field(*solar, "dayOfYear", "/solar",
                        settings.solar.day_of_year, result.errors);
        parse_float_field(*solar, "latitudeDegrees", "/solar",
                          settings.solar.latitude_degrees, result.errors);
        parse_float_field(*solar, "northOffsetDegrees", "/solar",
                          settings.solar.north_offset_degrees, result.errors);
        parse_bool_field(*solar, "running", "/solar",
                         settings.solar.running, result.errors);
        parse_float_field(*solar, "timeScale", "/solar",
                          settings.solar.time_scale, result.errors);
    }
    if (has_weather) {
        check_fields(*weather, {"aerosolPreset", "aerosolDensity", "aerosolAbsorption",
                                "sunIrradianceScale"}, "/weather", result.errors);
        if (weather->contains("aerosolPreset")) {
            const auto& field = weather->at("aerosolPreset");
            if (!field.is_string()) {
                add_error(result.errors, "sky-environment.invalid-type",
                          "/weather/aerosolPreset", "Expected clear, hazy or dusty.");
            } else {
                const auto value = field.get<std::string>();
                const auto preset = sky_aerosol_preset_from_string(value);
                if (!sky_aerosol_preset_valid(preset) ||
                    sky_aerosol_preset_name(preset) != value) {
                    add_error(result.errors, "sky-environment.invalid-aerosol-preset",
                              "/weather/aerosolPreset",
                              "aerosolPreset must be clear, hazy or dusty.");
                } else {
                    settings.weather.aerosol_preset = preset;
                }
            }
        }
        parse_float_field(*weather, "aerosolDensity", "/weather",
                          settings.weather.aerosol_density, result.errors);
        parse_float_field(*weather, "aerosolAbsorption", "/weather",
                          settings.weather.aerosol_absorption, result.errors);
        parse_float_field(*weather, "sunIrradianceScale", "/weather",
                          settings.weather.sun_irradiance_scale, result.errors);
    }

    const auto semantic_errors = validate_sky_environment(settings);
    result.errors.insert(result.errors.end(), semantic_errors.begin(), semantic_errors.end());
    if (result.errors.empty()) result.document = std::move(settings);
    return result;
}

std::vector<SkyEnvironmentDiagnostic> SkyEnvironmentCodec::validate(
    const SkyEnvironmentSettings& settings) {
    return validate_sky_environment(settings);
}

std::string SkyEnvironmentCodec::write_canonical_json(
    const SkyEnvironmentSettings& settings) {
    return sky_environment_canonical_json(settings);
}

std::string SkyEnvironmentCodec::fingerprint(const SkyEnvironmentSettings& settings) {
    return sky_environment_fingerprint(settings);
}

SkyEnvironmentAdvanceResult advance_sky_environment(
    SkyEnvironmentSettings& settings, const float delta_seconds) {
    SkyEnvironmentAdvanceResult result;
    result.code = "sky-environment.no-change";
    result.detail = "The explicit delta produced no clock change.";

    result.diagnostics = validate_sky_environment(settings);
    if (!result.diagnostics.empty()) {
        result.success = false;
        result.code = "sky-environment.invalid-state";
        result.detail = "The environment must validate before it can advance.";
        return result;
    }
    if (!finite_in_range(delta_seconds, 0.0F, sky_environment_max_advance_delta_seconds)) {
        add_error(result.diagnostics, "sky-environment.delta-range", "/deltaSeconds",
                  "deltaSeconds must be finite in [0,604800].");
        result.success = false;
        result.code = "sky-environment.invalid-delta";
        result.detail = "Advance requires an explicit finite non-negative delta.";
        return result;
    }
    result.success = true;
    if (!settings.solar.running || settings.solar.time_scale == 0.0F || delta_seconds == 0.0F) {
        result.code = "sky-environment.paused";
        result.detail = "The solar clock is paused or has zero time scale.";
        return result;
    }

    const auto total_hours = static_cast<double>(settings.solar.time_of_day_hours) +
                             static_cast<double>(delta_seconds) *
                                 static_cast<double>(settings.solar.time_scale) / 3600.0;
    const auto elapsed_days = static_cast<std::int64_t>(std::floor(total_hours / 24.0));
    auto wrapped_hours = std::fmod(total_hours, 24.0);
    if (wrapped_hours < 0.0) wrapped_hours += 24.0;
    if (wrapped_hours >= 24.0) {
        wrapped_hours = std::nextafter(24.0, 0.0);
    }

    const auto old_day = static_cast<std::int64_t>(settings.solar.day_of_year) - 1LL;
    auto wrapped_day = (old_day + elapsed_days) % static_cast<std::int64_t>(last_day_of_year);
    if (wrapped_day < 0LL) wrapped_day += static_cast<std::int64_t>(last_day_of_year);
    const auto next_day = static_cast<std::uint16_t>(wrapped_day + 1LL);
    // A double remainder immediately below 24 can round to 24 when converted
    // to float; keep the stored half-open value canonical after the cast.
    const auto next_hours = std::min(
        static_cast<float>(wrapped_hours), std::nextafter(24.0F, 0.0F));
    result.changed = next_day != settings.solar.day_of_year ||
                     next_hours != settings.solar.time_of_day_hours;
    settings.solar.day_of_year = next_day;
    settings.solar.time_of_day_hours = next_hours;
    result.code = result.changed ? "sky-environment.advanced" : "sky-environment.no-change";
    result.detail = result.changed ? "Solar time advanced from the explicit delta."
                                   : "The explicit delta produced no representable clock change.";
    return result;
}

std::array<float, 3> sky_environment_solar_direction(
    const SkyEnvironmentSettings& settings) noexcept {
    const auto& solar = settings.solar;
    const auto safe_time = std::clamp(std::isfinite(solar.time_of_day_hours)
                                          ? static_cast<double>(solar.time_of_day_hours)
                                          : 12.0,
                                      0.0, std::nextafter(24.0, 0.0));
    const auto safe_day = std::clamp(static_cast<double>(solar.day_of_year), 1.0, 366.0);
    const auto safe_latitude = std::clamp(std::isfinite(solar.latitude_degrees)
                                              ? static_cast<double>(solar.latitude_degrees)
                                              : 0.0,
                                          -90.0, 90.0) * degrees_to_radians;
    const auto safe_north_offset = std::isfinite(solar.north_offset_degrees)
                                       ? static_cast<double>(solar.north_offset_degrees)
                                       : 0.0;

    // Day 80 is near the March equinox.  A sinusoid is intentionally used as
    // a deterministic low-cost approximation, not as an astronomical clock.
    const auto declination = solar_declination_amplitude_degrees *
                             std::sin(2.0 * pi * (safe_day - 80.0) / 366.0) *
                             degrees_to_radians;
    const auto hour_angle = (safe_time - 12.0) * 15.0 * degrees_to_radians;
    const auto cos_latitude = std::cos(safe_latitude);
    const auto sin_latitude = std::sin(safe_latitude);
    const auto cos_declination = std::cos(declination);
    const auto sin_declination = std::sin(declination);

    // Local components are east (+X), up (+Y), north (+Z).  The geographic
    // north offset is a positive +Y rotation, so +Z rotates toward +X.
    const auto local_east = cos_declination * std::sin(hour_angle);
    const auto local_up = sin_latitude * sin_declination +
                          cos_latitude * cos_declination * std::cos(hour_angle);
    const auto local_north = cos_latitude * sin_declination -
                             sin_latitude * cos_declination * std::cos(hour_angle);
    const auto offset = safe_north_offset * degrees_to_radians;
    const auto cos_offset = std::cos(offset);
    const auto sin_offset = std::sin(offset);
    const auto world_x = cos_offset * local_east + sin_offset * local_north;
    const auto world_z = -sin_offset * local_east + cos_offset * local_north;
    const auto length = std::sqrt(world_x * world_x + local_up * local_up + world_z * world_z);
    if (!std::isfinite(length) || length <= std::numeric_limits<double>::epsilon()) {
        return {0.0F, 1.0F, 0.0F};
    }
    return {static_cast<float>(world_x / length), static_cast<float>(local_up / length),
            static_cast<float>(world_z / length)};
}

SkyAtmosphereSettings project_sky_environment(
    const SkyEnvironmentSettings& environment,
    const SkyAtmosphereSettings& atmosphere) {
    if (!validate_sky_environment(environment).empty()) return atmosphere;

    auto result = atmosphere;
    result.sun_direction = sky_environment_solar_direction(environment);
    const auto aerosol = aerosol_projection(environment.weather);
    for (std::size_t index = 0; index < result.mie_scattering_per_m.size(); ++index) {
        result.mie_scattering_per_m[index] *= aerosol.scattering_scale;
        result.mie_absorption_per_m[index] *= aerosol.absorption_scale;
    }
    result.mie_phase_g = aerosol.phase_g;
    for (std::size_t index = 0; index < result.sun_irradiance.size(); ++index) {
        result.sun_irradiance[index] *= environment.weather.sun_irradiance_scale;
    }
    return result;
}

std::string sky_environment_canonical_json(const SkyEnvironmentSettings& settings) {
    return canonical_source(settings);
}

std::string sky_environment_canonical_evidence(const SkyEnvironmentSettings& settings) {
    const auto direction = sky_environment_solar_direction(settings);
    const auto aerosol = aerosol_projection(settings.weather);
    std::string output;
    output.reserve(960U);
    output.push_back('{');
    bool first = true;
    append_key(output, "schema", first);
    append_json_string(output, settings.schema);
    append_key(output, "source", first);
    output += canonical_source(settings);
    append_key(output, "derived", first);
    output.push_back('{');
    bool derived_first = true;
    append_key(output, "solarDirection", derived_first);
    append_direction(output, direction);
    append_key(output, "mieScatteringScale", derived_first);
    append_number(output, aerosol.scattering_scale);
    append_key(output, "mieAbsorptionScale", derived_first);
    append_number(output, aerosol.absorption_scale);
    append_key(output, "miePhaseG", derived_first);
    append_number(output, aerosol.phase_g);
    output.push_back('}');
    output.push_back('}');
    return output;
}

std::string sky_environment_fingerprint(const SkyEnvironmentSettings& settings) {
    return "fnv1a64:" + fnv1a_hex(sky_environment_canonical_json(settings));
}

std::string sky_environment_history_reset_identity(const SkyEnvironmentSettings& settings) {
    return "sky-environment-history/1:fnv1a64:" + fnv1a_hex(history_source(settings));
}

} // namespace noemancer
