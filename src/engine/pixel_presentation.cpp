#include "engine/pixel_presentation.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace noemancer {
namespace {

PixelPresentation invalid_presentation(
    const PixelPresentationInput& input,
    std::string code,
    std::string detail) {
    PixelPresentation result;
    result.code = std::move(code);
    result.detail = std::move(detail);
    result.virtual_extent = input.virtual_extent;
    result.physical_output_extent = input.physical_output_extent;
    return result;
}

bool in_half_open_rect(const PixelRect& rect, const std::uint32_t x,
                       const std::uint32_t y) {
    const auto right = static_cast<std::uint64_t>(rect.x) + rect.width;
    const auto bottom = static_cast<std::uint64_t>(rect.y) + rect.height;
    return static_cast<std::uint64_t>(x) >= rect.x &&
           static_cast<std::uint64_t>(x) < right &&
           static_cast<std::uint64_t>(y) >= rect.y &&
           static_cast<std::uint64_t>(y) < bottom;
}

} // namespace

PixelPresentation plan_pixel_presentation(const PixelPresentationInput& input) {
    if (input.virtual_extent.width == 0U || input.virtual_extent.height == 0U) {
        return invalid_presentation(
            input,
            "pixel-presentation.virtual-extent-zero",
            "Virtual pixel extent must have non-zero width and height.");
    }
    if (input.physical_output_extent.width == 0U ||
        input.physical_output_extent.height == 0U) {
        return invalid_presentation(
            input,
            "pixel-presentation.output-extent-zero",
            "Physical output extent must have non-zero width and height.");
    }

    PixelPresentation result;
    result.valid = true;
    result.virtual_extent = input.virtual_extent;
    result.physical_output_extent = input.physical_output_extent;

    const bool can_fit_without_crop =
        input.physical_output_extent.width >= input.virtual_extent.width &&
        input.physical_output_extent.height >= input.virtual_extent.height;
    if (!can_fit_without_crop) {
        // Never use a fractional scale when the output is smaller than the
        // authored virtual resolution.  A centered 1:1 source crop is stable
        // under resize and keeps every surviving physical pixel sharp.
        result.undersized = true;
        result.status = PixelPresentationStatus::undersized;
        result.code = "pixel-presentation.undersized";
        result.detail =
            "Physical output is smaller than the virtual extent; integer scale is 1 and the centered virtual source is cropped.";
        result.integer_scale = 1U;

        const auto source_width = std::min(
            input.virtual_extent.width, input.physical_output_extent.width);
        const auto source_height = std::min(
            input.virtual_extent.height, input.physical_output_extent.height);
        result.virtual_rect = PixelRect{
            (input.virtual_extent.width - source_width) / 2U,
            (input.virtual_extent.height - source_height) / 2U,
            source_width,
            source_height};
        const auto remaining_width =
            input.physical_output_extent.width - source_width;
        const auto remaining_height =
            input.physical_output_extent.height - source_height;
        const auto left = remaining_width / 2U;
        const auto top = remaining_height / 2U;
        // Keep the surviving source pixels strictly 1:1.  If only one axis
        // is undersized, the other axis is centered with bars rather than
        // fractionally stretching the cropped image to fill the output.
        result.content_rect = PixelRect{
            left,
            top,
            source_width,
            source_height};
        result.letterbox = PixelLetterbox{
            left,
            top,
            remaining_width - left,
            remaining_height - top};
        return result;
    }

    // Integer division is the contract: it is the largest integer scale for
    // which the complete virtual image fits in both output dimensions.
    result.integer_scale = std::min(
        input.physical_output_extent.width / input.virtual_extent.width,
        input.physical_output_extent.height / input.virtual_extent.height);
    if (result.integer_scale == 0U) {
        // This is unreachable after can_fit_without_crop, but retaining an
        // explicit failure keeps the plan safe if the arithmetic contract is
        // changed later.
        return invalid_presentation(
            input,
            "pixel-presentation.integer-scale-zero",
            "A fitting presentation requires a positive integer scale.");
    }

    const auto content_width64 = static_cast<std::uint64_t>(
        input.virtual_extent.width) * result.integer_scale;
    const auto content_height64 = static_cast<std::uint64_t>(
        input.virtual_extent.height) * result.integer_scale;
    if (content_width64 > input.physical_output_extent.width ||
        content_height64 > input.physical_output_extent.height ||
        content_width64 > std::numeric_limits<std::uint32_t>::max() ||
        content_height64 > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_presentation(
            input,
            "pixel-presentation.geometry-overflow",
            "Integer-scaled content does not fit the physical output extent.");
    }

    const auto content_width = static_cast<std::uint32_t>(content_width64);
    const auto content_height = static_cast<std::uint32_t>(content_height64);
    const auto remaining_width = input.physical_output_extent.width - content_width;
    const auto remaining_height = input.physical_output_extent.height - content_height;
    // The floor half goes to the leading edge; an odd remainder therefore
    // differs by at most one pixel and does not introduce a half-pixel offset.
    const auto left = remaining_width / 2U;
    const auto top = remaining_height / 2U;
    result.content_rect = PixelRect{left, top, content_width, content_height};
    result.virtual_rect = PixelRect{
        0U, 0U, input.virtual_extent.width, input.virtual_extent.height};
    result.letterbox = PixelLetterbox{
        left,
        top,
        remaining_width - left,
        remaining_height - top};

    const bool has_letterbox = remaining_width != 0U || remaining_height != 0U;
    result.status = has_letterbox
        ? PixelPresentationStatus::letterboxed
        : PixelPresentationStatus::exact;
    result.code = has_letterbox
        ? "pixel-presentation.letterboxed"
        : "pixel-presentation.ok";
    result.detail = has_letterbox
        ? "Virtual pixels are replicated at an integer scale and centered with deterministic letterbox margins."
        : "Virtual pixels fill the physical output at an integer scale.";
    return result;
}

