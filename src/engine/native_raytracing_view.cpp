#include "engine/native_raytracing_view.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

namespace noemancer {
namespace {

using Vec3 = NativeRayTracingViewVec3;

constexpr float kBasisEpsilon = 1.0e-6F;
constexpr float kOrthonormalTolerance = 1.0e-4F;
constexpr float kDegreesToRadians = 0.01745329251994329577F;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

float dot(const Vec3& left, const Vec3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

Vec3 cross(const Vec3& left, const Vec3& right) noexcept {
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
}

Vec3 add(const Vec3& left, const Vec3& right) noexcept {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

Vec3 scale(const Vec3& value, const float amount) noexcept {
    return {value[0] * amount, value[1] * amount, value[2] * amount};
}

float magnitude(const Vec3& value) noexcept {
    return std::sqrt(dot(value, value));
}

Vec3 normalize(const Vec3& value) noexcept {
    const float length = magnitude(value);
    return length > kBasisEpsilon ? scale(value, 1.0F / length) : Vec3{};
}

bool finite(const Vec3& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool finite(const NativeRayTracingPrimaryRayParameters& value) noexcept {
    return std::isfinite(value.tan_half_fov_y) &&
           std::isfinite(value.tan_half_fov_x) &&
           std::isfinite(value.orthographic_half_height) &&
           std::isfinite(value.orthographic_half_width);
}

bool bounded_world_vector(const Vec3& value) noexcept {
    return finite(value) &&
           std::abs(value[0]) <= native_raytracing_view_max_world_coordinate &&
           std::abs(value[1]) <= native_raytracing_view_max_world_coordinate &&
           std::abs(value[2]) <= native_raytracing_view_max_world_coordinate;
}

bool bounded_text(const std::string_view value) noexcept {
    if (value.empty() || value.size() > native_raytracing_view_max_text_bytes) {
        return false;
    }
    return std::ranges::any_of(value, [](const char character) {
        return std::isspace(static_cast<unsigned char>(character)) == 0;
    });
}

std::string bounded_copy(const std::string_view value) {
    return std::string(value.substr(0, native_raytracing_view_max_text_bytes));
}

bool orthonormal(const NativeRayTracingViewBasis& basis) noexcept {
    const auto near_one = [](const float value) noexcept {
        return std::isfinite(value) && std::abs(value - 1.0F) <= kOrthonormalTolerance;
    };
    const auto near_zero = [](const float value) noexcept {
        return std::isfinite(value) && std::abs(value) <= kOrthonormalTolerance;
    };
    return finite(basis.position) && finite(basis.forward) && finite(basis.right) &&
           finite(basis.up) && near_one(dot(basis.forward, basis.forward)) &&
           near_one(dot(basis.right, basis.right)) && near_one(dot(basis.up, basis.up)) &&
           near_zero(dot(basis.forward, basis.right)) &&
           near_zero(dot(basis.forward, basis.up)) && near_zero(dot(basis.right, basis.up));
}

void hash_byte(std::uint64_t& hash, const std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

void hash_u32(std::uint64_t& hash, const std::uint32_t value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void hash_u64(std::uint64_t& hash, const std::uint64_t value) noexcept {
    hash_u32(hash, static_cast<std::uint32_t>(value & 0xffffffffULL));
    hash_u32(hash, static_cast<std::uint32_t>((value >> 32U) & 0xffffffffULL));
}

void hash_float(std::uint64_t& hash, const float value) noexcept {
    std::uint32_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u32(hash, bits);
}

void hash_text(std::uint64_t& hash, const std::string_view value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value.size()));
    for (const auto character : value) {
        hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
}

void hash_vec3(std::uint64_t& hash, const Vec3& value) noexcept {
    hash_float(hash, value[0]);
    hash_float(hash, value[1]);
    hash_float(hash, value[2]);
}

std::uint64_t view_fingerprint(const NativeRayTracingViewPlan& plan) noexcept {
    auto hash = kFnvOffset;
    hash_text(hash, plan.schema);
    hash_text(hash, plan.camera_id);
    hash_u64(hash, plan.camera_revision);
    hash_text(hash, plan.projection);
    hash_text(hash, plan.output_contract);
    hash_u32(hash, plan.output_width);
    hash_u32(hash, plan.output_height);
    hash_float(hash, plan.aspect);
    hash_float(hash, plan.near_clip);
    hash_float(hash, plan.far_clip);
    hash_vec3(hash, plan.basis.position);
    hash_vec3(hash, plan.basis.forward);
    hash_vec3(hash, plan.basis.right);
    hash_vec3(hash, plan.basis.up);
    hash_float(hash, plan.primary_ray_parameters.tan_half_fov_y);
    hash_float(hash, plan.primary_ray_parameters.tan_half_fov_x);
    hash_float(hash, plan.primary_ray_parameters.orthographic_half_height);
    hash_float(hash, plan.primary_ray_parameters.orthographic_half_width);
    return hash == 0U ? 1U : hash;
}

std::uint64_t pixel_fingerprint(const NativeRayTracingViewPlan& plan,
                                const NativeRayTracingPrimaryRay& ray) noexcept {
    auto hash = plan.primary_ray_fingerprint == 0U ? kFnvOffset : plan.primary_ray_fingerprint;
    hash_u32(hash, ray.pixel_x);
    hash_u32(hash, ray.pixel_y);
    hash_float(hash, ray.sample_u);
    hash_float(hash, ray.sample_v);
    hash_vec3(hash, ray.origin);
    hash_vec3(hash, ray.direction);
    hash_float(hash, ray.minimum_distance);
    hash_float(hash, ray.maximum_distance);
    return hash == 0U ? 1U : hash;
}

NativeRayTracingViewPlan initial_plan(const NativeRayTracingViewInput& input,
                                      const NativeRayTracingViewOutputMode output_mode) {
    NativeRayTracingViewPlan result;
    result.output_mode = output_mode;
    result.camera_id = bounded_copy(input.camera_id);
    result.camera_revision = input.camera_revision;
    result.projection = bounded_copy(input.projection);
    result.vertical_fov_degrees = input.vertical_fov_degrees;
    result.orthographic_height = input.orthographic_height;
    result.aspect = input.aspect;
    result.near_clip = input.near_clip;
    result.far_clip = input.far_clip;
    result.output_width = input.output_width;
    result.output_height = input.output_height;
    if (output_mode == NativeRayTracingViewOutputMode::future_linear_radiance) {
        result.output_contract =
            std::string(native_raytracing_future_linear_radiance_contract);
        result.output_semantic = "future-linear-radiance";
    }
    return result;
}

NativeRayTracingViewPlan failure_plan(const NativeRayTracingViewInput& input,
                                     const NativeRayTracingViewOutputMode output_mode,
                                     const std::string_view code,
                                     const std::string_view detail) {
    auto result = initial_plan(input, output_mode);
    result.code = std::string(code);
    result.detail = std::string(detail);
    result.fallback_active = true;
    return result;
}

NativeRayTracingPrimaryRayResult unavailable_ray(const std::string_view code,
                                                 const std::string_view detail) {
    NativeRayTracingPrimaryRayResult result;
    result.code = std::string(code);
    result.detail = std::string(detail);
    return result;
}

} // namespace

std::string_view native_raytracing_view_output_mode_name(
    const NativeRayTracingViewOutputMode mode) noexcept {
    switch (mode) {
    case NativeRayTracingViewOutputMode::diagnostic_hit_mask:
        return "diagnostic-hit-mask";
    case NativeRayTracingViewOutputMode::future_linear_radiance:
        return "future-linear-radiance";
    }
    return "unknown";
}

NativeRayTracingViewPlan build_native_raytracing_view_plan(
    const NativeRayTracingViewInput& input,
    const NativeRayTracingViewOutputMode output_mode) {
    if (output_mode != NativeRayTracingViewOutputMode::diagnostic_hit_mask &&
        output_mode != NativeRayTracingViewOutputMode::future_linear_radiance) {
        return failure_plan(input, output_mode, "view-output-mode-invalid",
                            "The native ray-tracing view output mode is outside the versioned contract vocabulary.");
    }
    if (!bounded_text(input.camera_id)) {
        return failure_plan(input, output_mode, "camera-id-invalid",
                            "The native ray-tracing view requires a non-empty bounded stable camera id.");
    }
    if (input.camera_revision == 0U) {
        return failure_plan(input, output_mode, "camera-revision-invalid",
                            "The native ray-tracing view requires a non-zero source camera revision.");
    }
    if (!bounded_world_vector(input.position)) {
        return failure_plan(input, output_mode, "camera-position-invalid",
                            "The camera position must contain finite coordinates inside the bounded world range.");
    }
    if (!bounded_world_vector(input.forward) || !bounded_world_vector(input.up) ||
        magnitude(input.forward) <= kBasisEpsilon || magnitude(input.up) <= kBasisEpsilon) {
        return failure_plan(input, output_mode, "camera-basis-invalid",
                            "The camera forward and up vectors must be finite, bounded and non-zero.");
    }
    if (input.projection != "perspective" && input.projection != "orthographic") {
        return failure_plan(input, output_mode, "projection-invalid",
                            "The native ray-tracing view accepts only perspective or orthographic projection.");
    }
    if (!std::isfinite(input.vertical_fov_degrees) || input.vertical_fov_degrees <= 0.0F ||
        input.vertical_fov_degrees >= 179.0F) {
        return failure_plan(input, output_mode, "vertical-fov-invalid",
                            "The vertical field of view must be finite and strictly between zero and 179 degrees.");
    }
    if (!std::isfinite(input.orthographic_height) || input.orthographic_height <= 0.0F ||
        input.orthographic_height > native_raytracing_view_max_world_coordinate) {
        return failure_plan(input, output_mode, "orthographic-height-invalid",
                            "The orthographic height must be finite, positive and bounded.");
    }
    if (!std::isfinite(input.aspect) || input.aspect <= 0.0F ||
        input.aspect > native_raytracing_view_max_aspect) {
        return failure_plan(input, output_mode, "aspect-invalid",
                            "The output aspect ratio must be finite, positive and bounded.");
    }
    if (!std::isfinite(input.near_clip) || !std::isfinite(input.far_clip) ||
        input.near_clip <= 0.0F || input.far_clip <= input.near_clip ||
        input.far_clip > native_raytracing_view_max_clip_distance) {
        return failure_plan(input, output_mode, "clip-range-invalid",
                            "The camera clip range must be finite, positive, ordered and bounded.");
    }
    if (input.output_width == 0U || input.output_height == 0U ||
        input.output_width > native_raytracing_view_max_extent ||
        input.output_height > native_raytracing_view_max_extent) {
        return failure_plan(input, output_mode, "output-extent-invalid",
                            "The native ray-tracing view extent is outside its bounded 2D surface range.");
    }

    auto result = initial_plan(input, output_mode);
    const auto forward = normalize(input.forward);
    const auto right_unormalized = cross(forward, input.up);
    if (!finite(right_unormalized) || magnitude(right_unormalized) <= kBasisEpsilon) {
        return failure_plan(input, output_mode, "camera-basis-degenerate",
                            "The camera forward and up vectors are parallel and cannot form a view basis.");
    }
    const auto right = normalize(right_unormalized);
    const auto up = normalize(cross(right, forward));
    if (!finite(forward) || !finite(right) || !finite(up) ||
        magnitude(up) <= kBasisEpsilon) {
        return failure_plan(input, output_mode, "camera-basis-degenerate",
                            "The camera basis could not be normalized into three finite axes.");
    }
    result.basis = NativeRayTracingViewBasis{input.position, forward, right, up};
    result.basis_orthonormal = orthonormal(result.basis);
    if (!result.basis_orthonormal) {
        return failure_plan(input, output_mode, "camera-basis-non-orthonormal",
                            "The normalized camera basis failed the orthogonality tolerance.");
    }

    if (input.projection == "perspective") {
        const float tangent = std::tan(input.vertical_fov_degrees * kDegreesToRadians * 0.5F);
        if (!std::isfinite(tangent) || tangent <= 0.0F) {
            return failure_plan(input, output_mode, "perspective-parameters-invalid",
                                "The perspective field of view produced a non-finite ray scale.");
        }
        result.primary_ray_parameters.tan_half_fov_y = tangent;
        result.primary_ray_parameters.tan_half_fov_x = tangent * input.aspect;
    }
    if (input.projection == "orthographic") {
        result.primary_ray_parameters.orthographic_half_height = input.orthographic_height * 0.5F;
        result.primary_ray_parameters.orthographic_half_width =
            result.primary_ray_parameters.orthographic_half_height * input.aspect;
    }
    if (!finite(result.primary_ray_parameters)) {
        return failure_plan(input, output_mode, "projection-parameters-invalid",
                            "The native ray-tracing projection parameters are non-finite.");
    }

    result.primary_ray_fingerprint = view_fingerprint(result);
    if (output_mode == NativeRayTracingViewOutputMode::future_linear_radiance) {
        result.code = "future-linear-radiance-not-implemented";
        result.detail =
            "The 0.1 view contract exposes diagnostic hit-mask rays; a linear radiance producer is reserved for a later version and remains on fallback.";
        result.fallback_active = true;
        return result;
    }

    result.valid = true;
    result.supported = true;
    result.diagnostic_hit_mask = true;
    result.future_linear_radiance_planned = true;
    result.linear_radiance_implemented = false;
    result.claims_linear_radiance = false;
    result.fallback_active = false;
    result.code = "diagnostic-hit-mask-ready";
    result.detail =
        "The renderer-neutral camera view can produce deterministic primary rays for the native diagnostic hit-mask path; no linear-radiance claim is made.";
    return result;
}

NativeRayTracingPrimaryRayResult make_native_raytracing_primary_ray(
    const NativeRayTracingViewPlan& plan,
    const std::uint32_t pixel_x,
    const std::uint32_t pixel_y) {
    if (!plan.valid || !plan.supported || !plan.diagnostic_hit_mask ||
        plan.output_mode != NativeRayTracingViewOutputMode::diagnostic_hit_mask) {
        return unavailable_ray("view-plan-unavailable",
                               "A valid diagnostic hit-mask view plan is required before deriving a primary ray.");
    }
    if (plan.output_width == 0U || plan.output_height == 0U ||
        pixel_x >= plan.output_width || pixel_y >= plan.output_height) {
        return unavailable_ray("pixel-out-of-range",
                               "The requested primary-ray pixel lies outside the view extent.");
    }
    if (plan.schema != native_raytracing_view_schema ||
        plan.output_contract != native_raytracing_diagnostic_hit_mask_contract ||
        plan.camera_id.empty() || plan.camera_revision == 0U ||
        plan.output_width > native_raytracing_view_max_extent ||
        plan.output_height > native_raytracing_view_max_extent ||
        !orthonormal(plan.basis) || !finite(plan.primary_ray_parameters) ||
        !std::isfinite(plan.vertical_fov_degrees) || plan.vertical_fov_degrees <= 0.0F ||
        plan.vertical_fov_degrees >= 179.0F ||
        !std::isfinite(plan.orthographic_height) || plan.orthographic_height <= 0.0F ||
        !std::isfinite(plan.aspect) || plan.aspect <= 0.0F ||
        !std::isfinite(plan.near_clip) || !std::isfinite(plan.far_clip) ||
        plan.near_clip <= 0.0F || plan.far_clip <= plan.near_clip ||
        (plan.projection != "perspective" && plan.projection != "orthographic") ||
        (plan.projection == "perspective" &&
         (plan.primary_ray_parameters.tan_half_fov_y <= 0.0F ||
          plan.primary_ray_parameters.tan_half_fov_x <= 0.0F)) ||
        (plan.projection == "orthographic" &&
         (plan.primary_ray_parameters.orthographic_half_height <= 0.0F ||
          plan.primary_ray_parameters.orthographic_half_width <= 0.0F))) {
        return unavailable_ray("view-plan-invalid",
                               "The view plan contains invalid camera basis or projection values.");
    }

    NativeRayTracingPrimaryRayResult result;
    result.ray.pixel_x = pixel_x;
    result.ray.pixel_y = pixel_y;
    result.ray.sample_u = (static_cast<float>(pixel_x) + 0.5F) /
                          static_cast<float>(plan.output_width);
    result.ray.sample_v = (static_cast<float>(pixel_y) + 0.5F) /
                          static_cast<float>(plan.output_height);
    const float ndc_x = result.ray.sample_u * 2.0F - 1.0F;
    const float ndc_y = 1.0F - result.ray.sample_v * 2.0F;

    if (plan.projection == "perspective") {
        const auto horizontal = scale(plan.basis.right,
                                      ndc_x * plan.primary_ray_parameters.tan_half_fov_x);
        const auto vertical = scale(plan.basis.up,
                                    ndc_y * plan.primary_ray_parameters.tan_half_fov_y);
        result.ray.origin = plan.basis.position;
        result.ray.direction = normalize(add(add(plan.basis.forward, horizontal), vertical));
    } else {
        const auto horizontal = scale(
            plan.basis.right, ndc_x * plan.primary_ray_parameters.orthographic_half_width);
        const auto vertical = scale(
            plan.basis.up, ndc_y * plan.primary_ray_parameters.orthographic_half_height);
        result.ray.origin = add(add(plan.basis.position, horizontal), vertical);
        result.ray.direction = plan.basis.forward;
    }
    result.ray.minimum_distance = plan.near_clip;
    result.ray.maximum_distance = plan.far_clip;

    if (!finite(result.ray.origin) || !finite(result.ray.direction) ||
        magnitude(result.ray.direction) <= kBasisEpsilon ||
        !std::isfinite(result.ray.sample_u) || !std::isfinite(result.ray.sample_v)) {
        return unavailable_ray("primary-ray-non-finite",
                               "The requested primary-ray derivation produced a non-finite result.");
    }
    result.ray.direction = normalize(result.ray.direction);
    result.fingerprint = pixel_fingerprint(plan, result.ray);
    result.valid = true;
    result.code = "ok";
    result.detail =
        "Primary ray derived from the versioned pixel-center convention and normalized camera basis.";
    return result;
}

} // namespace noemancer
