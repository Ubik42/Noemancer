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
    }
    return "unknown";
}

NativeRayTracingCompositePlan build_native_raytracing_composite_plan(
    const NativeRayTracingCompositeInput& input,
    const NativeRayTracingCompositeMode mode) {
    if (mode != NativeRayTracingCompositeMode::debug_marker &&
        mode != NativeRayTracingCompositeMode::future_rtgi) {
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
                            "The composite shader requires the native R32G32B32A32_UINT output format.");
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

    if (mode == NativeRayTracingCompositeMode::future_rtgi) {
        auto result = failure_plan(input, mode, "rtgi-not-implemented",
                                   "RTGI is reserved for a later radiance contract; the 0.1 shader only composites the native debug marker.");
        result.future_rtgi_planned = true;
        return result;
    }

    NativeRayTracingCompositePlan result;
    result.mode = mode;
    result.valid = true;
    result.supported = true;
    result.debug_composite = true;
    result.future_rtgi_planned = true;
    result.rtgi_implemented = false;
    result.claims_rtgi = false;
    result.integer_texture_load = true;
    result.fallback_active = false;
    result.code = "debug-composite-ready";
    result.detail = "The native integer marker output can be loaded with Texture2D<uint4>.Load and normalized into the linear post-process target; no RTGI claim is made.";
    result.visual_path = "native-rt-debug-composite";
    result.input_resource_id = input.resource_id;
    result.input_resource_kind = input.resource_kind;
    result.width = input.width;
    result.height = input.height;
    result.resource_generation = input.resource_generation;
    result.debug_mode = 0U;
    return result;
}

} // namespace noemancer
