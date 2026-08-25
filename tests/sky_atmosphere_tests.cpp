#include "engine/sky_atmosphere.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int fail(const char* message) {
    std::cerr << "sky_atmosphere_tests: " << message << '\n';
    return 1;
}

bool has_diagnostic(const std::vector<noemancer::SkyAtmosphereDiagnostic>& diagnostics,
                    const std::string_view code, const std::string_view path) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code && diagnostic.path == path && !diagnostic.message.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using namespace noemancer;

    // Every published preset has an explicit, monotonic LUT/sample budget.
    const auto off = make_sky_atmosphere_settings(SkyAtmosphereQuality::off);
    const auto low = make_sky_atmosphere_settings(SkyAtmosphereQuality::low);
    const auto medium = make_sky_atmosphere_settings(SkyAtmosphereQuality::medium);
    const auto high = make_sky_atmosphere_settings(SkyAtmosphereQuality::high);
    const auto ultra = make_sky_atmosphere_settings(SkyAtmosphereQuality::ultra);
    if (off.enabled || sky_atmosphere_quality_budget(SkyAtmosphereQuality::off).lut_storage_bytes != 0U ||
        !sky_atmosphere_quality_budget(SkyAtmosphereQuality::low).enabled ||
        sky_atmosphere_quality_budget(SkyAtmosphereQuality::low).lut_storage_bytes >=
            sky_atmosphere_quality_budget(SkyAtmosphereQuality::medium).lut_storage_bytes ||
        sky_atmosphere_quality_budget(SkyAtmosphereQuality::medium).lut_storage_bytes >=
            sky_atmosphere_quality_budget(SkyAtmosphereQuality::high).lut_storage_bytes ||
        sky_atmosphere_quality_budget(SkyAtmosphereQuality::high).lut_storage_bytes >=
            sky_atmosphere_quality_budget(SkyAtmosphereQuality::ultra).lut_storage_bytes ||
        sky_atmosphere_quality_budget(SkyAtmosphereQuality::high).sky_view_width != 192U ||
        sky_atmosphere_quality_budget(SkyAtmosphereQuality::ultra).camera_volume_slices != 48U) {
        return fail("quality presets do not expose the expected budgets");
    }
    if (!sky_atmosphere_quality_valid(SkyAtmosphereQuality::high) ||
        sky_atmosphere_quality_valid(static_cast<SkyAtmosphereQuality>(99U)) ||
        sky_atmosphere_quality_from_string("ultra") != SkyAtmosphereQuality::ultra ||
        sky_atmosphere_quality_from_string("unknown") != SkyAtmosphereQuality::off ||
        sky_atmosphere_debug_view_from_string("skyViewLut") != SkyAtmosphereDebugView::sky_view_lut ||
        sky_atmosphere_debug_view_name(SkyAtmosphereDebugView::camera_volume_lut) != "cameraVolumeLut") {
        return fail("quality/debug vocabulary is not stable");
    }
    for (const auto& settings : {off, low, medium, high, ultra}) {
        if (!validate_sky_atmosphere(settings).empty() && settings.quality != SkyAtmosphereQuality::off) {
            return fail("Earth-like preset unexpectedly failed validation");
        }
    }
    auto negative_sun_direction = high;
    negative_sun_direction.sun_direction = {-0.70710677F, 0.0F, 0.70710677F};
    if (!validate_sky_atmosphere(negative_sun_direction).empty()) {
        return fail("valid negative sun direction was rejected");
    }

    // Diagnostics carry a stable code, field path and actionable message.
    auto invalid = high;
    invalid.profile_id = "   ";
    invalid.planet_radius_m = std::numeric_limits<float>::quiet_NaN();
    invalid.mie_phase_g = 1.0F;
    invalid.sun_direction = {0.0F, 0.0F, 0.0F};
    invalid.ozone_center_height_m = 1'000.0F;
    invalid.ozone_width_m = 100'000.0F;
    const auto diagnostics = validate_sky_atmosphere(invalid);
    if (!has_diagnostic(diagnostics, "sky-atmosphere.empty-profile-id", "/profileId") ||
        !has_diagnostic(diagnostics, "sky-atmosphere.planet-radius-range", "/planetRadiusM") ||
        !has_diagnostic(diagnostics, "sky-atmosphere.mie-phase-range", "/miePhaseG") ||
        !has_diagnostic(diagnostics, "sky-atmosphere.sun-direction-normalization", "/sunDirection") ||
        !has_diagnostic(diagnostics, "sky-atmosphere.ozone-band-range", "/ozoneCenterHeightM")) {
        return fail("invalid settings did not return actionable diagnostics");
    }

    // Canonical evidence and identities are deterministic and independent of
    // calls or temporary allocations.
    const auto first_evidence = sky_atmosphere_canonical_evidence(high);
    const auto second_evidence = sky_atmosphere_canonical_json(high);
    const auto first_fingerprint = sky_atmosphere_fingerprint(high);
    const auto second_fingerprint = sky_atmosphere_fingerprint(high);
    const auto first_history = sky_atmosphere_history_reset_identity(high);
    const auto second_history = sky_atmosphere_history_reset_identity(high);
    if (first_evidence != second_evidence || first_fingerprint != second_fingerprint ||
        first_history != second_history || first_fingerprint.rfind("fnv1a64:", 0U) != 0U ||
        first_history.rfind("sky-atmosphere-history/1:fnv1a64:", 0U) != 0U ||
        first_evidence.find("qualityBudget") == std::string::npos ||
        first_evidence.find("rayleighScatteringPerM") == std::string::npos) {
        return fail("canonical evidence or fingerprint was not deterministic");
    }

    // Debug views are presentation-only and must not flush temporal LUT
    // history; physical and quality changes must produce a new identity.
    auto debug_changed = high;
    debug_changed.debug_view = SkyAtmosphereDebugView::transmittance_lut;
    if (sky_atmosphere_fingerprint(debug_changed) == first_fingerprint ||
        sky_atmosphere_history_reset_identity(debug_changed) != first_history) {
        return fail("debug view incorrectly changed history identity");
    }
    auto physical_changed = high;
    physical_changed.mie_phase_g = 0.72F;
    if (sky_atmosphere_history_reset_identity(physical_changed) == first_history) {
        return fail("physical change did not reset history identity");
    }
    auto quality_changed = high;
    quality_changed.quality = SkyAtmosphereQuality::ultra;
    if (sky_atmosphere_history_reset_identity(quality_changed) == first_history) {
        return fail("quality change did not reset history identity");
    }
    return 0;
}
