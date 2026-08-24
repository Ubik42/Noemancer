#include "engine/hybrid_pixel_render.hpp"

#include "engine/pixel_snapping.hpp"

#include <array>

namespace noemancer {
namespace {

PixelGridVector3 vector_from(const std::array<float, 3>& value) {
    return {value[0], value[1], value[2]};
}

void assign(std::array<float, 3>& destination, const PixelGridVector3 value) {
    destination = {
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z)};
}

template <typename Snapshot>
bool snap_position(Snapshot& snapshot,
                   const PixelGridVector3 authored_camera,
                   const PixelGridVector3 snapped_camera,
                   const PixelGridProfile& grid) {
    const auto snapped = snap_pixel_sprite(
        vector_from(snapshot.position), authored_camera, snapped_camera, grid);
    if (!snapped.valid) return false;
    assign(snapshot.position, snapped.position);
    return true;
}

} // namespace

HybridPixelRenderProjection apply_hybrid_pixel_render_projection(
    RenderWorldSnapshot& render_world,
    const HybridPixelProfile& profile) {
    HybridPixelRenderProjection report;
    report.enabled = profile.enabled;
    if (!profile.enabled) return report;

    if (!HybridPixelProfileCodec::validate(profile).empty()) {
        report.code = "hybrid-pixel.profile-invalid";
        report.detail = "The Hybrid Pixel profile failed validation.";
        return report;
    }

    const PixelGridProfile grid{
        static_cast<double>(profile.pixels_per_unit),
        static_cast<double>(profile.virtual_width),
        static_cast<double>(profile.virtual_height)};
    const auto metrics = derive_pixel_grid_metrics(grid);
    if (!metrics.valid) {
        report.code = "hybrid-pixel.grid-invalid";
        report.detail = "The Hybrid Pixel profile could not derive a finite pixel grid.";
        return report;
    }
    report.world_units_per_pixel = metrics.world_units_per_pixel;
    report.orthographic_height = metrics.orthographic_height;

    if (!render_world.camera) {
        report.code = "hybrid-pixel.camera-missing";
        report.detail = "Hybrid Pixel presentation is active, but the Render World has no camera to snap.";
        return report;
    }
    auto& camera = *render_world.camera;
    report.orthographic_camera = camera.projection == "orthographic";
    if (!report.orthographic_camera) {
        report.code = "hybrid-pixel.camera-not-orthographic";
        report.detail = "Hybrid Pixel presentation requires an orthographic camera for a stable world pixel grid.";
        return report;
    }

    const auto authored_camera = vector_from(camera.position);
    auto snapped_camera = authored_camera;
    if (profile.snap_camera) {
        const auto snapped = snap_pixel_camera(
            {authored_camera, vector_from(camera.target)}, grid);
        if (!snapped.valid) {
            report.code = "hybrid-pixel.camera-snap-invalid";
            report.detail = "The authored camera could not be projected onto the pixel grid.";
            return report;
        }
        assign(camera.position, snapped.position);
        assign(camera.target, snapped.target);
        snapped_camera = snapped.position;
        report.camera_snapped = true;
    }
    camera.orthographic_height = static_cast<float>(metrics.orthographic_height);

    if (profile.snap_sprites) {
        for (auto& sprite : render_world.sprites) {
            if (!snap_position(sprite, authored_camera, snapped_camera, grid)) {
                report.code = "hybrid-pixel.sprite-snap-invalid";
                report.detail = "A Sprite position could not be projected onto the pixel grid.";
                return report;
            }
            ++report.sprites_snapped;
        }
        for (auto& cell : render_world.tile_cells) {
            if (!snap_position(cell, authored_camera, snapped_camera, grid)) {
                report.code = "hybrid-pixel.tile-cell-snap-invalid";
                report.detail = "A Tile Cell position could not be projected onto the pixel grid.";
                return report;
            }
            ++report.tile_cells_snapped;
        }
    }

    report.valid = true;
    report.code = "ok";
    report.detail = "The extracted Render World uses the authoritative Hybrid Pixel grid.";
    return report;
}

} // namespace noemancer
