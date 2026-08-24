#include "engine/linear_dirty_ranges.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using noemancer::LinearDirtyRange;
using noemancer::LinearDirtyRangesResult;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> records(const std::size_t item_count, const std::size_t stride) {
    std::vector<std::byte> result(item_count * stride);
    for (std::size_t item = 0U; item < item_count; ++item) {
        for (std::size_t byte = 0U; byte < stride; ++byte) {
            result[item * stride + byte] = static_cast<std::byte>((item * 17U + byte + 1U) & 0xffU);
        }
    }
    return result;
}

void require_ranges(const LinearDirtyRangesResult& result,
                    const std::vector<LinearDirtyRange>& expected,
                    const std::string& context) {
    require(result.valid, context + ": comparison must be valid");
    require(result.ranges == expected, context + ": dirty ranges are incorrect");
}

void test_first_full_and_force_full() {
    const auto current = records(4U, 4U);
    const auto first = noemancer::compare_linear_dirty_ranges(
        std::span<const std::byte>{}, current, 4U);
    require(first.previous_item_count == 0U && first.current_item_count == 4U,
            "first comparison item counts are incorrect");
    require_ranges(first, {{0U, 4U}}, "first comparison");

    const auto forced = noemancer::compare_linear_dirty_ranges(current, current, 4U, true);
    require_ranges(forced, {{0U, 4U}}, "force-full comparison");
}

void test_same_single_contiguous_and_separate_changes() {
    const auto previous = records(6U, 4U);
    const auto same = noemancer::compare_linear_dirty_ranges(previous, previous, 4U);
    require_ranges(same, {}, "identical comparison");

    auto single = previous;
    single[2U * 4U + 1U] = static_cast<std::byte>(0xeeU);
    const auto one = noemancer::compare_linear_dirty_ranges(previous, single, 4U);
    require_ranges(one, {{2U, 1U}}, "single-slot comparison");

    auto changed = previous;
    changed[1U * 4U] = static_cast<std::byte>(0xe1U);
    changed[2U * 4U + 3U] = static_cast<std::byte>(0xe2U);
    changed[4U * 4U + 2U] = static_cast<std::byte>(0xe4U);
    const auto merged = noemancer::compare_linear_dirty_ranges(previous, changed, 4U);
    require_ranges(merged, {{1U, 2U}, {4U, 1U}}, "contiguous and separate comparison");
}

void test_move_and_resize_boundaries() {
    const auto previous = records(4U, 4U);
    auto moved = previous;
    std::copy_n(previous.begin() + 4, 8, moved.begin());
    std::copy_n(previous.begin(), 4, moved.begin() + 8);
    const auto move_result = noemancer::compare_linear_dirty_ranges(previous, moved, 4U);
    require_ranges(move_result, {{0U, 3U}}, "moved-item comparison");

    const auto shortened = std::vector<std::byte>(previous.begin(), previous.begin() + 3U * 4U);
    const auto shrink_result = noemancer::compare_linear_dirty_ranges(previous, shortened, 4U);
    require_ranges(shrink_result, {}, "shortened-tail comparison");

    const auto grown = records(6U, 4U);
    const auto grow_result = noemancer::compare_linear_dirty_ranges(shortened, grown, 4U);
    require_ranges(grow_result, {{3U, 3U}}, "grown-tail comparison");
}

void test_invalid_stride_and_alignment() {
    const auto valid = records(2U, 4U);
    const auto zero_stride = noemancer::compare_linear_dirty_ranges(valid, valid, 0U);
    require(!zero_stride.valid && zero_stride.code == "invalid-stride",
            "zero stride must be rejected");

    const auto invalid_previous = noemancer::compare_linear_dirty_ranges(
        std::span<const std::byte>(valid.data(), valid.size() - 1U), valid, 4U);
    require(!invalid_previous.valid && invalid_previous.code == "previous-size-not-aligned",
            "misaligned previous span must be rejected");

    const auto invalid_current = noemancer::compare_linear_dirty_ranges(
        valid, std::span<const std::byte>(valid.data(), valid.size() - 1U), 4U);
    require(!invalid_current.valid && invalid_current.code == "current-size-not-aligned",
            "misaligned current span must be rejected");
}

} // namespace

int main() {
    try {
        test_first_full_and_force_full();
        test_same_single_contiguous_and_separate_changes();
        test_move_and_resize_boundaries();
        test_invalid_stride_and_alignment();
    } catch (const std::exception& error) {
        std::cerr << "linear_dirty_ranges_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "linear_dirty_ranges_tests: ok\n";
    return 0;
}