PixelMapping map_physical_pixel(const PixelPresentation& presentation,
                                const std::uint32_t physical_x,
                                const std::uint32_t physical_y) {
    PixelMapping result;
    if (!presentation.valid || presentation.integer_scale == 0U) return result;
    if (physical_x >= presentation.physical_output_extent.width ||
        physical_y >= presentation.physical_output_extent.height) {
        return result;
    }
    result.inside_output = true;
    if (!in_half_open_rect(presentation.content_rect, physical_x, physical_y)) {
        return result;
    }

    const auto local_x = physical_x - presentation.content_rect.x;
    const auto local_y = physical_y - presentation.content_rect.y;
    const auto virtual_x_offset =
        static_cast<std::uint64_t>(local_x) / presentation.integer_scale;
    const auto virtual_y_offset =
        static_cast<std::uint64_t>(local_y) / presentation.integer_scale;
    if (virtual_x_offset >= presentation.virtual_rect.width ||
        virtual_y_offset >= presentation.virtual_rect.height) {
        // This protects callers that retain a malformed/hand-authored plan;
        // the plan produced above always satisfies the bounds.
        return result;
    }
    const auto virtual_x = static_cast<std::uint64_t>(presentation.virtual_rect.x) +
                           virtual_x_offset;
    const auto virtual_y = static_cast<std::uint64_t>(presentation.virtual_rect.y) +
                           virtual_y_offset;
    if (virtual_x >= presentation.virtual_extent.width ||
        virtual_y >= presentation.virtual_extent.height) {
        return result;
    }

    result.inside_content = true;
    result.valid_virtual_pixel = true;
    result.virtual_x = static_cast<std::uint32_t>(virtual_x);
    result.virtual_y = static_cast<std::uint32_t>(virtual_y);
    return result;
}

} // namespace noemancer
