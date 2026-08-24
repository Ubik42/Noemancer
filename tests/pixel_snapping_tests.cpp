#include "engine/pixel_snapping.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

bool near(const double left, const double right, const double epsilon = 1.0e-12) {
    return std::abs(left - right) <= epsilon;
}

bool near(const noemancer::PixelGridVector3 left,
          const noemancer::PixelGridVector3 right,
          const double epsilon = 1.0e-12) {
    return near(left.x, right.x, epsilon) && near(left.y, right.y, epsilon) &&
           near(left.z, right.z, epsilon);
}

} // namespace

int main() {
    using namespace noemancer;
    constexpr double ppu = 16.0;
    constexpr double world_pixel = 1.0 / ppu;

    const auto metrics = derive_pixel_grid_metrics(180.0, ppu);
    if (!metrics.valid || !near(metrics.world_units_per_pixel, world_pixel) ||
        !near(metrics.orthographic_height, 11.25)) {
        std::cerr << "virtual resolution did not derive stable world units and orthographic height\n";
        return 1;
    }
    const PixelGridProfile profile{ppu, 320.0, 180.0};
    const auto profile_metrics = derive_pixel_grid_metrics(profile);
    if (!profile_metrics.valid || !near(profile_metrics.orthographic_height, metrics.orthographic_height) ||
        !is_valid_pixel_grid_profile(profile)) {
        std::cerr << "valid pixel-grid profile was rejected\n";
        return 2;
    }

    // Half ties are deliberately round-half-away-from-zero, including both
    // signs.  These points are exactly representable for this grid.
    const auto positive_half = snap_pixel_sprite({0.03125, 0.0, 3.0}, {}, {}, ppu);
    const auto negative_half = snap_pixel_sprite({-0.03125, 0.0, 3.0}, {}, {}, ppu);
    const auto positive_one_half = snap_pixel_sprite({0.09375, 0.0, 3.0}, {}, {}, ppu);
    const auto negative_one_half = snap_pixel_sprite({-0.09375, 0.0, 3.0}, {}, {}, ppu);
    if (!positive_half.valid || !negative_half.valid || !positive_one_half.valid ||
        !negative_one_half.valid || !near(positive_half.position.x, 0.0625) ||
        !near(negative_half.position.x, -0.0625) ||
        !near(positive_one_half.position.x, 0.125) ||
        !near(negative_one_half.position.x, -0.125)) {
        std::cerr << "pixel-grid half-tie rounding is not deterministic\n";
        return 3;
    }
    const auto negative_relative = snap_pixel_sprite(
        {-0.03125, 0.03125, 7.0}, {}, {}, ppu);
    if (!negative_relative.valid || !near(negative_relative.position.x, -0.0625) ||
        !near(negative_relative.position.y, 0.0625) ||
        !near(negative_relative.position.z, 7.0)) {
        std::cerr << "camera-relative negative half ties were not snapped symmetrically\n";
        return 11;
    }

    const PixelGridCameraInput authored_camera{
        {1.03125, -2.03125, 10.0},
        {1.03125, -1.03125, 0.0},
    };
    const auto camera = snap_pixel_camera(authored_camera, ppu);
    if (!camera.valid || !near(camera.position.x, 1.0625) ||
        !near(camera.position.y, -2.0625) || !near(camera.position.z, 10.0) ||
        !near(camera.target.x, 1.0625) || !near(camera.target.y, -1.0625) ||
        !near(camera.target.z, 0.0) || !near(camera.translation.x, 0.03125) ||
        !near(camera.translation.y, -0.03125) || !near(camera.translation.z, 0.0)) {
        std::cerr << "camera position was not snapped to the expected XY grid\n";
        return 4;
    }
    const PixelGridVector3 authored_delta{
        authored_camera.target.x - authored_camera.position.x,
        authored_camera.target.y - authored_camera.position.y,
        authored_camera.target.z - authored_camera.position.z,
    };
    const PixelGridVector3 snapped_delta{
        camera.target.x - camera.position.x,
        camera.target.y - camera.position.y,
        camera.target.z - camera.position.z,
    };
    if (!near(authored_delta.x, snapped_delta.x) || !near(authored_delta.y, snapped_delta.y) ||
        !near(authored_delta.z, snapped_delta.z)) {
        std::cerr << "camera target did not receive the same translation as position\n";
        return 5;
    }

    const PixelGridVector3 authored_sprite{1.1, -1.95, 4.5};
    const auto sprite = snap_pixel_sprite(authored_sprite, authored_camera.position,
                                           camera.position, ppu);
    if (!sprite.valid || !near(sprite.position.x, 1.125) ||
        !near(sprite.position.y, -2.0) || !near(sprite.position.z, authored_sprite.z)) {
        std::cerr << "camera-relative sprite snapping did not preserve authored Z\n";
        return 6;
    }

    // The API is authored-input based: repeating a frame with the same
    // authored values is exactly idempotent and cannot accumulate a prior snap.
    const auto camera_again = snap_pixel_camera(authored_camera, ppu);
    const auto sprite_again = snap_pixel_sprite(authored_sprite, authored_camera.position,
                                                 camera.position, ppu);
    if (!near(camera.position, camera_again.position, 0.0) ||
        !near(camera.target, camera_again.target, 0.0) ||
        !near(sprite.position, sprite_again.position, 0.0)) {
        std::cerr << "pixel snapping is not idempotent for authored inputs\n";
        return 7;
    }

    // Physical resize is intentionally absent from the derivation: changing
    // the virtual width (the aspect/letterbox authority) does not change the
    // world-space height derived from the same virtual height.
    const auto resized_profile_metrics = derive_pixel_grid_metrics(PixelGridProfile{ppu, 640.0, 180.0});
    if (!resized_profile_metrics.valid ||
        !near(resized_profile_metrics.orthographic_height, profile_metrics.orthographic_height, 0.0)) {
        std::cerr << "virtual profile changed under resize-equivalent derivation\n";
        return 8;
    }
    const PixelGridProfile resized_profile{ppu, 640.0, 180.0};
    const auto resized_camera = snap_pixel_camera(authored_camera, resized_profile);
    const auto resized_sprite = snap_pixel_sprite(authored_sprite, authored_camera.position,
                                                   resized_camera.position, resized_profile);
    if (!resized_camera.valid || !resized_sprite.valid ||
        !near(camera.position, resized_camera.position, 0.0) ||
        !near(camera.target, resized_camera.target, 0.0) ||
        !near(sprite.position, resized_sprite.position, 0.0)) {
        std::cerr << "pixel snapping changed when only the virtual width was resized\n";
        return 12;
    }

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    if (is_valid_pixel_grid(0.0) || is_valid_pixel_grid(-ppu) ||
        is_valid_pixel_grid(nan) || is_valid_pixel_grid(inf) ||
        derive_pixel_grid_metrics(180.0, 0.0).valid ||
        derive_pixel_grid_metrics(nan, ppu).valid ||
        orthographic_height_from_virtual_height(180.0, 0.0) != 0.0 ||
        is_valid_pixel_grid_profile({ppu, 320.0, 0.0}) ||
        is_valid_pixel_grid_profile({ppu, nan, 180.0}) ||
        !is_finite_pixel_grid_vector({1.0, 2.0, 3.0})) {
        std::cerr << "invalid pixel-grid inputs were accepted\n";
        return 9;
    }
    if (snap_pixel_camera({{nan, 0.0, 0.0}, {0.0, 0.0, 0.0}}, ppu).valid ||
        snap_pixel_camera({{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}, 0.0).valid ||
        snap_pixel_sprite({0.0, 0.0, nan}, {}, {}, ppu).valid ||
        snap_pixel_sprite({0.0, 0.0, 0.0}, {inf, 0.0, 0.0}, {}, ppu).valid) {
        std::cerr << "invalid camera or sprite inputs were accepted\n";
        return 10;
    }
    return 0;
}
