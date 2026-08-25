#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Reference decision record (research only; no third-party source is copied
// into this contract):
//
// * Godot, commit 3000096f9aa6f46db98d3a6d2a9442d58cab96ac, MIT
//   (`scene/resources/3d/sky_material.h/.cpp`,
//   `servers/rendering/renderer_rd/environment/sky.h/.cpp`, and
//   `servers/rendering/renderer_rd/shaders/environment/sky.glsl`).  ADAPT:
//   separate authored sky inputs from renderer-owned sky resources and keep a
//   stable debug/material vocabulary.  REJECT: Godot's Object/RID/shader
//   ownership and implementation-specific serialization do not cross this
//   boundary.
// * WickedEngine, commit f4a0d2635d5224b4509da164fa75d90fbdaaea26, MIT
//   (`WickedEngine/shaders/skyAtmosphere.hlsli`,
//   `skyAtmosphere_transmittanceLutCS.hlsl`,
//   `skyAtmosphere_multiScatteredLuminanceLutCS.hlsl`,
//   `skyAtmosphere_skyViewLutCS.hlsl`,
//   `skyAtmosphere_cameraVolumeLutCS.hlsl`).  ADAPT: the four LUT stages,
//   physical scale-height/coefficient vocabulary and explicit sample budgets.
//   REJECT: Wicked's HLSL/global shader bindings are backend-specific and no
//   shader code is copied here.
// The resulting type is therefore an original plain-data contract; a later
// Renderer adapter may PORT compatible math or shaders only after the normal
// license/provenance review and without exposing those types here.

// This is an engine-owned, renderer-neutral atmosphere contract.  It contains
// no SDL/RHI, shader, texture or third-party type, so the same plain data can
// be consumed by the Editor, Runtime, CLI and Agent observation layers.
inline constexpr std::string_view sky_atmosphere_schema =
    "noemancer.sky-atmosphere/0.1";
inline constexpr std::size_t sky_atmosphere_max_source_bytes = 256U * 1024U;

enum class SkyAtmosphereQuality : std::uint8_t {
    off,
    low,
    medium,
    high,
    ultra,
    Off = off,
    Low = low,
    Medium = medium,
    High = high,
    Ultra = ultra,
};

enum class SkyAtmosphereDebugView : std::uint8_t {
    final,
    transmittance_lut,
    multi_scattering_lut,
    sky_view_lut,
    camera_volume_lut,
    aerial_perspective,
    optical_depth,
    rayleigh_scattering,
    mie_scattering,
    ozone_absorption,
    sun_visibility,
    Final = final,
    TransmittanceLut = transmittance_lut,
    MultiScatteringLut = multi_scattering_lut,
    SkyViewLut = sky_view_lut,
    CameraVolumeLut = camera_volume_lut,
    AerialPerspective = aerial_perspective,
    OpticalDepth = optical_depth,
    RayleighScattering = rayleigh_scattering,
    MieScattering = mie_scattering,
    OzoneAbsorption = ozone_absorption,
    SunVisibility = sun_visibility,
};

[[nodiscard]] std::string_view sky_atmosphere_quality_name(
    SkyAtmosphereQuality quality) noexcept;
[[nodiscard]] bool sky_atmosphere_quality_valid(
    SkyAtmosphereQuality quality) noexcept;
[[nodiscard]] std::string_view sky_atmosphere_debug_view_name(
    SkyAtmosphereDebugView view) noexcept;
[[nodiscard]] bool sky_atmosphere_debug_view_valid(
    SkyAtmosphereDebugView view) noexcept;

// A string parser is deliberately kept in the engine contract rather than in
// an Editor/JSON adapter so CLI, Agent and GUI use exactly one vocabulary.
[[nodiscard]] SkyAtmosphereQuality sky_atmosphere_quality_from_string(
    std::string_view value) noexcept;
[[nodiscard]] SkyAtmosphereDebugView sky_atmosphere_debug_view_from_string(
    std::string_view value) noexcept;

struct SkyAtmosphereLutBudget final {
    bool enabled{};

    std::uint32_t transmittance_width{};
    std::uint32_t transmittance_height{};
    std::uint32_t multi_scattering_width{};
    std::uint32_t multi_scattering_height{};
    std::uint32_t sky_view_width{};
    std::uint32_t sky_view_height{};
    std::uint32_t camera_volume_width{};
    std::uint32_t camera_volume_height{};
    std::uint32_t camera_volume_slices{};

    std::uint32_t transmittance_samples{};
    std::uint32_t multi_scattering_samples{};
    std::uint32_t sky_view_samples{};
    std::uint32_t aerial_perspective_samples{};

