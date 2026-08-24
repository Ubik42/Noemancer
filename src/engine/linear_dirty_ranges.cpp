#include "engine/linear_dirty_ranges.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace noemancer {
namespace {

[[nodiscard]] LinearDirtyRangesResult invalid_result(const std::string_view code,
                                                     const std::string_view detail) {
    LinearDirtyRangesResult result;
    result.valid = false;
    result.code = code;
    result.detail = detail;
    return result;
}

void append_range(std::vector<LinearDirtyRange>& ranges,
                  const std::size_t offset,
                  const std::size_t count) {
    if (count == 0U) return;
    if (!ranges.empty() && ranges.back().offset + ranges.back().count == offset) {
        ranges.back().count += count;
        return;
    }
    ranges.push_back({offset, count});
}

} // namespace

LinearDirtyRangesResult compare_linear_dirty_ranges(
    const std::span<const std::byte> previous,
    const std::span<const std::byte> current,
    const std::size_t stride,
    const bool force_full) {
    if (stride == 0U) {
        return invalid_result("invalid-stride", "Linear dirty range stride must be non-zero.");
    }
    if (previous.size() % stride != 0U) {
        return invalid_result("previous-size-not-aligned",
                              "Previous byte span size must be an integral number of stride-sized items.");
    }
    if (current.size() % stride != 0U) {
        return invalid_result("current-size-not-aligned",
                              "Current byte span size must be an integral number of stride-sized items.");
    }

    LinearDirtyRangesResult result;
    result.valid = true;
    result.code = "ok";
    result.previous_item_count = previous.size() / stride;
    result.current_item_count = current.size() / stride;

    if (result.current_item_count == 0U) return result;
    if (force_full) {
        result.ranges.push_back({0U, result.current_item_count});
        return result;
    }

    const auto common_item_count = std::min(result.previous_item_count, result.current_item_count);
    std::size_t dirty_begin = result.current_item_count;
    for (std::size_t item = 0U; item < result.current_item_count; ++item) {
        const bool dirty = item >= common_item_count ||
            (std::memcmp(previous.data() + item * stride, current.data() + item * stride, stride) != 0);
        if (dirty) {
            if (dirty_begin == result.current_item_count) dirty_begin = item;
            continue;
        }
        if (dirty_begin != result.current_item_count) {
            append_range(result.ranges, dirty_begin, item - dirty_begin);
            dirty_begin = result.current_item_count;
        }
    }
    if (dirty_begin != result.current_item_count) {
        append_range(result.ranges, dirty_begin, result.current_item_count - dirty_begin);
    }
    return result;
}

} // namespace noemancer
