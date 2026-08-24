#include "engine/asset_job_queue.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
        const auto current = queue.snapshot(job_id);
        if (current.has_value() && current->state == expected) return true;
        std::this_thread::sleep_for(2ms);
    }
    const auto current = queue.snapshot(job_id);
    return current.has_value() && current->state == expected;
}

AssetJobRequest request(
    const AssetJobKind kind,
    const std::string& asset_id,
    const std::string& fingerprint) {
    return AssetJobRequest{
        .kind = kind,
        .asset_id = asset_id,
        .source_uri = "D:\\private\\project\\" + asset_id,
        .plan_payload = "{\"plan\":\"" + fingerprint + "\"}",
        .input_fingerprint = fingerprint,
        .target_profile = "windows-x64-debug",
        .source_revision = 7U,
        .description = "private source path should not appear in observation"
    };
}

} // namespace

int main() {
    std::atomic<int> active_workers{};
    std::atomic<int> maximum_workers{};
    const auto observe_workers = [&] {
        const auto active = active_workers.fetch_add(1) + 1;
        auto maximum = maximum_workers.load();
        while (active > maximum && !maximum_workers.compare_exchange_weak(maximum, active)) {}
    };
    const auto release_workers = [&] { active_workers.fetch_sub(1); };

    AssetJobQueueConfig config;
    config.worker_count = 2U;
    config.max_queued_jobs = 8U;
    config.max_diagnostics = 3U;
    config.max_artifacts = 2U;
    config.max_observation_bytes = 2048U;
    AssetJobQueue queue(config);

    const auto import_request = request(AssetJobKind::import, "asset.import.one", "sha256:import");
    const auto import = queue.submit(import_request, [&](const auto&, auto& context) {
        observe_workers();
        static_cast<void>(context.report(0.25F, "decode", "D:\\secret\\never-observe.bin"));
        std::this_thread::sleep_for(25ms);
        static_cast<void>(context.report(0.75F, "validate", "validated"));
        release_workers();
        return noemancer::AssetJobExecutionResult{
            .success = true,
            .code = "asset.import.ok",
            .detail = "Import completed.",
            .artifact_uris = {"generated://imports/import-one"}
        };
    });
    if (!import.accepted || import.job_id.find("asset-job-import-") != 0U) {
        std::cerr << "import job was not queued with a stable ID\n";
        return 1;
    }
    const auto duplicate = queue.submit(import_request, {});
    if (!duplicate.duplicate || duplicate.job_id != import.job_id || duplicate.accepted) {
        std::cerr << "duplicate request did not reuse its stable ID\n";
        return 2;
    }

    const auto inspect_request = request(AssetJobKind::inspect, "asset.inspect.one", "sha256:inspect");
    const auto inspect = queue.submit(inspect_request, [&](const auto&, auto& context) {
        observe_workers();
        static_cast<void>(context.report(0.5F, "parse", "ready"));
        std::this_thread::sleep_for(25ms);
        release_workers();
        return noemancer::AssetJobExecutionResult{
            .success = true,
            .code = "asset.inspect.ok",
            .detail = "Inspection completed.",
            .artifact_uris = {"generated://inspect/one"}
        };
    });
    if (!inspect.accepted || !wait_for_state(queue, import.job_id, AssetJobState::succeeded) ||
        !wait_for_state(queue, inspect.job_id, AssetJobState::succeeded)) {
        std::cerr << "concurrent successful jobs did not complete\n";
        return 3;
    }
    if (maximum_workers.load() < 2) {
        std::cerr << "bounded worker queue did not exercise two workers\n";
        return 4;
    }

    const auto import_snapshot = queue.snapshot(import.job_id);
    if (!import_snapshot.has_value() || import_snapshot->revision < 4U ||
        import_snapshot->progress != 1.0F || import_snapshot->artifact_uris.size() != 1U) {
        std::cerr << "successful job snapshot did not retain progress/revision/artifact evidence\n";
        return 5;
    }
    const auto import_observation = nlohmann::json::parse(queue.observation_json(import.job_id));
    if (import_observation.at("state") != "succeeded" ||
        import_observation.at("artifacts").front() != "generated://imports/import-one" ||
        import_observation.dump().find("private") != std::string::npos ||
        import_observation.dump().find("secret") != std::string::npos ||
        import_observation.dump().find("\\") != std::string::npos) {
        std::cerr << "bounded observation leaked a local path or missed artifact state\n";
        return 6;
    }
    if (queue.observation_json(import.job_id, 512U).size() > 512U) {
        std::cerr << "bounded observation exceeded its explicit byte limit\n";
        return 19;
    }

    std::atomic<int> cook_attempts{};
    const auto cook_request = request(AssetJobKind::cook, "asset.cook.one", "sha256:cook");
    const auto cook = queue.submit(cook_request, [&](const auto&, auto& context) {
        const auto attempt = cook_attempts.fetch_add(1) + 1;
        static_cast<void>(context.report(0.2F, "cook", "building meshbin"));
        if (attempt == 1) {
            return noemancer::AssetJobExecutionResult{
                .success = false,
                .code = "asset.cook.failed",
                .detail = "Injected Cook failure for retry coverage.",
                .diagnostics = {"first attempt failed"}
            };
        }
        static_cast<void>(context.report(0.9F, "write", "complete"));
        return noemancer::AssetJobExecutionResult{
            .success = true,
            .code = "asset.cook.ok",
            .detail = "Cook completed after retry.",
            .artifact_uris = {"generated://cook-cache/sha256-cook/payload.meshbin"}
        };
    });
    if (!cook.accepted || !wait_for_state(queue, cook.job_id, AssetJobState::failed)) {
        std::cerr << "injected Cook failure did not produce failed state\n";
        return 7;
    }
    const auto failed = queue.snapshot(cook.job_id);
    const auto retried = queue.retry(cook.job_id);
    if (!failed.has_value() || failed->code != "asset.cook.failed" || !retried.accepted ||
        retried.job_id != cook.job_id || !wait_for_state(queue, cook.job_id, AssetJobState::succeeded) ||
        cook_attempts.load() != 2) {
        std::cerr << "failed Cook job did not retry with the same stable ID\n";
        return 8;
    }
    const auto retried_snapshot = queue.snapshot(cook.job_id);
    if (!retried_snapshot.has_value() || retried_snapshot->attempt != 2U ||
        retried_snapshot->artifact_uris.size() != 1U) {
        std::cerr << "retry attempt/recovery evidence is incomplete\n";
        return 9;
    }

    const auto cancel_request = request(AssetJobKind::thumbnail, "asset.thumbnail.cancel", "sha256:cancel");
    const auto cancel_job = queue.submit(cancel_request, [](const auto&, auto& context) {
        while (!context.cancellation_requested()) {
            static_cast<void>(context.report(0.1F, "thumbnail", "waiting"));
            std::this_thread::sleep_for(2ms);
        }
        return noemancer::AssetJobExecutionResult{
            .success = false,
            .cancelled = true,
            .code = "asset.thumbnail.cancelled",
            .detail = "Executor observed cancellation."
        };
    });
    if (!cancel_job.accepted) {
        std::cerr << "cancellation job was not queued\n";
        return 10;
    }
    std::this_thread::sleep_for(10ms);
    const auto cancellation = queue.cancel(cancel_job.job_id);
    if (!cancellation.accepted || !wait_for_state(queue, cancel_job.job_id, AssetJobState::cancelled)) {
        std::cerr << "running job did not transition to cancelled\n";
        return 11;
    }
    const auto cancelled_snapshot = queue.snapshot(cancel_job.job_id);
    if (!cancelled_snapshot.has_value() || !cancelled_snapshot->cancellation_requested) {
        std::cerr << "cancelled job did not expose cancellation evidence\n";
        return 12;
    }

    const auto missing = nlohmann::json::parse(queue.observation_json("asset-job-missing"));
    if (missing.at("valid") || missing.at("code") != "asset.job.not-found") {
        std::cerr << "missing job observation was not a stable bounded receipt\n";
        return 13;
    }
    if (queue.job_count() != 4U) {
        std::cerr << "job retention count was not deterministic\n";
        return 14;
    }

    queue.shutdown();
    queue.shutdown();
    const auto after_shutdown = queue.submit(
        request(AssetJobKind::inspect, "asset.after.shutdown", "sha256:shutdown"), {});
    if (after_shutdown.accepted || after_shutdown.code != "asset.job.shutdown") {
        std::cerr << "shutdown was not idempotent or did not stop new work\n";
        return 15;
    }

    AssetJobQueue shutdown_queue(AssetJobQueueConfig{
        .worker_count = 1U,
        .max_queued_jobs = 2U,
        .max_diagnostics = 2U,
        .max_artifacts = 2U,
        .max_text_bytes = 256U,
        .max_observation_bytes = 1024U
    });
    const auto running_request = request(AssetJobKind::import, "asset.shutdown.running", "sha256:running");
    const auto running = shutdown_queue.submit(running_request, [](const auto&, auto& context) {
        while (!context.cancellation_requested()) {
            static_cast<void>(context.report(0.2F, "blocking", "shutdown coverage"));
            std::this_thread::sleep_for(2ms);
        }
        return noemancer::AssetJobExecutionResult{.cancelled = true};
    });
    if (!running.accepted || !wait_for_state(shutdown_queue, running.job_id, AssetJobState::running)) {
        std::cerr << "shutdown coverage job did not enter running state\n";
        return 16;
    }
    std::atomic<bool> queued_executor_called{};
    const auto queued = shutdown_queue.submit(
        request(AssetJobKind::thumbnail, "asset.shutdown.queued", "sha256:queued"),
        [&](const auto&, auto&) {
            queued_executor_called.store(true);
            return noemancer::AssetJobExecutionResult{.success = true};
        });
    if (!queued.accepted) {
        std::cerr << "shutdown coverage queued job was not accepted\n";
        return 17;
    }
    shutdown_queue.shutdown();
    const auto running_after_shutdown = shutdown_queue.snapshot(running.job_id);
    const auto queued_after_shutdown = shutdown_queue.snapshot(queued.job_id);
    if (!running_after_shutdown.has_value() || !queued_after_shutdown.has_value() ||
        running_after_shutdown->state != AssetJobState::cancelled ||
        queued_after_shutdown->state != AssetJobState::cancelled || queued_executor_called.load()) {
        std::cerr << "shutdown did not cancel running/queued work safely\n";
        return 18;
    }
    return 0;
}
