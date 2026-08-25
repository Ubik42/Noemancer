#include "runtime/gpu_pass_timestamp_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace noemancer {

GpuPassTimestampAdapter::GpuPassTimestampAdapter(SDL_GPUDevice* device)
    : GpuPassTimestampAdapter(device, Limits{}) {}

GpuPassTimestampAdapter::GpuPassTimestampAdapter(SDL_GPUDevice* device, Limits limits)
    : device_(device), limits_(limits) {
    if (!device_) {
        capability_reason_ = "gpu-device-unavailable";
        return;
    }
    backend_ = SDL_GetGPUDeviceDriver(device_);
    if (backend_ != "direct3d12" && backend_ != "vulkan") {
        capability_reason_ = "backend-timestamp-adapter-unavailable";
        return;
    }
    if (limits_.frames_in_flight == 0U || limits_.maximum_passes == 0U ||
        limits_.maximum_captured_frames == 0U ||
        limits_.maximum_passes > std::numeric_limits<std::uint32_t>::max() / 2U) {
        capability_reason_ = "invalid-timestamp-ring-limits";
        return;
    }
    if (!SDL_GPUDeviceSupportsTimestampQueries(device_)) {
        capability_reason_ = "backend-timestamp-query-unsupported";
        return;
    }
    timestamp_period_nanoseconds_ = SDL_GetGPUTimestampPeriodNanoseconds(device_);
    if (!(timestamp_period_nanoseconds_ > 0.0) || !std::isfinite(timestamp_period_nanoseconds_)) {
        capability_reason_ = "backend-timestamp-period-unavailable";
        return;
    }
    slots_.resize(limits_.frames_in_flight);
    const auto query_capacity = limits_.maximum_passes * 2U;
    for (auto& slot : slots_) {
        slot.pool = SDL_CreateGPUTimestampQueryPool(device_, query_capacity);
        if (!slot.pool) {
            capability_reason_ = "backend-timestamp-pool-create-failed";
            capability_detail_ = SDL_GetError();
            for (auto& allocated : slots_) release_slot(allocated);
            slots_.clear();
            return;
        }
        slot.pass_ids.reserve(limits_.maximum_passes);
    }
    supported_ = true;
    capability_reason_ = "ok";
}

GpuPassTimestampAdapter::~GpuPassTimestampAdapter() {
    if (device_) SDL_WaitForGPUIdle(device_);
    for (auto& slot : slots_) release_slot(slot);
}

void GpuPassTimestampAdapter::set_enabled(const bool enabled) noexcept {
    enabled_ = enabled && supported_;
}

bool GpuPassTimestampAdapter::begin_frame(
    SDL_GPUCommandBuffer* command, const std::uint64_t frame_index,
    const std::span<const std::string> pass_ids) {
    poll();
    recording_slot_ = nullptr;
    if (!enabled_) {
        if (!supported_) publish_unavailable(frame_index, capability_reason_);
        return false;
    }
    if (!command || slots_.empty()) {
        publish_unavailable(frame_index, "timestamp-command-buffer-unavailable");
        return false;
    }
    auto& slot = slots_[frame_index % slots_.size()];
    if (slot.pending || slot.awaiting_submission || slot.recording) {
        ++skipped_pending_slots_;
        publish_unavailable(frame_index, "timestamp-frame-slot-pending");
        return false;
    }
    slot.frame_index = frame_index;
    slot.query_count = 0U;
    slot.next_pass = 0U;
    slot.dropped_passes = pass_ids.size() > limits_.maximum_passes
        ? static_cast<std::uint32_t>(pass_ids.size() - limits_.maximum_passes)
        : 0U;
    slot.overflowed = slot.dropped_passes != 0U;
    slot.pass_ids.assign(pass_ids.begin(), pass_ids.begin() +
        static_cast<std::ptrdiff_t>(std::min<std::size_t>(pass_ids.size(), limits_.maximum_passes)));
    slot.begin_written.assign(slot.pass_ids.size(), false);
    slot.end_written.assign(slot.pass_ids.size(), false);
    slot.query_count = static_cast<std::uint32_t>(slot.pass_ids.size()) * 2U;
    if (slot.query_count == 0U) {
        publish_unavailable(frame_index, "render-graph-has-no-profiled-passes");
        return false;
    }
    if (!SDL_ResetGPUTimestampQueries(command, slot.pool, 0U, slot.query_count)) {
        const char* error = SDL_GetError();
        publish_unavailable(frame_index,
            error && *error ? "timestamp-query-reset-failed: " + std::string(error)
                            : "timestamp-query-reset-failed",
            slot.overflowed, slot.dropped_passes);
        return false;
    }
    slot.recording = true;
    recording_slot_ = &slot;
    return true;
}

