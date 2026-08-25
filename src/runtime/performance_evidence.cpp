#include "runtime/performance_evidence.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <numeric>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#endif

namespace noemancer {
namespace {

constexpr std::array<std::uint64_t, StartupTelemetry::max_frame_markers()> startup_frame_markers{
    1U, 3U, 64U};

std::string_view startup_frame_marker_kind(const std::uint64_t frame) noexcept {
    switch (frame) {
    case 1U: return "first-frame";
    case 3U: return "third-frame";
    case 64U: return "sixty-fourth-frame";
    default: return "acceptance-frame";
    }
}

double percentile(const std::vector<double>& sorted, const double fraction) {
    if (sorted.empty()) return 0.0;
    const auto position = fraction * static_cast<double>(sorted.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

nlohmann::json distribution(const std::span<const double> samples) {
    std::vector<double> sorted(samples.begin(), samples.end());
    std::ranges::sort(sorted);
    const auto mean = sorted.empty() ? 0.0 :
        std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    double variance{};
    for (const auto value : sorted) variance += (value - mean) * (value - mean);
    if (!sorted.empty()) variance /= static_cast<double>(sorted.size());
    return {
        {"unit", "milliseconds"}, {"sampleCount", sorted.size()},
        {"min", sorted.empty() ? 0.0 : sorted.front()}, {"mean", mean},
        {"p50", percentile(sorted, 0.50)}, {"p95", percentile(sorted, 0.95)},
        {"p99", percentile(sorted, 0.99)}, {"max", sorted.empty() ? 0.0 : sorted.back()},
        {"standardDeviation", std::sqrt(variance)}
    };
}

nlohmann::json memory_observation() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return {{"available", true}, {"source", "GetProcessMemoryInfo"},
            {"workingSetBytes", counters.WorkingSetSize}, {"peakWorkingSetBytes", counters.PeakWorkingSetSize},
            {"privateUsageBytes", counters.PrivateUsage}, {"pagefileUsageBytes", counters.PagefileUsage}};
    }
#endif
    return {{"available", false}, {"reason", "Process memory counters are unavailable on this platform."}};
}

std::string compiler_name() {
#ifdef _MSC_VER
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#else
    return "unknown";
#endif
}

std::string configuration_name() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

} // namespace

StartupTelemetry::StartupTelemetry() noexcept
    : started_(std::chrono::steady_clock::now()) {}

double StartupTelemetry::elapsed_milliseconds(
    const std::chrono::steady_clock::time_point point) const noexcept {
    return std::chrono::duration<double, std::milli>(point - started_).count();
}

void StartupTelemetry::begin_phase(const std::string_view name) noexcept {
    finish_phase();
    if (phase_count_ >= kMaxPhases) {
        phase_overflow_ = true;
        return;
    }
    phases_[phase_count_] = Phase{name, std::chrono::steady_clock::now(), {}, false};
    active_phase_ = phase_count_;
    ++phase_count_;
}

void StartupTelemetry::finish_phase() noexcept {
    if (active_phase_ >= phase_count_) return;
    auto& phase = phases_[active_phase_];
    if (!phase.complete) {
        phase.finished = std::chrono::steady_clock::now();
        phase.complete = true;
    }
    active_phase_ = kMaxPhases;
}

void StartupTelemetry::mark_frame(const std::uint64_t frame) noexcept {
    if (frame_marker_count_ >= kMaxFrameMarkers ||
        std::find(startup_frame_markers.begin(), startup_frame_markers.end(), frame) == startup_frame_markers.end()) {
        return;
    }
    frame_markers_[frame_marker_count_] = FrameMarker{frame, std::chrono::steady_clock::now()};
    ++frame_marker_count_;
}

std::string StartupTelemetry::json(
    const std::string_view mode,
    const std::string_view outcome) const {
    using Json = nlohmann::json;
    const auto now = std::chrono::steady_clock::now();
    Json phases = Json::array();
    for (std::size_t index = 0U; index < phase_count_; ++index) {
        const auto& phase = phases_[index];
        const auto end = phase.complete ? phase.finished : now;
        phases.push_back({
            {"name", phase.name},
            {"startOffsetMs", elapsed_milliseconds(phase.started)},
            {"durationMs", std::max(0.0, std::chrono::duration<double, std::milli>(end - phase.started).count())},
            {"complete", phase.complete}});
    }
    Json frame_markers = Json::array();
    for (std::size_t index = 0U; index < frame_marker_count_; ++index) {
        const auto& marker = frame_markers_[index];
        frame_markers.push_back({
            {"frame", marker.frame},
            {"kind", startup_frame_marker_kind(marker.frame)},
            {"elapsedMs", elapsed_milliseconds(marker.captured)}});
    }
    const auto first_frame = frame_marker_count_ > 0U && frame_markers_[0].frame == 1U
        ? elapsed_milliseconds(frame_markers_[0].captured) : 0.0;
    return Json{
        {"schemaVersion", "noemancer.runtime-startup-telemetry/0.1"},
        {"mode", mode},
        {"outcome", outcome},
        {"clock", "steady_clock"},
        {"bounded", true},
        {"limits", {{"maxPhases", kMaxPhases}, {"maxFrameMarkers", kMaxFrameMarkers}}},
        {"phaseOverflow", phase_overflow_},
        {"totalMs", elapsed_milliseconds(now)},
        {"firstFrameMs", first_frame},
        {"phases", std::move(phases)},
        {"frameMarkers", std::move(frame_markers)}}.dump();
}

bool write_performance_evidence(const std::filesystem::path& path,
    const PerformanceEvidenceInput& input, std::string& error) {
    if (path.empty() || input.sampled_frame_milliseconds.empty()) {
        error = "Performance evidence requires a path and at least one measured frame.";
        return false;
    }
    const auto renderer = nlohmann::json::parse(input.renderer_status_json, nullptr, false);
    if (renderer.is_discarded()) {
        error = "Renderer status is not valid JSON.";
        return false;
    }
    const auto gpu_pass_timestamps = nlohmann::json::parse(input.gpu_pass_timestamps_json, nullptr, false);
    if (gpu_pass_timestamps.is_discarded()) {
        error = "GPU pass timestamp evidence is not valid JSON.";
        return false;
    }
    auto gpu_pass_timestamp_evidence = gpu_pass_timestamps;
    nlohmann::json pass_distributions = nlohmann::json::object();
    std::map<std::string, std::vector<double>> pass_samples;
    std::size_t available_timestamp_frames{};
    std::size_t unavailable_timestamp_frames{};
    if (gpu_pass_timestamps.contains("capturedFrames") && gpu_pass_timestamps.at("capturedFrames").is_array()) {
        for (const auto& frame : gpu_pass_timestamps.at("capturedFrames")) {
            bool frame_has_value{};
            if (frame.contains("passes") && frame.at("passes").is_array()) for (const auto& pass : frame.at("passes")) {
                if (!pass.value("available", false) || !pass.contains("milliseconds") ||
                    !pass.at("milliseconds").is_number()) continue;
                const auto pass_id = pass.value("passId", std::string{});
                const auto milliseconds = pass.at("milliseconds").get<double>();
                if (pass_id.empty() || !std::isfinite(milliseconds) || milliseconds < 0.0) continue;
                pass_samples[pass_id].push_back(milliseconds);
                frame_has_value = true;
            }
            if (frame_has_value) ++available_timestamp_frames;
            else ++unavailable_timestamp_frames;
        }
    }
    const bool gpu_pass_timestamps_available = gpu_pass_timestamps.value("supported", false) &&
        available_timestamp_frames > 0U;
    for (const auto& [pass_id, samples] : pass_samples) pass_distributions[pass_id] = distribution(samples);
    gpu_pass_timestamp_evidence["passDistributions"] = std::move(pass_distributions);
    gpu_pass_timestamp_evidence["availableFrameCount"] = available_timestamp_frames;
    gpu_pass_timestamp_evidence["unavailableFrameCount"] = unavailable_timestamp_frames;
    const auto now = std::chrono::system_clock::now();
    const auto document = nlohmann::json{
        {"schemaVersion", "noemancer.performance-evidence/0.1"},
        {"capturedAtUnixMilliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()},
        {"workload", {{"id", input.workload_id}, {"projectId", input.project_id},
            {"targetProfile", input.target_profile}, {"resolution", {{"width", input.width}, {"height", input.height}}},
            {"warmupFrames", input.warmup_frames}, {"measuredFrames", input.sampled_frame_milliseconds.size()}}},
        {"runtime", {{"backend", input.backend}, {"presentMode", input.present_mode},
            {"compiler", compiler_name()}, {"configuration", configuration_name()}}},
        {"scope", {{"includes", nlohmann::json::array({"event polling", "simulation", "render extraction", "GPU command recording", "submission", "presentation pacing"})},
            {"excludes", nlohmann::json::array({"process startup", "asset cook", "packaging", "warmup frames", "final GPU idle wait", "capture encoding"})}}},
        {"cpu", {{"frameTime", distribution(input.sampled_frame_milliseconds)},
            {"swapchainAcquireWait", distribution(input.sampled_swapchain_wait_milliseconds)},
            {"commandSubmitWait", distribution(input.sampled_submit_wait_milliseconds)},
            {"preparation", distribution(input.sampled_preparation_milliseconds)},
            {"eventProcessing", distribution(input.sampled_event_processing_milliseconds)},
            {"simulation", distribution(input.sampled_simulation_milliseconds)},
            {"postSimulationPreparation", [&] {
                std::vector<double> values;values.reserve(input.sampled_preparation_milliseconds.size());
                for(std::size_t index=0;index<input.sampled_preparation_milliseconds.size();++index)
                    values.push_back(std::max(0.0,input.sampled_preparation_milliseconds[index]-
                        (index<input.sampled_event_processing_milliseconds.size()?input.sampled_event_processing_milliseconds[index]:0.0)-
                        (index<input.sampled_simulation_milliseconds.size()?input.sampled_simulation_milliseconds[index]:0.0)));
                return distribution(values);
            }()},
            {"commandRecord", distribution(input.sampled_command_record_milliseconds)},
            {"renderExtract", distribution(input.sampled_render_extract_milliseconds)},
            {"sceneRenderRecord", distribution(input.sampled_scene_render_record_milliseconds)},
            {"editorCommandRecordSegments", {
                {"thumbnailSync", distribution(input.sampled_thumbnail_sync_milliseconds)},
                {"imguiBuild", distribution(input.sampled_imgui_build_milliseconds)},
                {"retainedGame", distribution(input.sampled_retained_game_record_milliseconds)},
                {"retainedInspector", distribution(input.sampled_retained_inspector_record_milliseconds)},
                {"retainedOutliner", distribution(input.sampled_retained_outliner_record_milliseconds)},
                {"retainedAssetBrowser", distribution(input.sampled_retained_asset_browser_record_milliseconds)},
                {"imguiGpu", distribution(input.sampled_imgui_gpu_record_milliseconds)},
                {"imguiPanels", {
                    {"refresh", distribution(input.sampled_editor_ui_panel_milliseconds[0])},
                    {"chrome", distribution(input.sampled_editor_ui_panel_milliseconds[1])},
                    {"scene", distribution(input.sampled_editor_ui_panel_milliseconds[2])},
                    {"animation", distribution(input.sampled_editor_ui_panel_milliseconds[3])},
                    {"outliner", distribution(input.sampled_editor_ui_panel_milliseconds[4])},
                    {"inspector", distribution(input.sampled_editor_ui_panel_milliseconds[5])},
                    {"assets", distribution(input.sampled_editor_ui_panel_milliseconds[6])},
                    {"console", distribution(input.sampled_editor_ui_panel_milliseconds[7])},
                    {"agentContext", distribution(input.sampled_editor_ui_panel_milliseconds[8])}}},
                {"meaning", "Main-thread wall time inside named editor command-recording regions; values may include backend resource waits and are not GPU execution time."}}},
            {"activeFrameEstimate", [&] {
                std::vector<double> values;values.reserve(input.sampled_frame_milliseconds.size());
                for(std::size_t index=0;index<input.sampled_frame_milliseconds.size();++index)
                    values.push_back(std::max(0.0,input.sampled_frame_milliseconds[index]-
                        (index<input.sampled_swapchain_wait_milliseconds.size()?input.sampled_swapchain_wait_milliseconds[index]:0.0)-
                        (index<input.sampled_submit_wait_milliseconds.size()?input.sampled_submit_wait_milliseconds[index]:0.0)));
                return distribution(values);
            }()},
            {"meaning", "End-to-end main-thread wall time, observed swapchain acquisition blocking, command-submit blocking, and an active estimate with both waits removed; none is GPU execution time."}}},
        {"gpu", {{"available", gpu_pass_timestamps_available},
            {"source", gpu_pass_timestamps_available ? "Noemancer SDL_GPU native timestamp adapter" : "unavailable"},
            {"passTimestamps", gpu_pass_timestamp_evidence},
            {"presentationTelemetry", {{"available", false}, {"requiredSource", "PresentMon/2.4.1"},
                {"reason", "In-process pass timestamps do not measure presentation latency; external telemetry remains a separate evidence source."}}}}},
        {"memory", memory_observation()}, {"renderer", renderer}
    };
    std::error_code filesystem_error;
    auto parent = path.parent_path();
    if (parent.empty()) parent = ".";
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        error = "Unable to create evidence directory: " + filesystem_error.message();
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    if (std::filesystem::exists(path)) {
        error = "Performance evidence target already exists; evidence is immutable.";
        return false;
    }
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) { error = "Unable to open temporary evidence file."; return false; }
        stream << document.dump(2) << '\n';
        if (!stream) { error = "Unable to write performance evidence."; return false; }
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary);
        error = "Unable to publish performance evidence atomically: " + filesystem_error.message();
        return false;
    }
    return true;
}

} // namespace noemancer
