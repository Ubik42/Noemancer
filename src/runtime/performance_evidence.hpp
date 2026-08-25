#pragma once

#include <cstdint>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace noemancer {

// Startup timing is deliberately a small, bounded runtime observation rather
// than a profiler.  It records only the phase boundaries owned by the runtime
// bootstrap and the three acceptance frame markers (1, 3 and 64).  The
// implementation keeps third-party JSON types private and serializes one
// stable machine-readable projection at the end of startup.
class StartupTelemetry final {
public:
    StartupTelemetry() noexcept;

    void begin_phase(std::string_view name) noexcept;
    void finish_phase() noexcept;
    void mark_frame(std::uint64_t frame) noexcept;

    [[nodiscard]] std::string json(
        std::string_view mode,
        std::string_view outcome) const;

    static constexpr std::size_t max_phases() noexcept { return kMaxPhases; }
    static constexpr std::size_t max_frame_markers() noexcept { return kMaxFrameMarkers; }

private:
    struct Phase final {
        std::string_view name{};
        std::chrono::steady_clock::time_point started{};
        std::chrono::steady_clock::time_point finished{};
        bool complete{};
    };

    struct FrameMarker final {
        std::uint64_t frame{};
        std::chrono::steady_clock::time_point captured{};
    };

    static constexpr std::size_t kMaxPhases = 32U;
    static constexpr std::size_t kMaxFrameMarkers = 3U;

    [[nodiscard]] double elapsed_milliseconds(
        std::chrono::steady_clock::time_point point) const noexcept;

    std::chrono::steady_clock::time_point started_;
    std::array<Phase, kMaxPhases> phases_{};
    std::array<FrameMarker, kMaxFrameMarkers> frame_markers_{};
    std::size_t phase_count_{};
    std::size_t active_phase_{kMaxPhases};
    std::size_t frame_marker_count_{};
    bool phase_overflow_{};
};

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
    std::span<const double> sampled_thumbnail_sync_milliseconds;
    std::span<const double> sampled_imgui_build_milliseconds;
    std::span<const double> sampled_retained_game_record_milliseconds;
    std::span<const double> sampled_retained_inspector_record_milliseconds;
    std::span<const double> sampled_retained_outliner_record_milliseconds;
    std::span<const double> sampled_retained_asset_browser_record_milliseconds;
    std::span<const double> sampled_imgui_gpu_record_milliseconds;
    std::array<std::span<const double>,9> sampled_editor_ui_panel_milliseconds;
    std::string renderer_status_json;
};

[[nodiscard]] bool write_performance_evidence(
    const std::filesystem::path& path,
    const PerformanceEvidenceInput& input,
    std::string& error);

} // namespace noemancer
