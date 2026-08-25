#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct GpuPassTiming final {
    std::string pass_id;
    std::optional<double> milliseconds;
};

struct GpuPassTimingFrame final {
    std::uint64_t frame_index{};
    std::string state{"unavailable"};
    std::string reason{"not-captured"};
    bool overflowed{};
    std::uint32_t dropped_passes{};
    std::vector<GpuPassTiming> passes;
};

// Runtime-only adapter around Noemancer's pinned SDL_GPU timestamp extension.
// It deliberately keeps SDL and native query ownership out of the engine's
// persisted scene, Render Graph and Agent schemas. Every in-flight frame owns
// a separate query pool and fence; results are read only after that fence is
// signalled. Missing or invalid values remain absent, never fabricated as 0ms.
class GpuPassTimestampAdapter final {
public:
    struct Limits final {
        std::uint32_t frames_in_flight{3U};
        std::uint32_t maximum_passes{64U};
        std::uint32_t maximum_captured_frames{4096U};
    };

    explicit GpuPassTimestampAdapter(SDL_GPUDevice* device);
    GpuPassTimestampAdapter(SDL_GPUDevice* device, Limits limits);
    ~GpuPassTimestampAdapter();

    GpuPassTimestampAdapter(const GpuPassTimestampAdapter&) = delete;
    GpuPassTimestampAdapter& operator=(const GpuPassTimestampAdapter&) = delete;

    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool begin_frame(
        SDL_GPUCommandBuffer* command,
        std::uint64_t frame_index,
        std::span<const std::string> pass_ids);
    void begin_pass(SDL_GPUCommandBuffer* command, std::string_view pass_id);
    void end_pass(SDL_GPUCommandBuffer* command, std::string_view pass_id);
    void end_frame(SDL_GPUCommandBuffer* command);

    [[nodiscard]] bool submission_requires_fence() const noexcept;
    void attach_submission_fence(SDL_GPUFence* fence);
    void abandon_submission() noexcept;
    void poll();

    [[nodiscard]] bool supported() const noexcept { return supported_; }
    [[nodiscard]] std::string status_json() const;
    [[nodiscard]] std::string evidence_json() const;
    [[nodiscard]] const std::optional<GpuPassTimingFrame>& latest_frame() const noexcept {
        return latest_frame_;
    }

private:
    struct FrameSlot final {
        SDL_GPUTimestampQueryPool* pool{};
        SDL_GPUFence* fence{};
        std::uint64_t frame_index{};
        std::uint32_t query_count{};
        std::uint32_t next_pass{};
        std::uint32_t dropped_passes{};
        bool recording{};
        bool awaiting_submission{};
        bool pending{};
        bool overflowed{};
        std::vector<std::string> pass_ids;
        std::vector<bool> begin_written;
        std::vector<bool> end_written;
    };

    void release_slot(FrameSlot& slot) noexcept;
    void consume_slot(FrameSlot& slot);
    void publish_unavailable(std::uint64_t frame_index, std::string reason,
                             bool overflowed = false, std::uint32_t dropped_passes = 0U);

    SDL_GPUDevice* device_{};
    Limits limits_{};
    std::vector<FrameSlot> slots_;
    FrameSlot* recording_slot_{};
    double timestamp_period_nanoseconds_{};
    std::string backend_;
    std::string capability_reason_;
    std::string capability_detail_;
    bool supported_{};
    bool enabled_{};
    std::uint64_t skipped_pending_slots_{};
    std::uint64_t dropped_completed_frames_{};
    std::optional<GpuPassTimingFrame> latest_frame_;
    std::deque<GpuPassTimingFrame> completed_frames_;
};

} // namespace noemancer
