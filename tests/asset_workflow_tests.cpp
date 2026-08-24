#include "engine/asset_workflow.hpp"
#include "engine/asset_registry.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using noemancer::AssetJobKind;
using noemancer::AssetJobQueue;
using noemancer::AssetJobQueueConfig;
using noemancer::AssetJobRequest;
using noemancer::AssetJobState;

bool wait_for_state(
    const AssetJobQueue& queue,
    const std::string& job_id,
    const AssetJobState expected,
    const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = queue.snapshot(job_id);
        if (snapshot.has_value() && snapshot->state == expected) return true;
        std::this_thread::sleep_for(2ms);
    }
    const auto snapshot = queue.snapshot(job_id);
    return snapshot.has_value() && snapshot->state == expected;
}

AssetJobRequest request(
    const AssetJobKind kind,
    std::string asset_id,
    std::string source_uri,
    std::string fingerprint,
    const std::uint64_t source_revision = 1U) {
    return AssetJobRequest{
        .kind = kind,
        .asset_id = std::move(asset_id),
        .source_uri = std::move(source_uri),
        .input_fingerprint = std::move(fingerprint),
        .source_revision = source_revision,
        .description = "asset workflow test"
    };
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "noemancer-asset-workflow-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(root / "generated");
    {
        std::ofstream registry(root / "registry.json", std::ios::binary);
        registry << R"({"schema":"noemancer.assets/0.1","assets":[
            {"id":"asset.workflow.fixture","displayName":"Workflow Fixture","kind":"Geometry",
             "uri":"builtin://geometry/cube","license":"Noemancer built-in","redistribution":"public"}
        ]})";
    }

    noemancer::AssetWorkflowConfig workflow_config;
    workflow_config.asset_roots = {root};
    workflow_config.artifact_root = root / "generated";
    workflow_config.max_diagnostic_bytes = 256U;
    const auto executor = noemancer::make_asset_workflow_executor(workflow_config);
    const noemancer::AssetRegistry registry(root);
    const auto* fixture=registry.find("asset.workflow.fixture");
    if(fixture==nullptr||fixture->content_hash.empty())return 11;
    const auto fixture_fingerprint=fixture->content_hash;

    AssetJobQueue queue(AssetJobQueueConfig{.worker_count = 1U, .max_queued_jobs = 8U});
    const auto imported = queue.submit(
        request(AssetJobKind::import, "asset.workflow.fixture", "builtin://geometry/cube", fixture_fingerprint),
        executor);
    if (!imported.accepted || !wait_for_state(queue, imported.job_id, AssetJobState::succeeded)) {
        std::cerr << "real Import workflow did not complete successfully\n";
        return 1;
    }
    const auto import_observation = nlohmann::json::parse(queue.observation_json(imported.job_id));
    if (import_observation.at("code") != "asset.import.ok" ||
        import_observation.dump().find("<path-redacted>") != std::string::npos) {
        std::cerr << "Import workflow returned an unstable observation\n";
        return 2;
    }

    const auto inspected = queue.submit(
        request(AssetJobKind::inspect, "asset.workflow.fixture", "builtin://geometry/cube", fixture_fingerprint),
        executor);
    if (!inspected.accepted || !wait_for_state(queue, inspected.job_id, AssetJobState::succeeded)) {
        std::cerr << "real Inspect workflow did not complete successfully\n";
        return 3;
    }
    const auto inspect_observation = nlohmann::json::parse(queue.observation_json(inspected.job_id));
    const auto artifacts = inspect_observation.at("artifacts");
    if (inspect_observation.at("code") != "ok" || artifacts.empty() ||
        artifacts.front().get<std::string>().find("generated://asset-workflow/inspect/") != 0U) {
        std::cerr << "Inspect workflow did not return a stable artifact URI\n";
        return 4;
    }
    bool found_artifact = false;
    for (const auto& entry : std::filesystem::directory_iterator(root / "generated" / "inspect")) {
        found_artifact = true;
        std::ifstream payload(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(payload)), std::istreambuf_iterator<char>());
        if (text.find(root.string()) != std::string::npos || text.find(root.generic_string()) != std::string::npos) {
            std::cerr << "Inspect artifact leaked a local source path\n";
            return 6;
        }
    }
    if (!found_artifact) {
        std::cerr << "Inspect workflow did not persist evidence\n";
        return 5;
    }

    const auto missing = queue.submit(
        request(AssetJobKind::import, "asset.workflow.missing", "asset://missing", "sha256:missing"),
        executor);
    if (!missing.accepted || !wait_for_state(queue, missing.job_id, AssetJobState::failed) ||
        queue.snapshot(missing.job_id)->code != "asset.workflow.asset-not-found") {
        std::cerr << "missing asset did not produce a stable failure\n";
        return 7;
    }

    const auto stale = queue.submit(
        request(AssetJobKind::import, "asset.workflow.fixture", "builtin://geometry/cube", "sha256:stale"),
        executor);
    if (!stale.accepted || !wait_for_state(queue, stale.job_id, AssetJobState::failed) ||
        queue.snapshot(stale.job_id)->code != "asset.workflow.fingerprint-stale") {
        std::cerr << "stale fingerprint was not rejected\n";
        return 8;
    }

    const auto refreshed_revision = queue.submit(
        request(AssetJobKind::import, "asset.workflow.fixture", "builtin://geometry/cube", fixture_fingerprint, 2U),
        executor);
    if (!refreshed_revision.accepted || !wait_for_state(queue, refreshed_revision.job_id, AssetJobState::succeeded) ||
        queue.snapshot(refreshed_revision.job_id)->code != "asset.import.ok") {
        std::cerr << "source revision provenance was incorrectly compared with the isolated registry\n";
        return 9;
    }

    AssetJobQueue cancel_queue(AssetJobQueueConfig{.worker_count = 1U, .max_queued_jobs = 4U});
    const auto blocker = cancel_queue.submit(
        request(AssetJobKind::import, "asset.workflow.blocker", "asset://blocker", "sha256:blocker"),
        [](const auto&, auto& context) {
        while (!context.cancellation_requested()) std::this_thread::sleep_for(2ms);
        return noemancer::AssetJobExecutionResult{.cancelled = true};
    });
    const auto queued = cancel_queue.submit(
        request(AssetJobKind::inspect, "asset.workflow.fixture", "builtin://geometry/cube", fixture_fingerprint),
        executor);
    if (!blocker.accepted || !queued.accepted || !cancel_queue.cancel(queued.job_id).accepted ||
        !wait_for_state(cancel_queue, queued.job_id, AssetJobState::cancelled)) {
        std::cerr << "queued workflow did not support cancellation\n";
        return 10;
    }

    cancel_queue.shutdown();
    queue.shutdown();
    std::filesystem::remove_all(root, cleanup_error);
    return 0;
}
