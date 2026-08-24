#pragma once

#include "engine/hybrid_pixel_profile.hpp"
#include "engine/render_world.hpp"

#include <cstddef>
#include <string>

namespace noemancer {

// A renderer-neutral projection receipt.  The canonical Scene/World is never
// mutated: callers apply this to the freshly extracted Render World each frame.
struct HybridPixelRenderProjection final {
    bool enabled{};
    bool valid{};
    bool orthographic_camera{};
    bool camera_snapped{};
    std::size_t sprites_snapped{};
    std::size_t tile_cells_snapped{};
    double world_units_per_pixel{};
    double orthographic_height{};
    std::string code{"hybrid-pixel.disabled"};
    std::string detail{"Hybrid Pixel projection is disabled."};
};

[[nodiscard]] HybridPixelRenderProjection apply_hybrid_pixel_render_projection(
    RenderWorldSnapshot& render_world,
    const HybridPixelProfile& profile);

} // namespace noemancer
