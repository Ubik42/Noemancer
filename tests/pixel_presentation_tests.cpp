#include "engine/pixel_presentation.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace noemancer;

bool check(const bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool same_plan(const PixelPresentation& left, const PixelPresentation& right) {
    return left.valid == right.valid && left.undersized == right.undersized &&
           left.status == right.status && left.code == right.code &&
           left.detail == right.detail && left.virtual_extent.width == right.virtual_extent.width &&
           left.virtual_extent.height == right.virtual_extent.height &&
           left.physical_output_extent.width == right.physical_output_extent.width &&
           left.physical_output_extent.height == right.physical_output_extent.height &&
           left.integer_scale == right.integer_scale &&
           left.content_rect.x == right.content_rect.x &&
           left.content_rect.y == right.content_rect.y &&
           left.content_rect.width == right.content_rect.width &&
           left.content_rect.height == right.content_rect.height &&
           left.virtual_rect.x == right.virtual_rect.x &&
           left.virtual_rect.y == right.virtual_rect.y &&
           left.virtual_rect.width == right.virtual_rect.width &&
           left.virtual_rect.height == right.virtual_rect.height &&
           left.letterbox.left == right.letterbox.left &&
           left.letterbox.top == right.letterbox.top &&
           left.letterbox.right == right.letterbox.right &&
           left.letterbox.bottom == right.letterbox.bottom;
}

PixelPresentation plan(const std::uint32_t virtual_width,
                       const std::uint32_t virtual_height,
                       const std::uint32_t output_width,
                       const std::uint32_t output_height) {
    return plan_pixel_presentation(PixelPresentationInput{
        .virtual_extent = PixelExtent{virtual_width, virtual_height},
        .physical_output_extent = PixelExtent{output_width, output_height}});
}

bool test_integer_upscale_and_exact_fill() {
    const auto presentation = plan(320U, 180U, 1920U, 1080U);
    if (!check(presentation.valid && !presentation.undersized &&
                   presentation.status == PixelPresentationStatus::exact &&
                   presentation.code == "pixel-presentation.ok",
               "Exact integer presentation was not valid.")) {
        return false;
    }
    if (!check(presentation.integer_scale == 6U &&
                   presentation.content_rect.x == 0U &&
                   presentation.content_rect.y == 0U &&
                   presentation.content_rect.width == 1920U &&
                   presentation.content_rect.height == 1080U &&
                   presentation.letterbox.left == 0U &&
                   presentation.letterbox.top == 0U &&
                   presentation.letterbox.right == 0U &&
                   presentation.letterbox.bottom == 0U,
               "Exact integer content geometry was incorrect.")) {
        return false;
    }

    const auto first = map_physical_pixel(presentation, 0U, 0U);
    const auto last = map_physical_pixel(presentation, 1919U, 1079U);
    if (!check(first.inside_output && first.inside_content &&
                   first.valid_virtual_pixel && first.virtual_x == 0U &&
                   first.virtual_y == 0U && last.valid_virtual_pixel &&
                   last.virtual_x == 319U && last.virtual_y == 179U,
               "Exact presentation pixel mapping was incorrect.")) {
        return false;
    }
    return check(!map_physical_pixel(presentation, 1920U, 0U).inside_output,
                 "The right output edge was treated as an inside pixel.");
}

bool test_letterbox_and_edges() {
    const auto presentation = plan(320U, 180U, 1440U, 900U);
    if (!check(presentation.valid &&
                   presentation.status == PixelPresentationStatus::letterboxed &&
                   presentation.integer_scale == 4U &&
                   presentation.code == "pixel-presentation.letterboxed",
               "1440x900 letterbox presentation was not valid.")) {
        return false;
    }
    if (!check(presentation.content_rect.x == 80U &&
                   presentation.content_rect.y == 90U &&
                   presentation.content_rect.width == 1280U &&
                   presentation.content_rect.height == 720U &&
                   presentation.letterbox.left == 80U &&
                   presentation.letterbox.top == 90U &&
                   presentation.letterbox.right == 80U &&
                   presentation.letterbox.bottom == 90U,
               "1440x900 letterbox margins were incorrect.")) {
        return false;
    }

    const auto left_edge = map_physical_pixel(presentation, 80U, 90U);
    const auto right_edge = map_physical_pixel(presentation, 1359U, 809U);
    const auto leading_bar = map_physical_pixel(presentation, 79U, 90U);
    const auto trailing_bar = map_physical_pixel(presentation, 1360U, 90U);
    if (!check(left_edge.inside_output && left_edge.inside_content &&
                   left_edge.valid_virtual_pixel && left_edge.virtual_x == 0U &&
                   left_edge.virtual_y == 0U && right_edge.valid_virtual_pixel &&
                   right_edge.virtual_x == 319U && right_edge.virtual_y == 179U,
               "Letterbox content edges did not map to virtual edges.")) {
        return false;
    }
    return check(leading_bar.inside_output && !leading_bar.inside_content &&
                     !leading_bar.valid_virtual_pixel &&
                     trailing_bar.inside_output && !trailing_bar.inside_content,
                 "Letterbox pixels were incorrectly mapped to virtual pixels.");
}

bool test_odd_remainders_and_resize_stability() {
    const auto odd = plan(320U, 180U, 1439U, 899U);
    if (!check(odd.valid && odd.integer_scale == 4U &&
                   odd.content_rect.x == 79U && odd.content_rect.y == 89U &&
                   odd.content_rect.width == 1280U &&
                   odd.content_rect.height == 720U && odd.letterbox.left == 79U &&
                   odd.letterbox.right == 80U && odd.letterbox.top == 89U &&
                   odd.letterbox.bottom == 90U,
               "Odd letterbox remainder was not split deterministically.")) {
        return false;
    }
    const auto narrower = plan(320U, 180U, 1440U, 899U);
    const auto wider = plan(320U, 180U, 1441U, 899U);
    if (!check(narrower.content_rect.x == 80U && narrower.letterbox.right == 80U &&
                   wider.content_rect.x == 80U && wider.letterbox.right == 81U,
               "Resize did not preserve stable integer anchoring.")) {
        return false;
    }
    const auto old_edge = map_physical_pixel(odd, 1358U, 808U);
    return check(old_edge.valid_virtual_pixel && old_edge.virtual_x == 319U &&
                     old_edge.virtual_y == 179U,
                 "Odd-size content edge mapping had an off-by-one error.");
}

bool test_undersized_center_crop() {
    const auto presentation = plan(320U, 180U, 160U, 90U);
    if (!check(presentation.valid && presentation.undersized &&
                   presentation.status == PixelPresentationStatus::undersized &&
                   presentation.integer_scale == 1U &&
                   presentation.code == "pixel-presentation.undersized",
               "Undersized output did not report the explicit 1:1 mode.")) {
        return false;
    }
    if (!check(presentation.content_rect.x == 0U &&
                   presentation.content_rect.y == 0U &&
                   presentation.content_rect.width == 160U &&
                   presentation.content_rect.height == 90U &&
                   presentation.virtual_rect.x == 80U &&
                   presentation.virtual_rect.y == 45U &&
                   presentation.virtual_rect.width == 160U &&
                   presentation.virtual_rect.height == 90U &&
                   presentation.letterbox.left == 0U &&
                   presentation.letterbox.top == 0U &&
                   presentation.letterbox.right == 0U &&
                   presentation.letterbox.bottom == 0U,
               "Undersized centered crop geometry was incorrect.")) {
        return false;
    }
    const auto first = map_physical_pixel(presentation, 0U, 0U);
    const auto last = map_physical_pixel(presentation, 159U, 89U);
    if (!check(first.inside_content && first.valid_virtual_pixel &&
                   first.virtual_x == 80U && first.virtual_y == 45U &&
                   last.valid_virtual_pixel && last.virtual_x == 239U &&
                   last.virtual_y == 134U,
               "Undersized crop mapping was not centered or 1:1.")) {
        return false;
    }

    const auto width_only = plan(320U, 180U, 200U, 240U);
    if (!check(width_only.undersized && width_only.virtual_rect.x == 60U &&
                   width_only.virtual_rect.y == 0U &&
                   width_only.virtual_rect.width == 200U &&
                   width_only.virtual_rect.height == 180U &&
                   width_only.content_rect.x == 0U &&
                   width_only.content_rect.y == 30U &&
                   width_only.content_rect.width == 200U &&
                   width_only.content_rect.height == 180U &&
                   width_only.letterbox.top == 30U &&
                   width_only.letterbox.bottom == 30U,
               "Asymmetric undersized crop did not preserve strict 1:1 pixels.")) {
        return false;
    }
    const auto top_bar = map_physical_pixel(width_only, 100U, 29U);
    const auto first_content = map_physical_pixel(width_only, 0U, 30U);
    const auto last_content = map_physical_pixel(width_only, 199U, 209U);
    return check(top_bar.inside_output && !top_bar.inside_content &&
                     first_content.valid_virtual_pixel &&
                     first_content.virtual_x == 60U &&
                     first_content.virtual_y == 0U &&
                     last_content.valid_virtual_pixel &&
                     last_content.virtual_x == 259U &&
                     last_content.virtual_y == 179U,
                 "Asymmetric undersized mapping introduced fractional scaling.");
}

bool test_invalid_extents() {
    const auto zero_virtual = plan(0U, 180U, 1920U, 1080U);
    if (!check(!zero_virtual.valid &&
                   zero_virtual.status == PixelPresentationStatus::invalid &&
                   zero_virtual.code == "pixel-presentation.virtual-extent-zero",
               "Zero virtual extent was accepted.")) {
        return false;
    }
    const auto zero_output = plan(320U, 180U, 0U, 1080U);
    if (!check(!zero_output.valid &&
                   zero_output.code == "pixel-presentation.output-extent-zero",
               "Zero output extent was accepted.")) {
        return false;
    }
    const auto invalid_map = map_physical_pixel(zero_output, 0U, 0U);
    return check(!invalid_map.inside_output && !invalid_map.inside_content &&
                     !invalid_map.valid_virtual_pixel,
                 "Invalid presentation produced a virtual mapping.");
}

bool test_resize_round_trip_and_half_open_boundaries() {
    const auto first = plan(320U, 180U, 1440U, 900U);
    const auto intermediate = plan(320U, 180U, 1439U, 899U);
    const auto restored = plan(320U, 180U, 1440U, 900U);
    if (!check(first.valid && intermediate.valid && same_plan(first, restored),
               "Presentation layout did not round-trip after a physical resize.")) {
        return false;
    }

    // Content rectangles are half-open on both axes.  The first pixel after
    // the content rectangle is a letterbox pixel, never the last virtual
    // pixel a second time.
    const auto right_edge = intermediate.content_rect.x + intermediate.content_rect.width;
    const auto bottom_edge = intermediate.content_rect.y + intermediate.content_rect.height;
    const auto after_right = map_physical_pixel(intermediate, right_edge,
                                                 intermediate.content_rect.y);
    const auto after_bottom = map_physical_pixel(intermediate, intermediate.content_rect.x,
                                                   bottom_edge);
    const auto last = map_physical_pixel(intermediate, right_edge - 1U, bottom_edge - 1U);
    if (!check(after_right.inside_output && !after_right.inside_content &&
                   !after_right.valid_virtual_pixel && after_bottom.inside_output &&
                   !after_bottom.inside_content && !after_bottom.valid_virtual_pixel &&
                   last.valid_virtual_pixel && last.virtual_x == 319U &&
                   last.virtual_y == 179U,
               "Half-open content boundary mapping duplicated or dropped a pixel.")) {
        return false;
    }

    const auto outside = map_physical_pixel(intermediate, std::numeric_limits<std::uint32_t>::max(),
                                             std::numeric_limits<std::uint32_t>::max());
    return check(!outside.inside_output && !outside.inside_content &&
                     !outside.valid_virtual_pixel,
                 "Out-of-range physical picking coordinates were accepted.");
}

} // namespace

int main() {
    if (!test_integer_upscale_and_exact_fill()) return 1;
    if (!test_letterbox_and_edges()) return 2;
    if (!test_odd_remainders_and_resize_stability()) return 3;
    if (!test_undersized_center_crop()) return 4;
    if (!test_invalid_extents()) return 5;
    if (!test_resize_round_trip_and_half_open_boundaries()) return 6;
    std::cout << "Pixel presentation tests passed\n";
    return 0;
}
