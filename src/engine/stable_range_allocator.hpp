#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noemancer {

// Keeps logical groups at stable offsets in a fixed-size GPU arena. Draw order is
// deliberately handled by a separate indirection stream.
class StableRangeAllocator final {
public:
    struct Allocation final {
        std::size_t first{};
        std::size_t count{};
        std::size_t capacity{};
        bool moved{};
        bool valid{};
    };

    struct Statistics final {
        std::size_t capacity{};
        std::size_t high_water{};
        std::size_t live_ranges{};
        std::size_t live_slots{};
        std::size_t free_slots{};
        std::size_t largest_free_range{};
        std::size_t moves{};
        std::size_t evictions{};
    };

    explicit StableRangeAllocator(const std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] Allocation acquire(const std::string& key, const std::size_t count, const std::uint64_t epoch) {
        if (key.empty() || count == 0 || count > capacity_) return {};
        if (auto found = allocations_.find(key); found != allocations_.end()) {
            found->second.last_seen = epoch;
            found->second.count = count;
            if (count <= found->second.capacity)
                return {found->second.first, count, found->second.capacity, false, true};
            release(found);
            ++moves_;
        }
        const auto reserved = std::bit_ceil(count);
        if (reserved > capacity_) return {};
        const auto first = reserve(reserved);
        if (first == invalid_offset) return {};
        allocations_.insert_or_assign(key, Entry{first, count, reserved, epoch});
        return {first, count, reserved, true, true};
    }

    [[nodiscard]] std::size_t sweep(const std::uint64_t epoch, const std::uint64_t retention_epochs) {
        std::size_t removed = 0;
        for (auto iterator = allocations_.begin(); iterator != allocations_.end();) {
            if (epoch >= iterator->second.last_seen && epoch - iterator->second.last_seen > retention_epochs) {
                auto stale = iterator++;
                release(stale);
                ++removed;
            } else {
                ++iterator;
            }
        }
        evictions_ += removed;
        return removed;
    }

    [[nodiscard]] Statistics statistics() const {
        Statistics value{.capacity = capacity_, .high_water = high_water_, .live_ranges = allocations_.size(),
            .moves = moves_, .evictions = evictions_};
        for (const auto& [key, entry] : allocations_) {
            static_cast<void>(key);
            value.live_slots += entry.capacity;
        }
        for (const auto& range : free_ranges_) {
            value.free_slots += range.count;
            value.largest_free_range = std::max(value.largest_free_range, range.count);
        }
        value.free_slots += capacity_ - high_water_;
        value.largest_free_range = std::max(value.largest_free_range, capacity_ - high_water_);
        return value;
    }

    void clear() {
        allocations_.clear();
        free_ranges_.clear();
        high_water_ = 0;
        moves_ = 0;
        evictions_ = 0;
    }

private:
    struct Entry final {
        std::size_t first{};
        std::size_t count{};
        std::size_t capacity{};
        std::uint64_t last_seen{};
    };
    struct FreeRange final { std::size_t first{}; std::size_t count{}; };
    using AllocationMap = std::unordered_map<std::string, Entry>;
    static constexpr std::size_t invalid_offset = std::numeric_limits<std::size_t>::max();

    [[nodiscard]] std::size_t reserve(const std::size_t count) {
        auto best = free_ranges_.end();
        for (auto iterator = free_ranges_.begin(); iterator != free_ranges_.end(); ++iterator)
            if (iterator->count >= count && (best == free_ranges_.end() || iterator->count < best->count)) best = iterator;
        if (best != free_ranges_.end()) {
            const auto first = best->first;
            best->first += count;
            best->count -= count;
            if (best->count == 0) free_ranges_.erase(best);
            return first;
        }
        if (high_water_ + count > capacity_) return invalid_offset;
        const auto first = high_water_;
        high_water_ += count;
        return first;
    }

    void release(const AllocationMap::iterator iterator) {
        free_ranges_.push_back({iterator->second.first, iterator->second.capacity});
        allocations_.erase(iterator);
        std::ranges::sort(free_ranges_, {}, &FreeRange::first);
        std::vector<FreeRange> merged;
        for (const auto& range : free_ranges_) {
            if (!merged.empty() && merged.back().first + merged.back().count == range.first) merged.back().count += range.count;
            else merged.push_back(range);
        }
        free_ranges_ = std::move(merged);
        while (!free_ranges_.empty() && free_ranges_.back().first + free_ranges_.back().count == high_water_) {
            high_water_ = free_ranges_.back().first;
            free_ranges_.pop_back();
        }
    }

    std::size_t capacity_{};
    std::size_t high_water_{};
    std::size_t moves_{};
    std::size_t evictions_{};
    AllocationMap allocations_;
    std::vector<FreeRange> free_ranges_;
};

} // namespace noemancer
