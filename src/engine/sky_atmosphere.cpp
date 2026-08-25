#include "engine/sky_atmosphere.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace noemancer {
namespace {

constexpr std::uint64_t fnv1a_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv1a_prime = 1099511628211ULL;
constexpr float max_coefficient_per_m = 0.1F;
constexpr float min_planet_radius_m = 1'000.0F;
constexpr float max_planet_radius_m = 1'000'000'000.0F;
constexpr float min_atmosphere_height_m = 100.0F;
constexpr float max_atmosphere_height_m = 1'000'000.0F;
constexpr float max_sun_irradiance = 1'000.0F;

constexpr std::uint64_t rgba16f_lut_bytes(
    const std::uint32_t transmittance_width, const std::uint32_t transmittance_height,
    const std::uint32_t multi_scattering_width, const std::uint32_t multi_scattering_height,
    const std::uint32_t sky_view_width, const std::uint32_t sky_view_height,
    const std::uint32_t camera_volume_width, const std::uint32_t camera_volume_height,
    const std::uint32_t camera_volume_slices) noexcept {
    const auto transmittance = static_cast<std::uint64_t>(transmittance_width) * transmittance_height;
    const auto multi_scattering = static_cast<std::uint64_t>(multi_scattering_width) * multi_scattering_height;
    const auto sky_view = static_cast<std::uint64_t>(sky_view_width) * sky_view_height;
    const auto camera_volume = static_cast<std::uint64_t>(camera_volume_width) * camera_volume_height * camera_volume_slices;
    return (transmittance + multi_scattering + sky_view + camera_volume) * 8ULL;
}

constexpr SkyAtmosphereLutBudget off_budget{
    false, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U};
constexpr SkyAtmosphereLutBudget low_budget{
    true, 128U, 32U, 16U, 16U, 96U, 54U, 16U, 16U, 16U,
    16U, 8U, 16U, 8U, rgba16f_lut_bytes(128U, 32U, 16U, 16U, 96U, 54U, 16U, 16U, 16U)};
constexpr SkyAtmosphereLutBudget medium_budget{
    true, 256U, 64U, 32U, 32U, 128U, 72U, 24U, 24U, 24U,
    32U, 16U, 32U, 16U, rgba16f_lut_bytes(256U, 64U, 32U, 32U, 128U, 72U, 24U, 24U, 24U)};
constexpr SkyAtmosphereLutBudget high_budget{
    true, 256U, 64U, 32U, 32U, 192U, 108U, 32U, 32U, 32U,
    64U, 32U, 48U, 24U, rgba16f_lut_bytes(256U, 64U, 32U, 32U, 192U, 108U, 32U, 32U, 32U)};
constexpr SkyAtmosphereLutBudget ultra_budget{
    true, 512U, 128U, 64U, 64U, 256U, 144U, 48U, 48U, 48U,
    96U, 48U, 64U, 32U, rgba16f_lut_bytes(512U, 128U, 64U, 64U, 256U, 144U, 48U, 48U, 48U)};

void add_error(std::vector<SkyAtmosphereDiagnostic>& diagnostics,
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

bool valid_quality(const SkyAtmosphereQuality quality) {
    return quality >= SkyAtmosphereQuality::off && quality <= SkyAtmosphereQuality::ultra;
}

bool valid_debug_view(const SkyAtmosphereDebugView view) {
    return view >= SkyAtmosphereDebugView::final &&
           view <= SkyAtmosphereDebugView::sun_visibility;
}

void validate_rgb(std::vector<SkyAtmosphereDiagnostic>& diagnostics,
                  const std::array<float, 3>& values, const std::string_view path,
                  const float maximum, const std::string_view unit) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto component_path = std::string(path) + "/" + std::to_string(index);
        if (!finite_in_range(values[index], 0.0F, maximum)) {
            add_error(diagnostics, "sky-atmosphere.value-range", component_path,
                      "Expected a finite " + std::string(unit) + " value in [0," +
                          std::to_string(maximum) + "]; lower the value or choose a valid physical unit.");
        }
    }
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
        // The buffer is deliberately oversized for a finite IEEE-754 float;
        // this is an explicit fallback for non-conforming standard libraries.
        output += "0";
    }
}

