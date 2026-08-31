#include "runtime/native_raytracing_composite_plan.hpp"

#include <string_view>

namespace noemancer {
namespace {

bool bounded_text(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= native_raytracing_composite_max_text_bytes;
}

NativeRayTracingCompositePlan failure_plan(
    const NativeRayTracingCompositeInput& input,
    const NativeRayTracingCompositeMode mode,
    const std::string_view code,
    const std::string_view detail) {
    NativeRayTracingCompositePlan result;
    result.mode = mode;
    result.code = std::string(code);
    result.detail = std::string(detail);
    result.width = input.width;
    result.height = input.height;
    result.resource_generation = input.resource_generation;
    result.visual_path = "raster-pbr-fallback";
    if (input.producer_shader_contract.size() <= native_raytracing_composite_max_text_bytes)
        result.producer_shader_contract = input.producer_shader_contract;
    return result;
}

} // namespace

std::string_view native_raytracing_composite_mode_name(
    const NativeRayTracingCompositeMode mode) noexcept {
    switch (mode) {
    case NativeRayTracingCompositeMode::debug_marker:
        return "debug-marker";
    case NativeRayTracingCompositeMode::future_rtgi:
        return "future-rtgi";
    case NativeRayTracingCompositeMode::linear_radiance:
        return "linear-radiance";
    }
    return "unknown";
}

NativeRayTracingCompositePlan build_native_raytracing_composite_plan(
    const NativeRayTracingCompositeInput& input,
    const NativeRayTracingCompositeMode mode) {
    if (mode != NativeRayTracingCompositeMode::debug_marker &&
        mode != NativeRayTracingCompositeMode::future_rtgi &&
        mode != NativeRayTracingCompositeMode::linear_radiance) {
        return failure_plan(input, mode, "composite-mode-invalid",
                            "The native RT composite mode is outside the versioned plan vocabulary.");
    }
    if (!bounded_text(input.resource_id)) {
        return failure_plan(input, mode, "input-resource-id-invalid",
                            "The native RT composite input requires a bounded stable resource id.");
    }
    if (input.resource_kind != "texture2d") {
        return failure_plan(input, mode, "input-resource-kind-invalid",
                            "The composite shader consumes an imported same-device Texture2D view; the current native buffer needs an interop copy first.");
    }
    if (input.format != native_raytracing_composite_input_format) {
        return failure_plan(input, mode, "input-format-invalid",
                            "The current SDL native output is a same-device R32G32B32A32_UINT texture; other physical formats are fail-closed until a versioned texture contract is added.");
    }
    if (input.width == 0U || input.height == 0U ||
        input.width > native_raytracing_composite_max_extent ||
        input.height > native_raytracing_composite_max_extent) {
        return failure_plan(input, mode, "input-extent-invalid",
                            "The native RT composite extent is outside its bounded 2D surface range.");
    }
    if (input.resource_generation == 0U) {
        return failure_plan(input, mode, "input-generation-invalid",
                            "A live native output must provide a non-zero resource generation.");
    }
    if (!input.producer_complete || !input.shader_readable) {
        return failure_plan(input, mode, "input-not-ready",
                            "The native output must be producer-complete and transitioned to shader-read before compositing.");
    }

    if (!bounded_text(input.producer_shader_contract)) {
        return failure_plan(input, mode, "producer-contract-invalid",
                            "The native output must identify a bounded versioned producer shader contract.");
    }

    if (mode == NativeRayTracingCompositeMode::future_rtgi) {
        auto result = failure_plan(input, mode, "rtgi-not-implemented",
                                   "RTGI is reserved for a later radiance contract; the 0.2 shader only presents marker or direct+ambient diagnostic output.");
        result.future_rtgi_planned = true;
        return result;
    }

    const bool marker_contract =
        input.producer_shader_contract == native_raytracing_composite_marker_contract;
    const bool radiance_contract =
        input.producer_shader_contract == native_raytracing_composite_radiance_contract;
    if ((mode == NativeRayTracingCompositeMode::debug_marker && !marker_contract) ||
        (mode == NativeRayTracingCompositeMode::linear_radiance && !radiance_contract)) {
        return failure_plan(input, mode, "producer-contract-unsupported",
                            mode == NativeRayTracingCompositeMode::linear_radiance
                                ? "Linear-radiance composite requires the exact native-rt-full-frame/0.3 producer contract."
                                : "Debug-marker composite requires the exact native-rt-marker-probe/0.1 producer contract.");
    }

    NativeRayTracingCompositePlan result;
    result.mode = mode;
    result.valid = true;
    result.supported = true;
    result.debug_composite = mode == NativeRayTracingCompositeMode::debug_marker;
    result.linear_radiance_composite = mode == NativeRayTracingCompositeMode::linear_radiance;
    result.future_rtgi_planned = true;
    result.rtgi_implemented = false;
    result.claims_rtgi = false;
    result.integer_texture_load = true;
    result.float_bit_pattern_load = result.linear_radiance_composite;
    result.tone_mapping_precedes_composite = true;
    result.fallback_active = false;
    result.code = result.linear_radiance_composite
        ? "linear-radiance-composite-ready"
        : "debug-composite-ready";
    result.detail = result.linear_radiance_composite
        ? "The R32G32B32A32_UINT SDL surface is a raw copy of native 0.3 float bits; Texture2D<uint4>.Load plus asfloat reconstructs scene-linear direct+ambient radiance, then applies bounded display mapping after the regular tone-map pass. No RTGI claim is made."
        : "The native integer marker output can be loaded with Texture2D<uint4>.Load and normalized into the linear post-process target; no RTGI claim is made.";
    result.visual_path = result.linear_radiance_composite
        ? "native-rt-linear-radiance-composite"
        : "native-rt-debug-composite";
    result.input_resource_id = input.resource_id;
    result.input_resource_kind = input.resource_kind;
    result.producer_shader_contract = input.producer_shader_contract;
    result.width = input.width;
    result.height = input.height;
    result.resource_generation = input.resource_generation;
    result.debug_mode = result.linear_radiance_composite ? 1U : 0U;
    if (result.linear_radiance_composite) {
        result.color_space = "sRGB-display-encoded";
        result.decode = "asfloat-r32-bit-pattern/sanitize-scene-linear/aces-srgb";
    }
    return result;
}

} // namespace noemancer