void GpuPassTimestampAdapter::begin_pass(SDL_GPUCommandBuffer* command, const std::string_view pass_id) {
    if (!recording_slot_ || !recording_slot_->recording || !command) return;
    auto& slot = *recording_slot_;
    if (slot.next_pass >= slot.pass_ids.size()) return;
    if (slot.pass_ids[slot.next_pass] != pass_id) {
        slot.overflowed = true;
        ++slot.dropped_passes;
        return;
    }
    if (SDL_WriteGPUTimestamp(command, slot.pool, slot.next_pass * 2U)) {
        slot.begin_written[slot.next_pass] = true;
    } else {
        slot.overflowed = true;
        ++slot.dropped_passes;
    }
}

void GpuPassTimestampAdapter::end_pass(SDL_GPUCommandBuffer* command, const std::string_view pass_id) {
    if (!recording_slot_ || !recording_slot_->recording || !command) return;
    auto& slot = *recording_slot_;
    if (slot.next_pass >= slot.pass_ids.size()) return;
    if (slot.pass_ids[slot.next_pass] != pass_id) {
        slot.overflowed = true;
        ++slot.dropped_passes;
        return;
    }
    if (SDL_WriteGPUTimestamp(command, slot.pool, slot.next_pass * 2U + 1U)) {
        slot.end_written[slot.next_pass] = true;
    } else {
        slot.overflowed = true;
        ++slot.dropped_passes;
    }
    ++slot.next_pass;
}

void GpuPassTimestampAdapter::end_frame(SDL_GPUCommandBuffer* command) {
    if (!recording_slot_) return;
    auto& slot = *recording_slot_;
    slot.recording = false;
    if (slot.next_pass != slot.pass_ids.size()) {
        slot.overflowed = true;
        slot.dropped_passes += static_cast<std::uint32_t>(slot.pass_ids.size() - slot.next_pass);
    }
    if (slot.next_pass == 0U || !command ||
        !SDL_ResolveGPUTimestampQueries(command, slot.pool, 0U, slot.next_pass * 2U)) {
        publish_unavailable(slot.frame_index, "timestamp-query-resolve-failed", slot.overflowed, slot.dropped_passes);
        recording_slot_ = nullptr;
        return;
    }
    slot.query_count = slot.next_pass * 2U;
    slot.awaiting_submission = true;
    recording_slot_ = nullptr;
}

bool GpuPassTimestampAdapter::submission_requires_fence() const noexcept {
    for (const auto& slot : slots_) if (slot.awaiting_submission) return true;
    return false;
}

void GpuPassTimestampAdapter::attach_submission_fence(SDL_GPUFence* fence) {
    for (auto& slot : slots_) if (slot.awaiting_submission) {
        slot.awaiting_submission = false;
        if (!fence) {
            publish_unavailable(slot.frame_index, "timestamp-submission-fence-unavailable",
                slot.overflowed, slot.dropped_passes);
            return;
        }
        slot.fence = fence;
        slot.pending = true;
        return;
    }
}

void GpuPassTimestampAdapter::abandon_submission() noexcept {
    for (auto& slot : slots_) if (slot.awaiting_submission || slot.recording) {
        slot.awaiting_submission = false;
        slot.recording = false;
        recording_slot_ = nullptr;
        return;
    }
}

void GpuPassTimestampAdapter::poll() {
    if (!device_) return;
    for (auto& slot : slots_) {
        if (!slot.pending || !slot.fence || !SDL_QueryGPUFence(device_, slot.fence)) continue;
        consume_slot(slot);
    }
}

void GpuPassTimestampAdapter::release_slot(FrameSlot& slot) noexcept {
    if (slot.fence && device_) SDL_ReleaseGPUFence(device_, slot.fence);
    if (slot.pool && device_) SDL_ReleaseGPUTimestampQueryPool(device_, slot.pool);
    slot = {};
}