void append_unsigned(std::string& output, const std::uint64_t value) {
    output += std::to_string(value);
}

void append_key(std::string& output, const std::string_view key, const bool& first) {
    if (!first) output.push_back(',');
    append_json_string(output, key);
    output.push_back(':');
}

void append_bool(std::string& output, const bool value) {
    output += value ? "true" : "false";
}

void append_rgb(std::string& output, const std::array<float, 3>& values) {
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_number(output, values[index]);
    }
    output.push_back(']');
}

void append_budget(std::string& output, const SkyAtmosphereLutBudget& budget) {
    output.push_back('{');
    bool first = true;
    append_key(output, "enabled", first); first = false; append_bool(output, budget.enabled);
    append_key(output, "transmittance", first); first = false;
    output.push_back('{');
    bool nested_first = true;
    append_key(output, "width", nested_first); nested_first = false; append_unsigned(output, budget.transmittance_width);
    append_key(output, "height", nested_first); nested_first = false; append_unsigned(output, budget.transmittance_height);
    append_key(output, "samples", nested_first); nested_first = false; append_unsigned(output, budget.transmittance_samples);
    output.push_back('}');
    append_key(output, "multiScattering", first); first = false;
    output.push_back('{'); nested_first = true;
    append_key(output, "width", nested_first); nested_first = false; append_unsigned(output, budget.multi_scattering_width);
    append_key(output, "height", nested_first); nested_first = false; append_unsigned(output, budget.multi_scattering_height);
    append_key(output, "samples", nested_first); nested_first = false; append_unsigned(output, budget.multi_scattering_samples);
    output.push_back('}');
    append_key(output, "skyView", first); first = false;
    output.push_back('{'); nested_first = true;
    append_key(output, "width", nested_first); nested_first = false; append_unsigned(output, budget.sky_view_width);
    append_key(output, "height", nested_first); nested_first = false; append_unsigned(output, budget.sky_view_height);
    append_key(output, "samples", nested_first); nested_first = false; append_unsigned(output, budget.sky_view_samples);
    output.push_back('}');
    append_key(output, "cameraVolume", first); first = false;
    output.push_back('{'); nested_first = true;
    append_key(output, "width", nested_first); nested_first = false; append_unsigned(output, budget.camera_volume_width);
    append_key(output, "height", nested_first); nested_first = false; append_unsigned(output, budget.camera_volume_height);
    append_key(output, "slices", nested_first); nested_first = false; append_unsigned(output, budget.camera_volume_slices);
    append_key(output, "samples", nested_first); nested_first = false; append_unsigned(output, budget.aerial_perspective_samples);
    output.push_back('}');
    append_key(output, "lutStorageBytes", first); first = false; append_unsigned(output, budget.lut_storage_bytes);
    output.push_back('}');
}

void append_physical(std::string& output, const SkyAtmosphereSettings& settings) {
    output.push_back('{');
    bool first = true;
    append_key(output, "planetRadiusM", first); first = false; append_number(output, settings.planet_radius_m);
    append_key(output, "atmosphereHeightM", first); first = false; append_number(output, settings.atmosphere_height_m);
    append_key(output, "rayleighScaleHeightM", first); first = false; append_number(output, settings.rayleigh_scale_height_m);
    append_key(output, "mieScaleHeightM", first); first = false; append_number(output, settings.mie_scale_height_m);
    append_key(output, "groundAlbedo", first); first = false; append_rgb(output, settings.ground_albedo);
    append_key(output, "rayleighScatteringPerM", first); first = false; append_rgb(output, settings.rayleigh_scattering_per_m);
    append_key(output, "rayleighAbsorptionPerM", first); first = false; append_rgb(output, settings.rayleigh_absorption_per_m);
    append_key(output, "mieScatteringPerM", first); first = false; append_rgb(output, settings.mie_scattering_per_m);
    append_key(output, "mieAbsorptionPerM", first); first = false; append_rgb(output, settings.mie_absorption_per_m);
    append_key(output, "miePhaseG", first); first = false; append_number(output, settings.mie_phase_g);
    append_key(output, "ozoneAbsorptionPerM", first); first = false; append_rgb(output, settings.ozone_absorption_per_m);
    append_key(output, "ozoneCenterHeightM", first); first = false; append_number(output, settings.ozone_center_height_m);
    append_key(output, "ozoneWidthM", first); first = false; append_number(output, settings.ozone_width_m);
    append_key(output, "sunDirection", first); first = false; append_rgb(output, settings.sun_direction);
    append_key(output, "sunIrradiance", first); first = false; append_rgb(output, settings.sun_irradiance);
    append_key(output, "sunAngularRadiusRad", first); first = false; append_number(output, settings.sun_angular_radius_rad);
    output.push_back('}');
}

