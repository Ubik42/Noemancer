#include "runtime/performance_evidence.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
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
            {"activeFrameEstimate", [&] {
                std::vector<double> values;values.reserve(input.sampled_frame_milliseconds.size());
                for(std::size_t index=0;index<input.sampled_frame_milliseconds.size();++index)
                    values.push_back(std::max(0.0,input.sampled_frame_milliseconds[index]-
                        (index<input.sampled_swapchain_wait_milliseconds.size()?input.sampled_swapchain_wait_milliseconds[index]:0.0)-
                        (index<input.sampled_submit_wait_milliseconds.size()?input.sampled_submit_wait_milliseconds[index]:0.0)));
                return distribution(values);
            }()},
            {"meaning", "End-to-end main-thread wall time, observed swapchain acquisition blocking, command-submit blocking, and an active estimate with both waits removed; none is GPU execution time."}}},
        {"gpu", {{"available", false}, {"requiredSource", "PresentMon/2.4.1"},
            {"reason", "SDL_GPU exposes no portable timestamp-query contract; external presentation telemetry must supply GPUTime."}}},
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
