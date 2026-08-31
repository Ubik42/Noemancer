#include "engine/native_raytracing_view.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "native_raytracing_view_tests: " << message << '\n';
    }
    return condition;
}

bool near(const float left, const float right, const float tolerance = 1.0e-5F) {
    return std::abs(left - right) <= tolerance;
}

float dot(const NativeRayTracingViewVec3& left,
          const NativeRayTracingViewVec3& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

float length(const NativeRayTracingViewVec3& value) {
    return std::sqrt(dot(value, value));
}

bool near_vec(const NativeRayTracingViewVec3& left,
              const NativeRayTracingViewVec3& right) {
    return near(left[0], right[0]) && near(left[1], right[1]) &&
           near(left[2], right[2]);
}

NativeRayTracingViewInput perspective_input() {
    NativeRayTracingViewInput input;
    input.camera_id = "camera.main";
    input.camera_revision = 17U;
    input.position = {0.0F, 2.0F, -5.0F};
    input.forward = {0.0F, -0.1F, 1.0F};
    input.up = {0.0F, 1.0F, 0.0F};
    input.projection = "perspective";
    input.vertical_fov_degrees = 60.0F;
    input.orthographic_height = 10.0F;
    input.aspect = 16.0F / 9.0F;
    input.near_clip = 0.1F;
    input.far_clip = 500.0F;
    input.output_width = 1280U;
    input.output_height = 720U;
    return input;
}

} // namespace