void GpuPassTimestampAdapter::consume_slot(FrameSlot& slot) {
    std::vector<Uint64> ticks(slot.query_count);
    bool available = false;
    const bool read = SDL_GetGPUTimestampQueryResults(
        device_, slot.pool, 0U, slot.query_count, ticks.data(), &available);
    GpuPassTimingFrame frame;
    frame.frame_index = slot.frame_index;
    frame.overflowed = slot.overflowed;
    frame.dropped_passes = slot.dropped_passes;
    frame.state = read && available ? (slot.overflowed ? "overflow" : "available") : "unavailable";
    frame.reason = read ? (available ? (slot.overflowed ? "query-capacity-exceeded" : "ok")
                                           : "timestamp-results-not-ready")
                        : "timestamp-result-read-failed";
    frame.passes.reserve(slot.next_pass);
    for (std::uint32_t index = 0U; index < slot.next_pass; ++index) {
        GpuPassTiming timing{slot.pass_ids[index], std::nullopt};
        const auto begin = ticks[index * 2U];
        const auto end = ticks[index * 2U + 1U];
        if (read && available && slot.begin_written[index] && slot.end_written[index] && end > begin) {
            const double milliseconds = static_cast<double>(end - begin) *
                timestamp_period_nanoseconds_ / 1'000'000.0;
            if (std::isfinite(milliseconds) && milliseconds >= 0.0) timing.milliseconds = milliseconds;
        }
        frame.passes.push_back(std::move(timing));
    }
    latest_frame_ = frame;
    if (completed_frames_.size() == limits_.maximum_captured_frames) {
        completed_frames_.pop_front();
        ++dropped_completed_frames_;
    }
    completed_frames_.push_back(std::move(frame));
    SDL_ReleaseGPUFence(device_, slot.fence);
    slot.fence = nullptr;
    slot.pending = false;
    slot.query_count = 0U;
    slot.next_pass = 0U;
    slot.pass_ids.clear();
    slot.begin_written.clear();
    slot.end_written.clear();
}

void GpuPassTimestampAdapter::publish_unavailable(
    const std::uint64_t frame_index, std::string reason,
    const bool overflowed, const std::uint32_t dropped_passes) {
    latest_frame_ = GpuPassTimingFrame{frame_index, overflowed ? "overflow" : "unavailable",
        std::move(reason), overflowed, dropped_passes, {}};
}

std::string GpuPassTimestampAdapter::status_json() const {
    nlohmann::json result{{"schemaVersion", "noemancer.gpu-pass-timestamps/0.1"},
        {"supported", supported_}, {"enabled", enabled_}, {"backend", backend_},
        {"reason", capability_reason_},
        {"detail", capability_detail_.empty() ? nlohmann::json(nullptr) : nlohmann::json(capability_detail_)},
        {"queueScope", "graphics"},
        {"timestampPeriodNanoseconds", supported_ ? nlohmann::json(timestamp_period_nanoseconds_) : nlohmann::json(nullptr)},
        {"framesInFlight", limits_.frames_in_flight}, {"maximumPasses", limits_.maximum_passes},
        {"maximumCapturedFrames", limits_.maximum_captured_frames},
        {"queriesPerPass", 2}, {"readback", "fence-gated-frame-ring"},
        {"resolve", "frame-end-batch"}, {"skippedPendingSlots", skipped_pending_slots_},
        {"droppedCompletedFrames", dropped_completed_frames_},
        {"latestFrame", nullptr}};
    if (latest_frame_) {
        nlohmann::json passes = nlohmann::json::array();
        for (const auto& pass : latest_frame_->passes)
            passes.push_back({{"passId", pass.pass_id},
                {"milliseconds", pass.milliseconds ? nlohmann::json(*pass.milliseconds) : nlohmann::json(nullptr)},
                {"available", pass.milliseconds.has_value()}});
        result["latestFrame"] = {{"frameIndex", latest_frame_->frame_index},
            {"state", latest_frame_->state}, {"reason", latest_frame_->reason},
            {"overflowed", latest_frame_->overflowed}, {"droppedPasses", latest_frame_->dropped_passes},
            {"passes", std::move(passes)}};
    }
    return result.dump();
}

std::string GpuPassTimestampAdapter::evidence_json() const {
    auto result = nlohmann::json::parse(status_json());
    result["capturedFrames"] = nlohmann::json::array();
    for (const auto& frame : completed_frames_) {
        nlohmann::json passes = nlohmann::json::array();
        for (const auto& pass : frame.passes)
            passes.push_back({{"passId", pass.pass_id},
                {"milliseconds", pass.milliseconds ? nlohmann::json(*pass.milliseconds) : nlohmann::json(nullptr)},
                {"available", pass.milliseconds.has_value()}});
        result["capturedFrames"].push_back({{"frameIndex", frame.frame_index},
            {"state", frame.state}, {"reason", frame.reason},
            {"overflowed", frame.overflowed}, {"droppedPasses", frame.dropped_passes},
            {"passes", std::move(passes)}});
    }
    return result.dump();
}

} // namespace noemancer
