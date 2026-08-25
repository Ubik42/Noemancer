#include "engine/sky_environment.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

int fail(const char* message) {
    std::cerr << "sky_environment_tests: " << message << '\n';
    return 1;
}

bool has_diagnostic(const std::vector<noemancer::SkyEnvironmentDiagnostic>& diagnostics,
                    const std::string_view code, const std::string_view path) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code && diagnostic.path == path && !diagnostic.message.empty()) {
            return true;
        }
    }
    return false;
}

bool close(const float left, const float right, const float epsilon = 1.0e-5F) {
    return std::abs(left - right) <= epsilon;
}

bool same_array(const std::array<float, 3>& left, const std::array<float, 3>& right) {
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!close(left[index], right[index])) return false;
    }
    return true;
}

} // namespace

int main() {
    using namespace noemancer;

    const auto clear = make_sky_environment_settings(SkyAerosolPreset::clear);
    const auto hazy = make_sky_environment_settings(SkyAerosolPreset::hazy);
    const auto dusty = make_sky_environment_settings(SkyAerosolPreset::dusty);
    if (!validate_sky_environment(clear).empty() || !validate_sky_environment(hazy).empty() ||
        !validate_sky_environment(dusty).empty() ||
        sky_aerosol_preset_name(SkyAerosolPreset::dusty) != "dusty" ||
        sky_aerosol_preset_from_string("hazy") != SkyAerosolPreset::hazy ||
        sky_aerosol_preset_valid(static_cast<SkyAerosolPreset>(99U))) {
        return fail("default environment or aerosol vocabulary is invalid");
    }

    // Every authored boundary is explicit and the upper time-of-day bound is
    // half-open, so midnight is 0 rather than a second representation of 24.
    auto invalid = clear;
    invalid.solar.time_of_day_hours = 24.0F;
    invalid.solar.day_of_year = 0U;
    invalid.solar.latitude_degrees = 90.1F;
    invalid.solar.north_offset_degrees = 360.1F;
    invalid.solar.time_scale = 1000.1F;
    invalid.weather.aerosol_density = 1.1F;
    invalid.weather.aerosol_absorption = -0.1F;
    invalid.weather.sun_irradiance_scale = 4.1F;
    const auto invalid_diagnostics = validate_sky_environment(invalid);
    if (!has_diagnostic(invalid_diagnostics, "sky-environment.time-of-day-range",
                        "/solar/timeOfDayHours") ||
        !has_diagnostic(invalid_diagnostics, "sky-environment.day-of-year-range",
                        "/solar/dayOfYear") ||
        !has_diagnostic(invalid_diagnostics, "sky-environment.latitude-range",
                        "/solar/latitudeDegrees") ||
        !has_diagnostic(invalid_diagnostics, "sky-environment.north-offset-range",
                        "/solar/northOffsetDegrees") ||
        !has_diagnostic(invalid_diagnostics, "sky-environment.time-scale-range",
                        "/solar/timeScale") ||
        !has_diagnostic(invalid_diagnostics, "sky-environment.aerosol-density-range",
                        "/weather/aerosolDensity") ||
        !has_diagnostic(invalid_diagnostics, "sky-environment.aerosol-absorption-range",
                        "/weather/aerosolAbsorption") ||
        !has_diagnostic(invalid_diagnostics, "sky-environment.sun-irradiance-scale-range",
                        "/weather/sunIrradianceScale")) {
        return fail("bounded fields did not return stable diagnostics");
    }

    // Progression is deterministic, explicit and wraps both midnight and the
    // fixed 366-day authoring cycle.  A paused clock never mutates.
    auto clock = clear;
    clock.solar.running = true;
    clock.solar.time_scale = 1.0F;
    clock.solar.time_of_day_hours = 23.5F;
    clock.solar.day_of_year = 365U;
    const auto first_advance = advance_sky_environment(clock, 3600.0F);
    if (!first_advance || !first_advance.changed || clock.solar.day_of_year != 366U ||
        !close(clock.solar.time_of_day_hours, 0.5F)) {
        return fail("midnight advance did not wrap deterministically");
    }
    const auto year_wrap = advance_sky_environment(clock, 86'400.0F);
    if (!year_wrap || clock.solar.day_of_year != 1U ||
        !close(clock.solar.time_of_day_hours, 0.5F)) {
        return fail("day 366 did not wrap to day 1");
    }
    const auto paused_before = clock;
    clock.solar.running = false;
    const auto paused = advance_sky_environment(clock, 3600.0F);
    if (!paused || paused.changed || clock.solar.day_of_year != paused_before.solar.day_of_year ||
        clock.solar.time_of_day_hours != paused_before.solar.time_of_day_hours) {
        return fail("paused clock advanced unexpectedly");
    }
    auto deterministic_a = clear;
    auto deterministic_b = clear;
    deterministic_a.solar.running = deterministic_b.solar.running = true;
    deterministic_a.solar.time_scale = deterministic_b.solar.time_scale = 42.0F;
    if (!advance_sky_environment(deterministic_a, 12'345.0F) ||
        !advance_sky_environment(deterministic_b, 12'345.0F) ||
        sky_environment_canonical_json(deterministic_a) !=
            sky_environment_canonical_json(deterministic_b)) {
        return fail("same clock input did not produce the same canonical state");
    }
    auto invalid_delta = clear;
    invalid_delta.solar.running = true;
    if (advance_sky_environment(invalid_delta, -1.0F) ||
        advance_sky_environment(invalid_delta,
                                 std::numeric_limits<float>::quiet_NaN()) ||
        !has_diagnostic(advance_sky_environment(invalid_delta, -1.0F).diagnostics,
                        "sky-environment.delta-range", "/deltaSeconds")) {
        return fail("invalid explicit delta was accepted");
    }

    // Solar direction is normalized and follows +X east, +Y up, +Z north.
    auto solar = clear;
    solar.solar.day_of_year = 80U;
    solar.solar.time_of_day_hours = 12.0F;
    solar.solar.latitude_degrees = 0.0F;
    const auto noon = sky_environment_solar_direction(solar);
    const auto noon_length = std::sqrt(noon[0] * noon[0] + noon[1] * noon[1] + noon[2] * noon[2]);
    if (!close(noon_length, 1.0F, 1.0e-4F) || noon[1] < 0.99F) {
        return fail("solar noon direction is not a normalized upward vector");
    }
    auto offset_solar = solar;
    offset_solar.solar.north_offset_degrees = 90.0F;
    const auto offset = sky_environment_solar_direction(offset_solar);
    if (!close(offset[0], noon[2], 1.0e-4F) || !close(offset[2], -noon[0], 1.0e-4F)) {
        return fail("north offset did not rotate around +Y");
    }
    auto midnight = solar;
    midnight.solar.time_of_day_hours = 0.0F;
    const auto midnight_direction = sky_environment_solar_direction(midnight);
    const auto midnight_length = std::sqrt(
        midnight_direction[0] * midnight_direction[0] +
        midnight_direction[1] * midnight_direction[1] +
        midnight_direction[2] * midnight_direction[2]);
    if (!close(midnight_length, 1.0F, 1.0e-4F)) {
        return fail("midnight direction is not normalized");
    }

    // Projection changes only the solar/Mie/aerosol-facing fields.  Planet,
    // Rayleigh, ozone, ground, quality and profile remain with the base
    // atmosphere authority.
    auto atmosphere = make_sky_atmosphere_settings(SkyAtmosphereQuality::high);
    atmosphere.profile_id = "renderer-owned-profile";
    atmosphere.planet_radius_m = 7'000'000.0F;
    atmosphere.rayleigh_scattering_per_m = {0.1e-6F, 0.2e-6F, 0.3e-6F};
    atmosphere.ozone_absorption_per_m = {0.4e-6F, 0.5e-6F, 0.6e-6F};
    atmosphere.ground_albedo = {0.2F, 0.3F, 0.4F};
    atmosphere.mie_scale_height_m = 1'500.0F;
    const auto projected = project_sky_environment(dusty, atmosphere);
    if (projected.profile_id != atmosphere.profile_id ||
        projected.planet_radius_m != atmosphere.planet_radius_m ||
        projected.rayleigh_scattering_per_m != atmosphere.rayleigh_scattering_per_m ||
        projected.ozone_absorption_per_m != atmosphere.ozone_absorption_per_m ||
        projected.ground_albedo != atmosphere.ground_albedo ||
        projected.mie_scale_height_m != atmosphere.mie_scale_height_m ||
        same_array(projected.sun_direction, atmosphere.sun_direction) ||
        same_array(projected.sun_irradiance, atmosphere.sun_irradiance) ||
        projected.mie_scattering_per_m == atmosphere.mie_scattering_per_m ||
        projected.mie_absorption_per_m == atmosphere.mie_absorption_per_m ||
        close(projected.mie_phase_g, atmosphere.mie_phase_g)) {
        return fail("projection changed unrelated fields or did not apply solar/Mie fields");
    }
    auto invalid_projection = dusty;
    invalid_projection.solar.latitude_degrees = 100.0F;
    if (project_sky_environment(invalid_projection, atmosphere).sun_direction !=
        atmosphere.sun_direction) {
        return fail("invalid environment was projected into atmosphere state");
    }

    // Canonical codec is strict about schema, nested types and unknown fields,
    // and round-trips its normalized bytes.
    const auto canonical = SkyEnvironmentCodec::write_canonical_json(dusty);
    const auto parsed = SkyEnvironmentCodec::parse_json(canonical);
    if (!parsed || !parsed.document ||
        SkyEnvironmentCodec::write_canonical_json(*parsed.document) != canonical ||
        parsed.document->weather.aerosol_preset != SkyAerosolPreset::dusty ||
        parsed.document->solar.day_of_year != dusty.solar.day_of_year) {
        return fail("canonical environment JSON did not round-trip");
    }
    const auto unknown = SkyEnvironmentCodec::parse_json(
        canonical.substr(0U, canonical.size() - 1U) + R"(,"extra":true})");
    if (unknown || !has_diagnostic(unknown.errors, "sky-environment.unknown-field", "/extra")) {
        return fail("unknown environment field was accepted");
    }
    const auto invalid_json = SkyEnvironmentCodec::parse_json(
        R"({"schema":"noemancer.sky-environment/0.1","solar":{},"weather":{"aerosolPreset":"rain"}})");
    if (invalid_json || !has_diagnostic(invalid_json.errors,
                                        "sky-environment.invalid-aerosol-preset",
                                        "/weather/aerosolPreset")) {
        return fail("invalid aerosol preset was accepted");
    }

    const auto first_evidence = sky_environment_canonical_evidence(clear);
    const auto second_evidence = sky_environment_canonical_evidence(clear);
    const auto first_fingerprint = sky_environment_fingerprint(clear);
    const auto second_fingerprint = sky_environment_fingerprint(clear);
    const auto first_history = sky_environment_history_reset_identity(clear);
    const auto second_history = sky_environment_history_reset_identity(clear);
    if (first_evidence != second_evidence || first_fingerprint != second_fingerprint ||
        first_history != second_history || first_fingerprint.rfind("fnv1a64:", 0U) != 0U ||
        first_history.rfind("sky-environment-history/1:fnv1a64:", 0U) != 0U ||
        first_evidence.find("solarDirection") == std::string::npos ||
        first_evidence.find("mieScatteringScale") == std::string::npos) {
        return fail("environment evidence or identities were not deterministic");
    }
    auto control_changed = clear;
    control_changed.solar.running = true;
    control_changed.solar.time_scale = 10.0F;
    if (sky_environment_fingerprint(control_changed) == first_fingerprint ||
        sky_environment_history_reset_identity(control_changed) != first_history) {
        return fail("future-only clock controls incorrectly reset sky history");
    }
    auto appearance_changed = clear;
    appearance_changed.solar.time_of_day_hours = 13.0F;
    if (sky_environment_history_reset_identity(appearance_changed) == first_history) {
        return fail("appearance-changing time did not reset sky history");
    }
    return 0;
}
