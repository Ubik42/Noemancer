#include "runtime/performance_evidence.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

namespace {

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return nlohmann::json::parse(input);
}

noemancer::PerformanceEvidenceInput evidence_input(
    const std::vector<double>& samples, const std::string& gpu_json) {
    return {.workload_id = "gpu.timestamp.contract",
        .project_id = "game.test",
        .target_profile = "windows-x64-release",
        .backend = "direct3d12",
        .present_mode = "immediate",
        .width = 1280U,
        .height = 720U,
        .warmup_frames = 1U,
        .sampled_frame_milliseconds = samples,
        .sampled_swapchain_wait_milliseconds = samples,
        .sampled_submit_wait_milliseconds = samples,
        .sampled_preparation_milliseconds = samples,
        .sampled_event_processing_milliseconds = samples,
        .sampled_simulation_milliseconds = samples,
        .sampled_command_record_milliseconds = samples,
        .sampled_render_extract_milliseconds = samples,
        .sampled_scene_render_record_milliseconds = samples,
        .sampled_thumbnail_sync_milliseconds = samples,
        .sampled_imgui_build_milliseconds = samples,
        .sampled_retained_game_record_milliseconds = samples,
        .sampled_retained_inspector_record_milliseconds = samples,
        .sampled_retained_outliner_record_milliseconds = samples,
        .sampled_retained_asset_browser_record_milliseconds = samples,
        .sampled_imgui_gpu_record_milliseconds = samples,
        .sampled_editor_ui_panel_milliseconds = {samples, samples, samples, samples, samples,
            samples, samples, samples, samples},
        .gpu_pass_timestamps_json = gpu_json,
        .renderer_status_json = R"({"schemaVersion":"noemancer.renderer-status.test"})"};
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "noemancer-gpu-pass-evidence-tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    const std::vector<double> samples{1.0, 2.0};
    std::string error;

    const auto available_path = root / "available.json";
    const std::string available = R"({
      "schemaVersion":"noemancer.gpu-pass-timestamps/0.1",
      "supported":true,
      "capturedFrames":[
        {"frameIndex":7,"state":"available","reason":"ok","overflowed":false,"droppedPasses":0,
         "passes":[
           {"passId":"render.pass.opaque-lit","milliseconds":0.75,"available":true},
           {"passId":"render.pass.tone-map","milliseconds":null,"available":false}]},
        {"frameIndex":8,"state":"available","reason":"ok","overflowed":false,"droppedPasses":0,
         "passes":[{"passId":"render.pass.opaque-lit","milliseconds":1.25,"available":true}]}]})";
    if (!noemancer::write_performance_evidence(
            available_path, evidence_input(samples, available), error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto document = read_json(available_path);
    const auto& gpu = document.at("gpu");
    if (!gpu.value("available", false) ||
        gpu.at("passTimestamps").at("availableFrameCount") != 2U ||
        gpu.at("passTimestamps").at("unavailableFrameCount") != 0U ||
        gpu.at("passTimestamps").at("passDistributions").at("render.pass.opaque-lit").at("sampleCount") != 2U ||
        gpu.at("passTimestamps").at("passDistributions").contains("render.pass.tone-map")) {
        std::cerr << "Available and unavailable pass values were not preserved faithfully.\n";
        return 2;
    }

    const auto unsupported_path = root / "unsupported.json";
    const std::string unsupported = R"({
      "schemaVersion":"noemancer.gpu-pass-timestamps/0.1",
      "supported":false,"reason":"backend-unsupported","capturedFrames":[]})";
    if (!noemancer::write_performance_evidence(
            unsupported_path, evidence_input(samples, unsupported), error)) {
        std::cerr << error << '\n';
        return 3;
    }
    const auto unsupported_document = read_json(unsupported_path);
    if (unsupported_document.at("gpu").value("available", true) ||
        unsupported_document.at("gpu").at("passTimestamps").at("reason") != "backend-unsupported" ||
        !unsupported_document.at("gpu").at("passTimestamps").at("passDistributions").empty()) {
        std::cerr << "Unsupported timestamp evidence was converted into a fabricated measurement.\n";
        return 4;
    }

    const auto pending_path = root / "pending.json";
    const std::string pending = R"({
      "schemaVersion":"noemancer.gpu-pass-timestamps/0.1",
      "supported":true,"reason":"results-pending","capturedFrames":[
        {"frameIndex":9,"state":"unavailable","reason":"timestamp-results-not-ready",
         "overflowed":false,"droppedPasses":0,"passes":[
           {"passId":"render.pass.opaque-lit","milliseconds":null,"available":false}]}]})";
    if (!noemancer::write_performance_evidence(
            pending_path, evidence_input(samples, pending), error)) {
        std::cerr << error << '\n';
        return 5;
    }
    const auto pending_document = read_json(pending_path);
    if (pending_document.at("gpu").value("available", true) ||
        pending_document.at("gpu").at("passTimestamps").at("availableFrameCount") != 0U ||
        pending_document.at("gpu").at("passTimestamps").at("unavailableFrameCount") != 1U) {
        std::cerr << "Pending timestamp frames were exposed as available GPU measurements.\n";
        return 6;
    }

    std::filesystem::remove_all(root, ignored);
    return 0;
}
