#pragma once

namespace noemancer {

// Pixel-grid math intentionally uses its own value type.  It is safe to use
// from Engine, Runtime, Editor and Agent projections without importing SDL,
// Flecs, a JSON library, or a renderer-owned type.
struct PixelGridVector3 final {
    double x{};
    double y{};
    double z{};
};

struct PixelGridProfile final {
    // Pixels per world unit.  A value of 16 means one virtual pixel is
    // 1/16 world units wide.
    double pixels_per_unit{};
    double virtual_width{};
    double virtual_height{};
};

struct PixelGridMetrics final {
    bool valid{};
    double world_units_per_pixel{};
    double orthographic_height{};
};

struct PixelGridCameraInput final {
    PixelGridVector3 position{};
    PixelGridVector3 target{};
};

struct PixelGridCameraSnap final {
    bool valid{};
    PixelGridVector3 position{};
    PixelGridVector3 target{};
    // The translation applied to both authored camera position and target.
    PixelGridVector3 translation{};
};

struct PixelGridSpriteSnap final {
    bool valid{};
    PixelGridVector3 position{};
};

[[nodiscard]] bool is_finite_pixel_grid_vector(PixelGridVector3 value) noexcept;
[[nodiscard]] bool is_valid_pixel_grid(double pixels_per_unit) noexcept;
[[nodiscard]] bool is_valid_pixel_grid_profile(const PixelGridProfile& profile) noexcept;

// The virtual resolution is intentionally independent of the physical
// window.  This is the source of truth for integer scaling/letterboxing code
// owned by the caller, and keeps resize handling from changing world units.
[[nodiscard]] PixelGridMetrics derive_pixel_grid_metrics(
    double virtual_height, double pixels_per_unit) noexcept;
[[nodiscard]] PixelGridMetrics derive_pixel_grid_metrics(
    const PixelGridProfile& profile) noexcept;
[[nodiscard]] double world_units_per_pixel(double pixels_per_unit) noexcept;
[[nodiscard]] double orthographic_height_from_virtual_height(
    double virtual_height, double pixels_per_unit) noexcept;

// Snap authored camera position in the XY pixel grid.  Target is translated
// by exactly the same delta, rather than rounded independently, so the camera
// view direction (including its Z component) is preserved.  Z is not part of
// the 2D pixel grid and is copied unchanged.
[[nodiscard]] PixelGridCameraSnap snap_pixel_camera(
    PixelGridCameraInput authored, double pixels_per_unit) noexcept;
[[nodiscard]] PixelGridCameraSnap snap_pixel_camera(
    PixelGridCameraInput authored, const PixelGridProfile& profile) noexcept;

// Snap an authored object/sprite in camera-relative XY.  The object and
// authored camera are deliberately separate from the snapped camera: callers
// must supply authored values every frame instead of feeding the previous
// result back into the next frame.  Object Z is copied unchanged.
[[nodiscard]] PixelGridSpriteSnap snap_pixel_sprite(
    PixelGridVector3 authored_position,
    PixelGridVector3 authored_camera_position,
    PixelGridVector3 snapped_camera_position,
    double pixels_per_unit) noexcept;
[[nodiscard]] PixelGridSpriteSnap snap_pixel_sprite(
    PixelGridVector3 authored_position,
    PixelGridVector3 authored_camera_position,
    PixelGridVector3 snapped_camera_position,
    const PixelGridProfile& profile) noexcept;

} // namespace noemancer