void append_history_source(std::string& output, const SkyAtmosphereSettings& settings) {
    // debug_view is deliberately absent: inspecting a LUT must not invalidate
    // a temporal resource.  Quality and profile identity remain present since
    // they change LUT dimensions or the resource namespace.
    output.push_back('{');
    bool first = true;
    append_key(output, "schema", first); first = false; append_json_string(output, settings.schema);
    append_key(output, "profileId", first); first = false; append_json_string(output, settings.profile_id);
    append_key(output, "enabled", first); first = false; append_bool(output, settings.enabled);
    append_key(output, "quality", first); first = false;
    append_json_string(output, sky_atmosphere_quality_name(settings.quality));
    append_key(output, "physical", first); first = false; append_physical(output, settings);
    output.push_back('}');
}

std::string history_source(const SkyAtmosphereSettings& settings) {
    std::string output;
    output.reserve(1024U);
    append_history_source(output, settings);
    return output;
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
        const auto shift = static_cast<unsigned>((output.size() - index - 1U) * 4U);
        output[index] = hex[(hash >> shift) & 0x0FU];
    }
    return output;
}

} // namespace

std::string_view sky_atmosphere_quality_name(const SkyAtmosphereQuality quality) noexcept {
    switch (quality) {
    case SkyAtmosphereQuality::off: return "off";
    case SkyAtmosphereQuality::low: return "low";
    case SkyAtmosphereQuality::medium: return "medium";
    case SkyAtmosphereQuality::high: return "high";
    case SkyAtmosphereQuality::ultra: return "ultra";
    }
    return "invalid";
}

bool sky_atmosphere_quality_valid(const SkyAtmosphereQuality quality) noexcept {
    return valid_quality(quality);
}

std::string_view sky_atmosphere_debug_view_name(const SkyAtmosphereDebugView view) noexcept {
    switch (view) {
    case SkyAtmosphereDebugView::final: return "final";
    case SkyAtmosphereDebugView::transmittance_lut: return "transmittanceLut";
    case SkyAtmosphereDebugView::multi_scattering_lut: return "multiScatteringLut";
    case SkyAtmosphereDebugView::sky_view_lut: return "skyViewLut";
    case SkyAtmosphereDebugView::camera_volume_lut: return "cameraVolumeLut";
    case SkyAtmosphereDebugView::aerial_perspective: return "aerialPerspective";
    case SkyAtmosphereDebugView::optical_depth: return "opticalDepth";
    case SkyAtmosphereDebugView::rayleigh_scattering: return "rayleighScattering";
    case SkyAtmosphereDebugView::mie_scattering: return "mieScattering";
    case SkyAtmosphereDebugView::ozone_absorption: return "ozoneAbsorption";
    case SkyAtmosphereDebugView::sun_visibility: return "sunVisibility";
    }
    return "invalid";
}

bool sky_atmosphere_debug_view_valid(const SkyAtmosphereDebugView view) noexcept {
    return valid_debug_view(view);
}