    // This is an upper bound for RGBA16F storage (8 bytes/texel) across the
    // four LUTs.  It is a contract budget, not a claim about native VRAM.
    std::uint64_t lut_storage_bytes{};
};

[[nodiscard]] const SkyAtmosphereLutBudget& sky_atmosphere_quality_budget(
    SkyAtmosphereQuality quality) noexcept;

struct SkyAtmosphereDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

// Values are SI units unless the field name says otherwise.  Scattering and
// absorption coefficients are per metre (m^-1); RGB values are linear.  The
// sun direction points from the planet towards the light and is expected to
// be unit length.  The default values are an Earth-like reference, not a
// renderer-specific shader constant.
struct SkyAtmosphereSettings final {
    std::string schema{std::string(sky_atmosphere_schema)};
    std::string profile_id{"default"};
    bool enabled{true};
    SkyAtmosphereQuality quality{SkyAtmosphereQuality::high};
    SkyAtmosphereDebugView debug_view{SkyAtmosphereDebugView::final};

    float planet_radius_m{6'371'000.0F};
    float atmosphere_height_m{100'000.0F};
    float rayleigh_scale_height_m{8'000.0F};
    float mie_scale_height_m{1'200.0F};
    std::array<float, 3> ground_albedo{0.30F, 0.30F, 0.30F};

    std::array<float, 3> rayleigh_scattering_per_m{
        5.802e-6F, 13.558e-6F, 33.100e-6F};
    std::array<float, 3> rayleigh_absorption_per_m{0.0F, 0.0F, 0.0F};
    std::array<float, 3> mie_scattering_per_m{
        0.444e-6F, 0.444e-6F, 0.444e-6F};
    std::array<float, 3> mie_absorption_per_m{
        3.996e-6F, 3.996e-6F, 3.996e-6F};
    float mie_phase_g{0.80F};

    std::array<float, 3> ozone_absorption_per_m{
        0.650e-6F, 1.881e-6F, 0.085e-6F};
    float ozone_center_height_m{25'000.0F};
    float ozone_width_m{30'000.0F};

    std::array<float, 3> sun_direction{0.0F, 0.70710677F, 0.70710677F};
    std::array<float, 3> sun_irradiance{1.0F, 1.0F, 1.0F};
    float sun_angular_radius_rad{0.004675F};
};

struct SkyAtmosphereParseResult final {
    std::optional<SkyAtmosphereSettings> document;
    std::vector<SkyAtmosphereDiagnostic> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return document.has_value() && errors.empty();
    }
};

// The codec is the only JSON-facing part of this feature.  Its public
// boundary still exposes only engine-owned values and diagnostics; nlohmann
// JSON remains private to the implementation, just like the other engine
// document codecs.
class SkyAtmosphereSettingsCodec final {
public:
    [[nodiscard]] static SkyAtmosphereParseResult parse_json(std::string_view source);
    [[nodiscard]] static std::vector<SkyAtmosphereDiagnostic> validate(
        const SkyAtmosphereSettings& settings);
    [[nodiscard]] static std::string write_canonical_json(
        const SkyAtmosphereSettings& settings);
    [[nodiscard]] static std::string fingerprint(
        const SkyAtmosphereSettings& settings);
};

[[nodiscard]] SkyAtmosphereSettings make_sky_atmosphere_settings(
    SkyAtmosphereQuality quality);

[[nodiscard]] std::vector<SkyAtmosphereDiagnostic> validate_sky_atmosphere(
    const SkyAtmosphereSettings& settings);

// Canonical evidence is a deterministic, compact JSON object made only from
// the fields in this contract.  It is intentionally emitted here without a
// JSON dependency so an isolated renderer/Agent tool can use the same bytes.
[[nodiscard]] std::string sky_atmosphere_canonical_evidence(
    const SkyAtmosphereSettings& settings);

// Alias for adapters that use the Codec naming used by other engine assets.
[[nodiscard]] std::string sky_atmosphere_canonical_json(
    const SkyAtmosphereSettings& settings);

// The fingerprint uses the engine's existing lightweight deterministic
// spelling.  It is an identity for the complete canonical evidence.
[[nodiscard]] std::string sky_atmosphere_fingerprint(
    const SkyAtmosphereSettings& settings);

// Temporal LUT/history resources compare this identity each frame.  Debug
// view changes intentionally do not reset history; physical/quality/profile
// changes do.  A caller can still force a reset by changing profile_id or by
// owning a higher-level frame epoch in its renderer authority.
[[nodiscard]] std::string sky_atmosphere_history_reset_identity(
    const SkyAtmosphereSettings& settings);

} // namespace noemancer
