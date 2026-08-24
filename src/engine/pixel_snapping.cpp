#include "engine/pixel_snapping.hpp"

#include <cmath>

namespace noemancer {
namespace {

[[nodiscard]] bool snap_scalar(const double value, const double world_units,
                               double& snapped) noexcept {
    if (!std::isfinite(value) || !std::isfinite(world_units) || world_units <= 0.0) {
        snapped = 0.0;
        return false;
    }

    const double grid_coordinate = value / world_units;
    if (!std::isfinite(grid_coordinate)) {
        snapped = 0.0;
        return false;
    }

    // std::round is specified as halfway away from zero and is independent
    // of the current floating-point rounding mode.  That makes both +/-0.5
    // ties deterministic across supported platforms.
    const double rounded_coordinate = std::round(grid_coordinate);
    const double candidate = rounded_coordinate * world_units;
    if (!std::isfinite(candidate)) {
        snapped = 0.0;
        return false;
    }

    // Avoid exposing a negative zero as a semantically different grid point
    // to serialized/observed callers.
    snapped = candidate == 0.0 ? 0.0 : candidate;
    return true;
}

[[nodiscard]] bool add_finite(const double left, const double right,
                              double& result) noexcept {
    result = left + right;
    return std::isfinite(result);
}

} // namespace

bool is_finite_pixel_grid_vector(const PixelGridVector3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool is_valid_pixel_grid(const double pixels_per_unit) noexcept {
    if (!std::isfinite(pixels_per_unit) || pixels_per_unit <= 0.0) return false;
    const double world_units = 1.0 / pixels_per_unit;
    return std::isfinite(world_units) && world_units > 0.0;
}

bool is_valid_pixel_grid_profile(const PixelGridProfile& profile) noexcept {
    return is_valid_pixel_grid(profile.pixels_per_unit) &&
           std::isfinite(profile.virtual_width) && profile.virtual_width > 0.0 &&
           std::isfinite(profile.virtual_height) && profile.virtual_height > 0.0;
}

PixelGridMetrics derive_pixel_grid_metrics(const double virtual_height,
                                           const double pixels_per_unit) noexcept {
    PixelGridMetrics result{};
    if (!std::isfinite(virtual_height) || virtual_height <= 0.0 ||
        !is_valid_pixel_grid(pixels_per_unit)) {
        return result;
    }

    result.world_units_per_pixel = world_units_per_pixel(pixels_per_unit);
    result.orthographic_height = virtual_height * result.world_units_per_pixel;
    if (!std::isfinite(result.orthographic_height) || result.orthographic_height <= 0.0) {
        return {};
    }
    result.valid = true;
    return result;
}

PixelGridMetrics derive_pixel_grid_metrics(const PixelGridProfile& profile) noexcept {
    if (!is_valid_pixel_grid_profile(profile)) return {};
    return derive_pixel_grid_metrics(profile.virtual_height, profile.pixels_per_unit);
}

double world_units_per_pixel(const double pixels_per_unit) noexcept {
    if (!is_valid_pixel_grid(pixels_per_unit)) return 0.0;
    return 1.0 / pixels_per_unit;
}

double orthographic_height_from_virtual_height(const double virtual_height,
                                               const double pixels_per_unit) noexcept {
    const auto metrics = derive_pixel_grid_metrics(virtual_height, pixels_per_unit);
    return metrics.valid ? metrics.orthographic_height : 0.0;
}

PixelGridCameraSnap snap_pixel_camera(const PixelGridCameraInput authored,
                                      const double pixels_per_unit) noexcept {
    PixelGridCameraSnap result{};
    if (!is_finite_pixel_grid_vector(authored.position) || !is_finite_pixel_grid_vector(authored.target) ||
        !is_valid_pixel_grid(pixels_per_unit)) {
        return result;
    }

    const double world_units = world_units_per_pixel(pixels_per_unit);
    if (!snap_scalar(authored.position.x, world_units, result.position.x) ||
        !snap_scalar(authored.position.y, world_units, result.position.y)) {
        return {};
    }
    result.position.z = authored.position.z;
    result.translation = {
        result.position.x - authored.position.x,
        result.position.y - authored.position.y,
        0.0,
    };
    if (!is_finite_pixel_grid_vector(result.translation)) return {};

    if (!add_finite(authored.target.x, result.translation.x, result.target.x) ||
        !add_finite(authored.target.y, result.translation.y, result.target.y)) {
        return {};
    }
    result.target.z = authored.target.z;
    if (!is_finite_pixel_grid_vector(result.position) || !is_finite_pixel_grid_vector(result.target)) return {};
    result.valid = true;
    return result;
}

PixelGridCameraSnap snap_pixel_camera(const PixelGridCameraInput authored,
                                      const PixelGridProfile& profile) noexcept {
    if (!is_valid_pixel_grid_profile(profile)) return {};
    return snap_pixel_camera(authored, profile.pixels_per_unit);
}

PixelGridSpriteSnap snap_pixel_sprite(const PixelGridVector3 authored_position,
                                      const PixelGridVector3 authored_camera_position,
                                      const PixelGridVector3 snapped_camera_position,
                                      const double pixels_per_unit) noexcept {
    PixelGridSpriteSnap result{};
    if (!is_finite_pixel_grid_vector(authored_position) || !is_finite_pixel_grid_vector(authored_camera_position) ||
        !is_finite_pixel_grid_vector(snapped_camera_position) || !is_valid_pixel_grid(pixels_per_unit)) {
        return result;
    }

    const double world_units = world_units_per_pixel(pixels_per_unit);
    const double relative_x = authored_position.x - authored_camera_position.x;
    const double relative_y = authored_position.y - authored_camera_position.y;
    if (!std::isfinite(relative_x) || !std::isfinite(relative_y) ||
        !snap_scalar(relative_x, world_units, result.position.x) ||
        !snap_scalar(relative_y, world_units, result.position.y)) {
        return {};
    }
    if (!add_finite(snapped_camera_position.x, result.position.x, result.position.x) ||
        !add_finite(snapped_camera_position.y, result.position.y, result.position.y)) {
        return {};
    }
    result.position.z = authored_position.z;
    if (!is_finite_pixel_grid_vector(result.position)) return {};
    result.valid = true;
    return result;
}

PixelGridSpriteSnap snap_pixel_sprite(const PixelGridVector3 authored_position,
                                      const PixelGridVector3 authored_camera_position,
                                      const PixelGridVector3 snapped_camera_position,
                                      const PixelGridProfile& profile) noexcept {
    if (!is_valid_pixel_grid_profile(profile)) return {};
    return snap_pixel_sprite(authored_position, authored_camera_position,
                             snapped_camera_position, profile.pixels_per_unit);
}

} // namespace noemancer