int main() {
    const auto input = perspective_input();
    const auto plan = build_native_raytracing_view_plan(input);
    if (!check(plan.valid && plan.supported && plan.diagnostic_hit_mask &&
                   plan.future_linear_radiance_planned &&
                   !plan.linear_radiance_implemented && !plan.claims_linear_radiance &&
                   !plan.fallback_active && plan.basis_orthonormal &&
                   plan.output_contract == native_raytracing_diagnostic_hit_mask_contract &&
                   plan.output_format == native_raytracing_diagnostic_output_format &&
                   plan.output_width == input.output_width &&
                   plan.output_height == input.output_height &&
                   plan.camera_id == input.camera_id &&
                   plan.camera_revision == input.camera_revision &&
                   plan.primary_ray_fingerprint != 0U,
               "valid RenderWorld camera values did not produce a diagnostic view plan")) {
        return 1;
    }
    if (!check(near(length(plan.basis.forward), 1.0F) &&
                   near(length(plan.basis.right), 1.0F) &&
                   near(length(plan.basis.up), 1.0F) &&
                   near(dot(plan.basis.forward, plan.basis.right), 0.0F) &&
                   near(dot(plan.basis.forward, plan.basis.up), 0.0F) &&
                   near(dot(plan.basis.right, plan.basis.up), 0.0F),
               "camera basis is not normalized and orthogonal")) {
        return 2;
    }

    const auto repeat = build_native_raytracing_view_plan(input);
    if (!check(repeat.primary_ray_fingerprint == plan.primary_ray_fingerprint,
               "identical camera inputs changed the deterministic view fingerprint")) {
        return 3;
    }
    auto changed_revision = input;
    ++changed_revision.camera_revision;
    const auto changed_plan = build_native_raytracing_view_plan(changed_revision);
    if (!check(changed_plan.valid &&
                   changed_plan.primary_ray_fingerprint != plan.primary_ray_fingerprint,
               "camera revision did not participate in the primary-ray fingerprint")) {
        return 4;
    }

    const auto center = make_native_raytracing_primary_ray(plan, 640U, 360U);
    if (!check(center.valid && center.code == "ok" && center.ray.pixel_x == 640U &&
                   center.ray.pixel_y == 360U && near(center.ray.sample_u, 0.50039065F) &&
                   near(center.ray.sample_v, 0.50069445F) &&
                   near(center.ray.origin[0], input.position[0]) &&
                   near(center.ray.origin[1], input.position[1]) &&
                   near(center.ray.origin[2], input.position[2]) &&
                   near(length(center.ray.direction), 1.0F) &&
                   center.ray.minimum_distance == input.near_clip &&
                   center.ray.maximum_distance == input.far_clip &&
                   center.fingerprint != 0U,
               "perspective center pixel did not produce a bounded primary ray")) {
        return 5;
    }
    const auto center_repeat = make_native_raytracing_primary_ray(plan, 640U, 360U);
    if (!check(center_repeat.valid && center_repeat.fingerprint == center.fingerprint &&
                   center_repeat.ray.origin == center.ray.origin &&
                   center_repeat.ray.direction == center.ray.direction,
               "repeating one pixel did not produce the same ray and fingerprint")) {
        return 6;
    }
    const auto corner = make_native_raytracing_primary_ray(plan, 0U, 0U);
    if (!check(corner.valid && corner.ray.sample_u > 0.0F && corner.ray.sample_v > 0.0F &&
                   corner.ray.direction[1] > center.ray.direction[1],
               "top-left perspective pixel did not use top-left pixel-center sampling")) {
        return 7;
    }
    if (!check(!make_native_raytracing_primary_ray(plan, input.output_width, 0U).valid &&
                   make_native_raytracing_primary_ray(plan, input.output_width, 0U).code ==
                       "pixel-out-of-range" &&
                   !make_native_raytracing_primary_ray(plan, 0U, input.output_height).valid,
               "out-of-bounds pixels were accepted")) {
        return 8;
    }

    auto orthographic_input = input;
    orthographic_input.projection = "orthographic";
    orthographic_input.orthographic_height = 8.0F;
    const auto orthographic_plan = build_native_raytracing_view_plan(orthographic_input);
    const auto orthographic_center =
        make_native_raytracing_primary_ray(orthographic_plan, 640U, 360U);
    if (!check(orthographic_plan.valid && orthographic_plan.supported &&
                   orthographic_center.valid &&
                   near_vec(orthographic_center.ray.direction,
                            orthographic_plan.basis.forward) &&
                   orthographic_center.ray.origin != orthographic_plan.basis.position &&
                   near(length(orthographic_center.ray.direction), 1.0F),
               "orthographic view did not produce an offset origin with parallel rays")) {
        return 9;
    }

    const auto future = build_native_raytracing_view_plan(
        input, NativeRayTracingViewOutputMode::future_linear_radiance);
    if (!check(!future.valid && !future.supported && future.fallback_active &&
                   !future.diagnostic_hit_mask && future.future_linear_radiance_planned &&
                   !future.linear_radiance_implemented && !future.claims_linear_radiance &&
                   future.output_contract == native_raytracing_future_linear_radiance_contract &&
                   future.code == "future-linear-radiance-not-implemented" &&
                   native_raytracing_view_output_mode_name(
                       NativeRayTracingViewOutputMode::future_linear_radiance) ==
                       "future-linear-radiance",
               "future linear-radiance output was presented as an implemented producer")) {
        return 10;
    }
    if (!check(!make_native_raytracing_primary_ray(future, 0U, 0U).valid,
               "a future linear-radiance plan produced a diagnostic primary ray")) {
        return 11;
    }

    auto parallel_basis = input;
    parallel_basis.up = input.forward;
    const auto degenerate = build_native_raytracing_view_plan(parallel_basis);
    if (!check(!degenerate.valid && degenerate.code == "camera-basis-degenerate",
               "parallel forward/up vectors were not rejected")) {
        return 12;
    }
    auto bad_fov = input;
    bad_fov.vertical_fov_degrees = 179.0F;
    if (!check(!build_native_raytracing_view_plan(bad_fov).valid &&
                   build_native_raytracing_view_plan(bad_fov).code == "vertical-fov-invalid",
               "boundary field of view was accepted")) {
        return 13;
    }
    auto bad_extent = input;
    bad_extent.output_width = native_raytracing_view_max_extent + 1U;
    if (!check(!build_native_raytracing_view_plan(bad_extent).valid &&
                   build_native_raytracing_view_plan(bad_extent).code == "output-extent-invalid",
               "out-of-budget extent was accepted")) {
        return 14;
    }
    auto bad_finite = input;
    bad_finite.position[1] = std::numeric_limits<float>::quiet_NaN();
    if (!check(!build_native_raytracing_view_plan(bad_finite).valid &&
                   build_native_raytracing_view_plan(bad_finite).code == "camera-position-invalid",
               "non-finite camera position was accepted")) {
        return 15;
    }
    auto bad_id = input;
    bad_id.camera_id = "   ";
    if (!check(!build_native_raytracing_view_plan(bad_id).valid &&
                   build_native_raytracing_view_plan(bad_id).code == "camera-id-invalid",
               "blank stable camera id was accepted")) {
        return 16;
    }

    std::cout << "native_raytracing_view_tests: ok\n";
    return 0;
}
