#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace noemancer {

// A half-open range of active linear items.  The offset and count are item
// indices, not byte offsets; callers multiply them by their fixed stride when
// building GPU buffer regions.
struct LinearDirtyRange final {
    std::size_t offset{};
    std::size_t count{};

    friend bool operator==(const LinearDirtyRange&, const LinearDirtyRange&) = default;
};

struct LinearDirtyRangesResult final {
    bool valid{};
    std::string code;
    std::string detail;
    std::size_t previous_item_count{};
    std::size_t current_item_count{};
    std::vector<LinearDirtyRange> ranges;
};

// Compares two linear byte payloads using fixed-size items and coalesces
// adjacent changed items.  Only the current active item range is reported:
// removed tail items are represented by the caller's current item count and
// never become an upload range.  A force-full comparison reports the complete
// current range even when the bytes are unchanged.
[[nodiscard]] LinearDirtyRangesResult compare_linear_dirty_ranges(
    std::span<const std::byte> previous,
    std::span<const std::byte> current,
    std::size_t stride,
    bool force_full = false);

} // namespace noemancer
