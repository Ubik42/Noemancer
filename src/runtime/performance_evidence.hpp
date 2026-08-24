#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace noemancer {

struct PerformanceEvidenceInput final {
    std::string workload_id;
    std::string project_id;
    std::string target_profile;
    std::string backend;
    std::string present_mode;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t warmup_frames{};
    std::span<const double> sampled_frame_milliseconds;
    std::span<const double> sampled_swapchain_wait_milliseconds;
    std::span<const double> sampled_submit_wait_milliseconds;
    std::span<const double> sampled_preparation_milliseconds;
    std::span<const double> sampled_event_processing_milliseconds;
    std::span<const double> sampled_simulation_milliseconds;
    std::span<const double> sampled_command_record_milliseconds;
    std::span<const double> sampled_render_extract_milliseconds;
    std::span<const double> sampled_scene_render_record_milliseconds;
    std::string renderer_status_json;
};

[[nodiscard]] bool write_performance_evidence(
    const std::filesystem::path& path,
    const PerformanceEvidenceInput& input,
    std::string& error);

} // namespace noemancer
