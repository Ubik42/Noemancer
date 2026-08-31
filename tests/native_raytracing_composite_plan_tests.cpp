#include "runtime/native_raytracing_composite_plan.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition)
        std::cerr << "native_raytracing_composite_plan_tests: " << message << '\n';
    return condition;
}

NativeRayTracingCompositeInput ready_input() {
    NativeRayTracingCompositeInput input;
    input.resource_id = "rt.native.output.texture.1";
    input.width = 1280U;
    input.height = 720U;
    input.resource_generation = 7U;
    input.producer_complete = true;
    input.shader_readable = true;
    return input;
}

} // namespace

int main() {
    const auto input = ready_input();
    const auto plan = build_native_raytracing_composite_plan(input);
    if (!check(plan.valid && plan.supported && plan.debug_composite &&
                   plan.integer_texture_load && !plan.claims_rtgi &&
                   plan.future_rtgi_planned && !plan.rtgi_implemented &&
                   !plan.fallback_active && plan.input_format == "R32G32B32A32_UINT" &&
                   plan.output_format == "RGBA8_UNORM" &&
                   plan.producer_shader_contract == native_raytracing_composite_marker_contract &&
                   plan.tone_mapping_precedes_composite &&
                   plan.stage == native_raytracing_composite_stage &&
                   !plan.float_bit_pattern_load &&
                   plan.read_path == "Texture2D<uint4>.Load" &&
                   plan.width == input.width && plan.height == input.height &&
                   plan.resource_generation == input.resource_generation,
               "ready integer output did not produce the bounded debug composite plan"))
        return 1;

    auto buffer_input = input;
    buffer_input.resource_kind = "buffer";
    const auto buffer_plan = build_native_raytracing_composite_plan(buffer_input);
    if (!check(!buffer_plan.valid && !buffer_plan.supported && buffer_plan.fallback_active &&
                   buffer_plan.code == "input-resource-kind-invalid" &&
                   buffer_plan.visual_path == "raster-pbr-fallback",
               "the current native buffer was incorrectly treated as directly sampleable"))
        return 2;

    auto incomplete_input = input;
    incomplete_input.shader_readable = false;
    const auto incomplete_plan = build_native_raytracing_composite_plan(incomplete_input);
    if (!check(!incomplete_plan.valid && incomplete_plan.code == "input-not-ready",
               "an output without a shader-read transition was accepted"))
        return 3;

    const auto future_plan = build_native_raytracing_composite_plan(
        input, NativeRayTracingCompositeMode::future_rtgi);
    if (!check(!future_plan.valid && !future_plan.supported &&
                   future_plan.future_rtgi_planned && !future_plan.rtgi_implemented &&
                   !future_plan.claims_rtgi && future_plan.code == "rtgi-not-implemented",
               "the future RTGI mode was presented as implemented"))
        return 4;

    auto radiance_input = input;
    radiance_input.producer_shader_contract =
        std::string(native_raytracing_composite_radiance_contract);
    const auto radiance_plan = build_native_raytracing_composite_plan(
        radiance_input, NativeRayTracingCompositeMode::linear_radiance);
    if (!check(radiance_plan.valid && radiance_plan.supported &&
                   !radiance_plan.debug_composite && radiance_plan.linear_radiance_composite &&
                   radiance_plan.float_bit_pattern_load && radiance_plan.integer_texture_load &&
                   radiance_plan.producer_shader_contract == native_raytracing_composite_radiance_contract &&
                   radiance_plan.debug_mode == 1U && radiance_plan.tone_mapping_precedes_composite &&
                   radiance_plan.decode == "asfloat-r32-bit-pattern/sanitize-scene-linear/aces-srgb" &&
                   radiance_plan.color_space == "sRGB-display-encoded" &&
                   radiance_plan.visual_path == "native-rt-linear-radiance-composite" &&
                   !radiance_plan.claims_rtgi && !radiance_plan.rtgi_implemented,
               "the native 0.3 float bit pattern did not produce a bounded radiance composite plan"))
        return 5;

    auto stale_radiance_input = radiance_input;
    stale_radiance_input.producer_shader_contract =
        "noemancer.native-rt-full-frame/0.2";
    const auto stale_radiance_plan = build_native_raytracing_composite_plan(
        stale_radiance_input, NativeRayTracingCompositeMode::linear_radiance);
    if (!check(!stale_radiance_plan.valid && stale_radiance_plan.fallback_active &&
                   stale_radiance_plan.code == "producer-contract-unsupported" &&
                   stale_radiance_plan.visual_path == "raster-pbr-fallback",
               "a pre-0.3 full-frame producer was not rejected before radiance decoding"))
        return 6;

    auto mismatched_marker_input = input;
    mismatched_marker_input.producer_shader_contract =
        std::string(native_raytracing_composite_radiance_contract);
    const auto mismatched_marker_plan = build_native_raytracing_composite_plan(
        mismatched_marker_input, NativeRayTracingCompositeMode::debug_marker);
    if (!check(!mismatched_marker_plan.valid &&
                   mismatched_marker_plan.code == "producer-contract-unsupported",
               "the marker mode accepted a scene-linear producer contract"))
        return 7;

    auto bad_extent = input;
    bad_extent.width = native_raytracing_composite_max_extent + 1U;
    const auto extent_plan = build_native_raytracing_composite_plan(bad_extent);
    if (!check(!extent_plan.valid && extent_plan.code == "input-extent-invalid",
               "an out-of-budget native output extent was accepted"))
        return 8;

    std::cout << "native_raytracing_composite_plan_tests: ok\n";
    return 0;
}