SkyAtmosphereQuality sky_atmosphere_quality_from_string(const std::string_view value) noexcept {
    if (value == "off") return SkyAtmosphereQuality::off;
    if (value == "low") return SkyAtmosphereQuality::low;
    if (value == "medium") return SkyAtmosphereQuality::medium;
    if (value == "high") return SkyAtmosphereQuality::high;
    if (value == "ultra") return SkyAtmosphereQuality::ultra;
    return SkyAtmosphereQuality::off;
}

SkyAtmosphereDebugView sky_atmosphere_debug_view_from_string(const std::string_view value) noexcept {
    if (value == "final") return SkyAtmosphereDebugView::final;
    if (value == "transmittanceLut") return SkyAtmosphereDebugView::transmittance_lut;
    if (value == "multiScatteringLut") return SkyAtmosphereDebugView::multi_scattering_lut;
    if (value == "skyViewLut") return SkyAtmosphereDebugView::sky_view_lut;
    if (value == "cameraVolumeLut") return SkyAtmosphereDebugView::camera_volume_lut;
    if (value == "aerialPerspective") return SkyAtmosphereDebugView::aerial_perspective;
    if (value == "opticalDepth") return SkyAtmosphereDebugView::optical_depth;
    if (value == "rayleighScattering") return SkyAtmosphereDebugView::rayleigh_scattering;
    if (value == "mieScattering") return SkyAtmosphereDebugView::mie_scattering;
    if (value == "ozoneAbsorption") return SkyAtmosphereDebugView::ozone_absorption;
    if (value == "sunVisibility") return SkyAtmosphereDebugView::sun_visibility;
    return SkyAtmosphereDebugView::final;
}

const SkyAtmosphereLutBudget& sky_atmosphere_quality_budget(
    const SkyAtmosphereQuality quality) noexcept {
    switch (quality) {
    case SkyAtmosphereQuality::off: return off_budget;
    case SkyAtmosphereQuality::low: return low_budget;
    case SkyAtmosphereQuality::medium: return medium_budget;
    case SkyAtmosphereQuality::high: return high_budget;
    case SkyAtmosphereQuality::ultra: return ultra_budget;
    }
    return off_budget;
}

SkyAtmosphereSettings make_sky_atmosphere_settings(const SkyAtmosphereQuality quality) {
    SkyAtmosphereSettings settings;
    settings.quality = quality;
    settings.enabled = quality != SkyAtmosphereQuality::off;
    return settings;
}

