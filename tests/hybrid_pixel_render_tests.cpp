#include "engine/hybrid_pixel_render.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>

namespace {

bool near(const float left, const double right) {
    return std::abs(static_cast<double>(left) - right) <= 1.0e-6;
}

bool near(const double left, const double right) {
    return std::abs(left - right) <= 1.0e-12;
}

noemancer::RenderWorldSnapshot make_authored_world() {
    noemancer::RenderWorldSnapshot world;
    world.camera = noemancer::RenderCameraSnapshot{
        .entity_id = "camera.main",
        .position = {1.03125F, -2.03125F, 10.0F},
        .target = {1.03125F, -1.03125F, 0.0F},
        .projection = "orthographic",
        .orthographic_height = 99.0F};
    world.sprites.push_back(noemancer::RenderSpriteSnapshot{
        .entity_id = "sprite.hero", .position = {1.1F, -1.95F, 3.0F}});
    world.tile_cells.push_back(noemancer::RenderTileCellSnapshot{
        .stable_id = "tile.0", .position = {-0.03125F, 0.03125F, 1.0F}});
    return world;
}

bool same_position(const std::array<float, 3>& left,
                   const std::array<float, 3>& right) {
    return left == right;
}

bool same_projected_world(const noemancer::RenderWorldSnapshot& left,
                          const noemancer::RenderWorldSnapshot& right) {
    if (left.camera.has_value() != right.camera.has_value() ||
        left.sprites.size() != right.sprites.size() ||
        left.tile_cells.size() != right.tile_cells.size()) {
        return false;
    }
    if (left.camera) {
        if (!same_position(left.camera->position, right.camera->position) ||
            !same_position(left.camera->target, right.camera->target) ||
            left.camera->projection != right.camera->projection ||
            left.camera->orthographic_height != right.camera->orthographic_height) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.sprites.size(); ++index) {
        if (!same_position(left.sprites[index].position, right.sprites[index].position)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.tile_cells.size(); ++index) {
        if (!same_position(left.tile_cells[index].position, right.tile_cells[index].position)) {
            return false;
        }
    }
    return true;
}

bool same_report(const noemancer::HybridPixelRenderProjection& left,
                 const noemancer::HybridPixelRenderProjection& right) {
    return left.enabled == right.enabled && left.valid == right.valid &&
           left.orthographic_camera == right.orthographic_camera &&
           left.camera_snapped == right.camera_snapped &&
           left.sprites_snapped == right.sprites_snapped &&
           left.tile_cells_snapped == right.tile_cells_snapped &&
           near(left.world_units_per_pixel, right.world_units_per_pixel) &&
           near(left.orthographic_height, right.orthographic_height) &&
           left.code == right.code && left.detail == right.detail;
}

int fail(const char* detail) {
    std::cerr << "hybrid_pixel_render_tests: " << detail << '\n';
    return 1;
}

} // namespace

int main() {
    using namespace noemancer;
    HybridPixelProfile profile;
    profile.virtual_width = 320;
    profile.virtual_height = 180;
    profile.pixels_per_unit = 16.0F;

    // RenderWorldSnapshot is the per-frame projection buffer and is allowed
    // to receive the snapped result.  The authored/canonical snapshot must
    // stay untouched, and equal authored inputs must produce equal output.
    const auto canonical_world = make_authored_world();
    auto first_world = canonical_world;
    auto second_world = canonical_world;
    const auto first_report = apply_hybrid_pixel_render_projection(first_world, profile);
    const auto second_report = apply_hybrid_pixel_render_projection(second_world, profile);
    if (!first_report.valid || !first_report.enabled || !first_report.orthographic_camera ||
        !first_report.camera_snapped || first_report.sprites_snapped != 1U ||
        first_report.tile_cells_snapped != 1U || first_report.code != "ok" ||
        !same_report(first_report, second_report) ||
        !same_projected_world(first_world, second_world) ||
        !same_projected_world(canonical_world, make_authored_world()) ||
        !near(first_world.camera->orthographic_height, 11.25) ||
        !near(first_world.camera->position[0], 1.0625) ||
        !near(first_world.camera->position[1], -2.0625) ||
        !near(first_world.camera->target[0] - first_world.camera->position[0], 0.0) ||
        !near(first_world.camera->target[1] - first_world.camera->position[1], 1.0) ||
        !near(first_world.sprites[0].position[0], 1.125) ||
        !near(first_world.sprites[0].position[1], -2.0) ||
        !near(first_world.sprites[0].position[2], 3.0) ||
        !near(first_world.tile_cells[0].position[0], 0.0) ||
        !near(first_world.tile_cells[0].position[1], 0.0)) {
        return fail("valid orthographic projection drifted");
    }

    auto perspective = canonical_world;
    perspective.camera->projection = "perspective";
    const auto rejected = apply_hybrid_pixel_render_projection(perspective, profile);
    if (rejected.valid || rejected.code != "hybrid-pixel.camera-not-orthographic" ||
        !same_projected_world(perspective, [&]() {
            auto expected = canonical_world;
            expected.camera->projection = "perspective";
            return expected;
        }())) {
        return fail("perspective camera did not produce an explicit profile diagnostic");
    }

    profile.enabled = false;
    auto disabled_world = canonical_world;
    const auto disabled = apply_hybrid_pixel_render_projection(disabled_world, profile);
    if (disabled.enabled || disabled.valid || disabled.code != "hybrid-pixel.disabled" ||
        !same_projected_world(disabled_world, canonical_world)) {
        return fail("disabled profile was applied");
    }
    return 0;
}
