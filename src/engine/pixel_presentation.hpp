#pragma once

#include <cstdint>
#include <string>

namespace noemancer {

// PixelPresentation deliberately contains no renderer, window-system or
// project types.  A virtual pixel is presented into a physical output using
// only integer replication; the same plan can therefore be consumed by the
// Engine, Editor and Agent projections.
struct PixelExtent final {
    std::uint32_t width{};
    std::uint32_t height{};
};

struct PixelRect final {
    // Rectangles use a half-open interval: [x, x + width) x
    // [y, y + height).  Coordinates are pixel indices, not pixel edges.
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct PixelLetterbox final {
    std::uint32_t left{};
    std::uint32_t top{};
    std::uint32_t right{};
    std::uint32_t bottom{};
};

struct PixelPresentationInput final {
    PixelExtent virtual_extent{};
    PixelExtent physical_output_extent{};
};

enum class PixelPresentationStatus : std::uint8_t {
    invalid = 0,
    exact = 1,
    letterboxed = 2,
    undersized = 3
};

struct PixelPresentation final {
    bool valid{};
    bool undersized{};
    PixelPresentationStatus status{PixelPresentationStatus::invalid};
    // These codes are stable machine-readable diagnostics.  detail is
    // intentionally deterministic and contains no platform/window text.
    std::string code;
    std::string detail;

    PixelExtent virtual_extent{};
    PixelExtent physical_output_extent{};
    // For a valid non-undersized plan this is floor(output / virtual).  For
    // an undersized plan it is exactly 1:1 and the plan crops instead.
    std::uint32_t integer_scale{};
    // Physical destination and virtual source rectangles use half-open
    // intervals.  virtual_rect is useful for the deterministic undersized
    // center crop; normally it is the complete virtual extent.
    PixelRect content_rect{};
    PixelRect virtual_rect{};
    PixelLetterbox letterbox{};
};

struct PixelMapping final {
    // physical coordinates outside physical_output_extent set this false.
    bool inside_output{};
    // Letterbox pixels are inside the output but outside content_rect.
    bool inside_content{};
    // True only when a virtual pixel was produced by the mapping.
    bool valid_virtual_pixel{};
    std::uint32_t virtual_x{};
    std::uint32_t virtual_y{};
};

[[nodiscard]] PixelPresentation plan_pixel_presentation(
    const PixelPresentationInput& input);

// Maps one integer physical pixel index to one virtual pixel index.  No
// interpolation is performed.  For an undersized plan, physical pixels map
// into the centered cropped virtual_rect.  Coordinates are half-open pixel
// indices; the right/bottom edge never maps inside the rectangle.
[[nodiscard]] PixelMapping map_physical_pixel(
    const PixelPresentation& presentation,
    std::uint32_t physical_x,
    std::uint32_t physical_y);

} // namespace noemancer