std::vector<SkyAtmosphereDiagnostic> validate_sky_atmosphere(
    const SkyAtmosphereSettings& settings) {
    std::vector<SkyAtmosphereDiagnostic> diagnostics;
    if (settings.schema != sky_atmosphere_schema) {
        add_error(diagnostics, "sky-atmosphere.unsupported-schema", "/schema",
                  "Expected noemancer.sky-atmosphere/0.1; migrate the document before rendering.");
    }
    if (!has_non_whitespace(settings.profile_id)) {
        add_error(diagnostics, "sky-atmosphere.empty-profile-id", "/profileId",
                  "profileId must contain a non-whitespace stable identity.");
    } else if (settings.profile_id.size() > 128U) {
        add_error(diagnostics, "sky-atmosphere.profile-id-range", "/profileId",
                  "profileId must be at most 128 UTF-8 bytes.");
    }
    if (!sky_atmosphere_quality_valid(settings.quality)) {
        add_error(diagnostics, "sky-atmosphere.invalid-quality", "/quality",
                  "quality must be one of off, low, medium, high or ultra.");
    }
    if (!sky_atmosphere_debug_view_valid(settings.debug_view)) {
        add_error(diagnostics, "sky-atmosphere.invalid-debug-view", "/debugView",
                  "debugView must be one of the published LUT or scattering views.");
    }
    if (!finite_in_range(settings.planet_radius_m, min_planet_radius_m, max_planet_radius_m)) {
        add_error(diagnostics, "sky-atmosphere.planet-radius-range", "/planetRadiusM",
                  "planetRadiusM must be finite in [1000,1000000000] metres.");
    }
    if (!finite_in_range(settings.atmosphere_height_m, min_atmosphere_height_m, max_atmosphere_height_m)) {
        add_error(diagnostics, "sky-atmosphere.atmosphere-height-range", "/atmosphereHeightM",
                  "atmosphereHeightM must be finite in [100,1000000] metres.");
    }
    if (std::isfinite(settings.planet_radius_m) && std::isfinite(settings.atmosphere_height_m) &&
        settings.atmosphere_height_m >= settings.planet_radius_m) {
        add_error(diagnostics, "sky-atmosphere.radius-order", "/atmosphereHeightM",
                  "atmosphereHeightM must be smaller than planetRadiusM; use an outer radius of planet plus height.");
    }
    if (!finite_in_range(settings.rayleigh_scale_height_m, 1.0F, max_atmosphere_height_m)) {
        add_error(diagnostics, "sky-atmosphere.rayleigh-scale-height-range", "/rayleighScaleHeightM",
                  "rayleighScaleHeightM must be finite in [1,1000000] metres.");
    }
    if (!finite_in_range(settings.mie_scale_height_m, 1.0F, max_atmosphere_height_m)) {
        add_error(diagnostics, "sky-atmosphere.mie-scale-height-range", "/mieScaleHeightM",
                  "mieScaleHeightM must be finite in [1,1000000] metres.");
    }
    validate_rgb(diagnostics, settings.ground_albedo, "/groundAlbedo", 1.0F, "linear albedo");
    validate_rgb(diagnostics, settings.rayleigh_scattering_per_m, "/rayleighScatteringPerM",
                 max_coefficient_per_m, "scattering coefficient per metre");
    validate_rgb(diagnostics, settings.rayleigh_absorption_per_m, "/rayleighAbsorptionPerM",
                 max_coefficient_per_m, "absorption coefficient per metre");
    validate_rgb(diagnostics, settings.mie_scattering_per_m, "/mieScatteringPerM",
                 max_coefficient_per_m, "scattering coefficient per metre");
    validate_rgb(diagnostics, settings.mie_absorption_per_m, "/mieAbsorptionPerM",
                 max_coefficient_per_m, "absorption coefficient per metre");
    validate_rgb(diagnostics, settings.ozone_absorption_per_m, "/ozoneAbsorptionPerM",
                 max_coefficient_per_m, "absorption coefficient per metre");
    if (!finite_in_range(settings.mie_phase_g, -0.99F, 0.99F)) {
        add_error(diagnostics, "sky-atmosphere.mie-phase-range", "/miePhaseG",
                  "miePhaseG must be finite in (-1,1); values near 0.8 model forward-scattering aerosols.");
    }
    if (!finite_in_range(settings.ozone_center_height_m, 0.0F, max_atmosphere_height_m)) {
        add_error(diagnostics, "sky-atmosphere.ozone-center-range", "/ozoneCenterHeightM",
                  "ozoneCenterHeightM must be finite within the atmosphere height.");
    }
    if (!finite_in_range(settings.ozone_width_m, 1.0F, max_atmosphere_height_m)) {
        add_error(diagnostics, "sky-atmosphere.ozone-width-range", "/ozoneWidthM",
                  "ozoneWidthM must be finite and at least one metre within the atmosphere height.");
    }
    if (std::isfinite(settings.ozone_center_height_m) && std::isfinite(settings.ozone_width_m) &&
        (settings.ozone_center_height_m - settings.ozone_width_m * 0.5F < 0.0F ||
         settings.ozone_center_height_m + settings.ozone_width_m * 0.5F > settings.atmosphere_height_m)) {
        add_error(diagnostics, "sky-atmosphere.ozone-band-range", "/ozoneCenterHeightM",
                  "The ozone center plus/minus half width must stay inside [0,atmosphereHeightM].");
    }
    const auto direction_length = std::sqrt(
        static_cast<double>(settings.sun_direction[0]) * settings.sun_direction[0] +
        static_cast<double>(settings.sun_direction[1]) * settings.sun_direction[1] +
        static_cast<double>(settings.sun_direction[2]) * settings.sun_direction[2]);
    // Direction components can be negative, so the generic non-negative RGB
    // helper is not suitable here; report their range separately and then the
    // unit-length condition.
    for (std::size_t index = 0; index < settings.sun_direction.size(); ++index) {
        if (!finite_in_range(settings.sun_direction[index], -1.0F, 1.0F)) {
            add_error(diagnostics, "sky-atmosphere.sun-direction-range",
                      "/sunDirection/" + std::to_string(index),
                      "Each sunDirection component must be finite in [-1,1].");
        }
    }
    if (!std::isfinite(direction_length) || direction_length < 0.99 || direction_length > 1.01) {
        add_error(diagnostics, "sky-atmosphere.sun-direction-normalization", "/sunDirection",
                  "sunDirection must be approximately unit length; normalize it before submitting the atmosphere.");
    }
    validate_rgb(diagnostics, settings.sun_irradiance, "/sunIrradiance", max_sun_irradiance,
                 "linear irradiance");
    if (!finite_in_range(settings.sun_angular_radius_rad, 1.0e-6F, 0.5F)) {
        add_error(diagnostics, "sky-atmosphere.sun-radius-range", "/sunAngularRadiusRad",
                  "sunAngularRadiusRad must be finite in [1e-6,0.5] radians.");
    }
    const auto& budget = sky_atmosphere_quality_budget(settings.quality);
    if (budget.enabled != (settings.quality != SkyAtmosphereQuality::off)) {
        add_error(diagnostics, "sky-atmosphere.quality-budget-invalid", "/quality",
                  "The selected quality preset has an inconsistent enabled budget.");
    }
    if (settings.enabled && settings.quality != SkyAtmosphereQuality::off) {
        const auto total_extinction = [&] {
            float sum = 0.0F;
            for (std::size_t index = 0; index < 3U; ++index) {
                sum += settings.rayleigh_scattering_per_m[index] + settings.rayleigh_absorption_per_m[index] +
                       settings.mie_scattering_per_m[index] + settings.mie_absorption_per_m[index] +
                       settings.ozone_absorption_per_m[index];
            }
            return sum;
        }();
        if (std::isfinite(total_extinction) && total_extinction <= 0.0F) {
            add_error(diagnostics, "sky-atmosphere.empty-medium", "/",
                      "Enabled atmosphere has no scattering or absorption; add a medium or set quality to off.");
        }
    }
    return diagnostics;
}

std::string sky_atmosphere_canonical_evidence(const SkyAtmosphereSettings& settings) {
    std::string output;
    output.reserve(2048U);
    output.push_back('{');
    bool first = true;
    append_key(output, "schema", first); first = false; append_json_string(output, settings.schema);
    append_key(output, "profileId", first); first = false; append_json_string(output, settings.profile_id);
    append_key(output, "enabled", first); first = false; append_bool(output, settings.enabled);
    append_key(output, "quality", first); first = false; append_json_string(output, sky_atmosphere_quality_name(settings.quality));
    append_key(output, "debugView", first); first = false; append_json_string(output, sky_atmosphere_debug_view_name(settings.debug_view));
    append_key(output, "physical", first); first = false; append_physical(output, settings);
    append_key(output, "qualityBudget", first); first = false; append_budget(output, sky_atmosphere_quality_budget(settings.quality));
    append_key(output, "historyResetIdentity", first); first = false;
    append_json_string(output, sky_atmosphere_history_reset_identity(settings));
    output.push_back('}');
    return output;
}

std::string sky_atmosphere_canonical_json(const SkyAtmosphereSettings& settings) {
    return sky_atmosphere_canonical_evidence(settings);
}

std::string sky_atmosphere_fingerprint(const SkyAtmosphereSettings& settings) {
    return "fnv1a64:" + fnv1a_hex(sky_atmosphere_canonical_evidence(settings));
}

std::string sky_atmosphere_history_reset_identity(const SkyAtmosphereSettings& settings) {
    return "sky-atmosphere-history/1:fnv1a64:" + fnv1a_hex(history_source(settings));
}

} // namespace noemancer
