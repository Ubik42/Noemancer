#include "engine/asset_job_queue.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <deque>
#include <exception>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMinimumObservationBytes = 512U;
constexpr std::size_t kMaximumWorkers = 64U;
constexpr std::size_t kMaximumQueue = 4096U;
constexpr std::size_t kMaximumText = 16U * 1024U;

std::string bounded_text(const std::string_view value, const std::size_t limit) {
    if (limit == 0U) return {};
    if (value.size() <= limit) return std::string(value);
    if (limit <= 3U) return std::string(value.substr(0U, limit));
    std::string result(value.substr(0U, limit - 3U));
    result += "...";
    return result;
}

bool contains_path_detail(const std::string_view value) {
    if (value.empty()) return false;
    if (value.front() == '/' || value.starts_with("./") || value.starts_with("../")) return true;
    if (value.find("\\") != std::string_view::npos) return true;
    const bool safe_observation_uri = value.starts_with("generated://") ||
        value.starts_with("asset://") || value.starts_with("cache://") ||
        value.starts_with("artifact://") || value.starts_with("http://") ||
        value.starts_with("https://");
    if (value.find('/') != std::string_view::npos && !safe_observation_uri) return true;
    for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
        if (value[index] == ':' && value[index + 1U] == '/' &&
            (index + 2U >= value.size() || value[index + 2U] != '/') &&
            std::isalpha(static_cast<unsigned char>(value[index - 1U])) != 0) return true;
    }
    return false;
}

std::string observation_text(const std::string_view value, const std::size_t limit) {
    if (contains_path_detail(value)) return "<path-redacted>";
    return bounded_text(value, limit);
}

std::string observation_uri(const std::string_view value, const std::size_t limit) {
    // Generated/cache/asset URIs are useful to an Agent. Physical paths are
    // deliberately excluded from this boundary.
    if (contains_path_detail(value)) return "<path-redacted>";
    return bounded_text(value, limit);
}

std::uint64_t fnv1a_append(std::uint64_t hash, const std::uint64_t value) {
    std::uint64_t result = hash;
    for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
        result ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
        result *= 1099511628211ULL;
    }
    return result;
}

std::uint64_t fnv1a_append(std::uint64_t hash, const std::string_view value) {
    std::uint64_t result = fnv1a_append(hash, static_cast<std::uint64_t>(value.size()));
    for (const auto byte : value) {
        result ^= static_cast<std::uint8_t>(byte);
        result *= 1099511628211ULL;
    }
    return result;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

float clamp_progress(const float progress) {
    if (!std::isfinite(progress)) return 0.0F;
    return std::clamp(progress, 0.0F, 1.0F);
}

} // namespace

struct AssetJobQueue::SharedState final {
    struct Job final {
        AssetJobRequest request;
        AssetJobExecutor executor;
        std::string id;
        AssetJobState state{AssetJobState::queued};
        std::uint64_t revision{1U};
        std::uint32_t attempt{1U};
        float progress{};
        bool cancellation_requested{};
        std::atomic<bool> cancel_flag{false};
        std::string stage{"queued"};
        std::string code;
        std::string detail;
        std::vector<std::string> diagnostics;
        std::vector<std::string> artifact_uris;
        bool diagnostics_truncated{};
        bool artifacts_truncated{};
    };

    explicit SharedState(AssetJobQueueConfig value) : config(std::move(value)) {
        config.worker_count = std::clamp(config.worker_count, std::size_t{1U}, kMaximumWorkers);
        config.max_queued_jobs = std::clamp(config.max_queued_jobs, std::size_t{1U}, kMaximumQueue);
        config.max_diagnostics = std::min(config.max_diagnostics, std::size_t{256U});
        config.max_artifacts = std::min(config.max_artifacts, std::size_t{256U});
        config.max_text_bytes = std::clamp(config.max_text_bytes, std::size_t{32U}, kMaximumText);
        config.max_observation_bytes = std::max(config.max_observation_bytes, kMinimumObservationBytes);
    }

    AssetJobQueueConfig config;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> pending;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    bool accepting{true};
    bool stopping{};
};

bool AssetJobContext::cancellation_requested() const {
    return cancellation_ && cancellation_();
}

bool AssetJobContext::report(
    const float progress,
    const std::string_view stage,
    const std::string_view diagnostic) const {
    return report_ && report_(progress, stage, diagnostic);
}

AssetJobQueue::AssetJobQueue(const AssetJobQueueConfig config)
    : state_(std::make_shared<SharedState>(config)) {
    try {
        workers_.reserve(state_->config.worker_count);
        for (std::size_t index = 0U; index < state_->config.worker_count; ++index) {
            workers_.emplace_back([state = state_] { worker_loop(state); });
        }
    } catch (...) {
        {
            std::lock_guard lock(state_->mutex);
            state_->accepting = false;
            state_->stopping = true;
        }
        state_->wake.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        throw;
    }
}

AssetJobQueue::~AssetJobQueue() {
    shutdown();
}

std::string AssetJobQueue::kind_name(const AssetJobKind kind) {
    switch (kind) {
    case AssetJobKind::import: return "import";
    case AssetJobKind::inspect: return "inspect";
    case AssetJobKind::cook: return "cook";
    case AssetJobKind::thumbnail: return "thumbnail";
    }
    return "unknown";
}

std::string AssetJobQueue::state_name(const AssetJobState state) {
    switch (state) {
    case AssetJobState::queued: return "queued";
    case AssetJobState::running: return "running";
    case AssetJobState::succeeded: return "succeeded";
    case AssetJobState::failed: return "failed";
    case AssetJobState::cancelled: return "cancelled";
    }
    return "unknown";
}

std::string AssetJobQueue::stable_job_id(const AssetJobRequest& request) {
    // The source URI and prose description are intentionally excluded. A
    // relocation or wording change must not create a second job for the same
    // content/plan, while the content fingerprint and profile remain part of
    // the identity.
    std::uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a_append(hash, "noemancer.asset-job/0.1");
    hash = fnv1a_append(hash, static_cast<std::uint64_t>(request.kind));
    hash = fnv1a_append(hash, request.asset_id);
    hash = fnv1a_append(hash, request.input_fingerprint);
    hash = fnv1a_append(hash, request.target_profile);
    hash = fnv1a_append(hash, request.source_revision);
    hash = fnv1a_append(hash, request.plan_payload);
    return "asset-job-" + kind_name(request.kind) + "-" + hex_u64(hash);
}

AssetJobOperationResult AssetJobQueue::submit(
    const AssetJobRequest& request,
    AssetJobExecutor executor) {
    AssetJobOperationResult result;
    result.job_id = stable_job_id(request);
    if (request.asset_id.empty()) {
        result.code = "asset.job.invalid-request";
        result.detail = "asset_id is required.";
        return result;
    }

    std::shared_ptr<SharedState::Job> job;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->accepting || state_->stopping) {
            result.code = "asset.job.shutdown";
            result.detail = "The asset Job queue is shutting down.";
            return result;
        }
        if (state_->jobs.contains(result.job_id)) {
            result.duplicate = true;
            result.code = "asset.job.duplicate";
            result.detail = "A job with the same content identity already exists.";
            return result;
        }
        if (state_->pending.size() >= state_->config.max_queued_jobs) {
            result.code = "asset.job.queue-full";
            result.detail = "The bounded asset Job queue is full.";
            return result;
        }
        job = std::make_shared<SharedState::Job>();
        job->request = request;
        job->executor = std::move(executor);
        job->id = result.job_id;
        state_->jobs.emplace(job->id, job);
        state_->pending.push_back(job->id);
        result.accepted = true;
        result.code = "asset.job.queued";
        result.detail = "Asset Job queued.";
    }
    state_->wake.notify_one();
    return result;
}

AssetJobOperationResult AssetJobQueue::retry(const std::string_view job_id) {
    AssetJobOperationResult result;
    result.job_id = std::string(job_id);
    std::lock_guard lock(state_->mutex);
    const auto found = state_->jobs.find(std::string(job_id));
    if (found == state_->jobs.end()) {
        result.code = "asset.job.not-found";
        result.detail = "The requested asset Job does not exist.";
        return result;
    }
    const auto& job = found->second;
    if (!state_->accepting || state_->stopping) {
        result.code = "asset.job.shutdown";
        result.detail = "The asset Job queue is shutting down.";
        return result;
    }
    if (job->state != AssetJobState::failed) {
        result.code = "asset.job.retry-not-allowed";
        result.detail = "Only a failed terminal Job can be retried.";
        return result;
    }
    if (state_->pending.size() >= state_->config.max_queued_jobs) {
        result.code = "asset.job.queue-full";
        result.detail = "The bounded asset Job queue is full.";
        return result;
    }
    job->state = AssetJobState::queued;
    job->revision++;
    job->attempt++;
    job->progress = 0.0F;
    job->cancellation_requested = false;
    job->cancel_flag.store(false, std::memory_order_release);
    job->stage = "queued";
    job->code.clear();
    job->detail.clear();
    job->diagnostics.clear();
    job->artifact_uris.clear();
    job->diagnostics_truncated = false;
    job->artifacts_truncated = false;
    state_->pending.push_back(job->id);
    result.accepted = true;
    result.code = "asset.job.retry-queued";
    result.detail = "Failed asset Job queued for retry.";
    state_->wake.notify_one();
    return result;
}

AssetJobOperationResult AssetJobQueue::cancel(const std::string_view job_id) {
    AssetJobOperationResult result;
    result.job_id = std::string(job_id);
    std::lock_guard lock(state_->mutex);
    const auto found = state_->jobs.find(std::string(job_id));
    if (found == state_->jobs.end()) {
        result.code = "asset.job.not-found";
        result.detail = "The requested asset Job does not exist.";
        return result;
    }
    const auto& job = found->second;
    if (job->state == AssetJobState::succeeded || job->state == AssetJobState::failed ||
        job->state == AssetJobState::cancelled) {
        result.code = "asset.job.terminal";
        result.detail = "A terminal asset Job cannot be cancelled.";
        return result;
    }
    job->cancel_flag.store(true, std::memory_order_release);
    job->cancellation_requested = true;
    job->revision++;
    if (job->state == AssetJobState::queued) {
        job->state = AssetJobState::cancelled;
        job->stage = "cancelled";
        job->code = "asset.job.cancelled";
        job->detail = "Asset Job cancelled before execution.";
        state_->pending.erase(std::remove(state_->pending.begin(), state_->pending.end(), job->id),
            state_->pending.end());
    } else {
        job->stage = "cancelling";
        job->code = "asset.job.cancel-requested";
        job->detail = "Cancellation requested; executor is being allowed to stop.";
    }
    result.accepted = true;
    result.code = job->state == AssetJobState::cancelled ? "asset.job.cancelled" : "asset.job.cancel-requested";
    result.detail = "Asset Job cancellation accepted.";
    return result;
}

std::optional<AssetJobSnapshot> AssetJobQueue::snapshot(const std::string_view job_id) const {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->jobs.find(std::string(job_id));
    if (found == state_->jobs.end()) return std::nullopt;
    const auto& job = found->second;
    AssetJobSnapshot snapshot;
    snapshot.job_id = job->id;
    snapshot.kind = job->request.kind;
    snapshot.state = job->state;
    snapshot.asset_id = job->request.asset_id;
    snapshot.input_fingerprint = job->request.input_fingerprint;
    snapshot.target_profile = job->request.target_profile;
    snapshot.source_revision = job->request.source_revision;
    snapshot.revision = job->revision;
    snapshot.attempt = job->attempt;
    snapshot.progress = job->progress;
    snapshot.cancellation_requested = job->cancellation_requested;
    snapshot.stage = job->stage;
    snapshot.code = job->code;
    snapshot.detail = job->detail;
    snapshot.diagnostics = job->diagnostics;
    snapshot.artifact_uris = job->artifact_uris;
    snapshot.diagnostics_truncated = job->diagnostics_truncated;
    snapshot.artifacts_truncated = job->artifacts_truncated;
    return snapshot;
}

std::string AssetJobQueue::observation_json(
    const std::string_view job_id,
    const std::size_t max_bytes) const {
    const auto limit = std::max(max_bytes == 0U ? state_->config.max_observation_bytes : max_bytes,
        kMinimumObservationBytes);
    const auto current = snapshot(job_id);
    if (!current.has_value()) {
        const Json missing = {
            {"schemaVersion", "noemancer.asset-job-observation/0.1"},
            {"valid", false},
            {"jobId", bounded_text(job_id, 128U)},
            {"code", "asset.job.not-found"},
            {"detail", "The requested asset Job does not exist."}
        };
        return missing.dump();
    }
    const auto& value = *current;
    Json diagnostics = Json::array();
    for (const auto& diagnostic : value.diagnostics) {
        diagnostics.push_back(observation_text(diagnostic, state_->config.max_text_bytes));
    }
    Json artifacts = Json::array();
    for (const auto& artifact : value.artifact_uris) {
        artifacts.push_back(observation_uri(artifact, state_->config.max_text_bytes));
    }
    const Json observation = {
        {"schemaVersion", "noemancer.asset-job-observation/0.1"},
        {"valid", true},
        {"jobId", value.job_id},
        {"kind", kind_name(value.kind)},
        {"state", state_name(value.state)},
        {"assetId", observation_text(value.asset_id, state_->config.max_text_bytes)},
        {"inputFingerprint", observation_text(value.input_fingerprint, state_->config.max_text_bytes)},
        {"targetProfile", observation_text(value.target_profile, state_->config.max_text_bytes)},
        {"sourceRevision", value.source_revision},
        {"revision", value.revision},
        {"attempt", value.attempt},
        {"progress", value.progress},
        {"stage", observation_text(value.stage, state_->config.max_text_bytes)},
        {"cancellationRequested", value.cancellation_requested},
        {"code", observation_text(value.code, state_->config.max_text_bytes)},
        {"detail", observation_text(value.detail, state_->config.max_text_bytes)},
        {"diagnostics", std::move(diagnostics)},
        {"artifacts", std::move(artifacts)},
        {"diagnosticsTruncated", value.diagnostics_truncated},
        {"artifactsTruncated", value.artifacts_truncated}
    };
    auto serialized = observation.dump();
    if (serialized.size() <= limit) return serialized;
    const Json bounded = {
        {"schemaVersion", "noemancer.asset-job-observation/0.1"},
        {"valid", true},
        {"jobId", value.job_id},
        {"kind", kind_name(value.kind)},
        {"state", state_name(value.state)},
        {"assetId", observation_text(value.asset_id, 64U)},
        {"revision", value.revision},
        {"attempt", value.attempt},
        {"progress", value.progress},
        {"stage", observation_text(value.stage, 64U)},
        {"code", observation_text(value.code, 64U)},
        {"detail", observation_text(value.detail, 128U)},
        {"diagnostics", Json::array()},
        {"artifacts", Json::array()},
        {"truncated", true}
    };
    serialized = bounded.dump();
    if (serialized.size() > limit) {
        serialized = Json{
            {"schemaVersion", "noemancer.asset-job-observation/0.1"},
            {"valid", true},
            {"jobId", bounded_text(value.job_id, 64U)},
            {"state", state_name(value.state)},
            {"revision", value.revision},
            {"truncated", true}
        }.dump();
    }
    return serialized;
}

std::size_t AssetJobQueue::job_count() const {
    std::lock_guard lock(state_->mutex);
    return state_->jobs.size();
}

void AssetJobQueue::worker_loop(const std::shared_ptr<SharedState>& state) {
    while (true) {
        std::shared_ptr<SharedState::Job> job;
        {
            std::unique_lock lock(state->mutex);
            state->wake.wait(lock, [&] {
                return state->stopping || !state->pending.empty();
            });
            if (state->pending.empty()) {
                if (state->stopping) return;
                continue;
            }
            const auto job_id = std::move(state->pending.front());
            state->pending.pop_front();
            const auto found = state->jobs.find(job_id);
            if (found == state->jobs.end() || found->second->state != AssetJobState::queued) continue;
            job = found->second;
            if (job->cancel_flag.load(std::memory_order_acquire)) {
                job->state = AssetJobState::cancelled;
                job->cancellation_requested = true;
                job->revision++;
                job->stage = "cancelled";
                job->code = "asset.job.cancelled";
                job->detail = "Asset Job cancelled before execution.";
                continue;
            }
            job->state = AssetJobState::running;
            job->revision++;
            job->progress = 0.0F;
            job->stage = "starting";
            job->code = "asset.job.running";
            job->detail = "Asset Job executor started.";
        }

        AssetJobExecutionResult execution;
        AssetJobContext context;
        context.cancellation_ = [job] {
            return job->cancel_flag.load(std::memory_order_acquire);
        };
        context.report_ = [state, job](
            const float progress,
            const std::string_view stage,
            const std::string_view diagnostic) {
            std::lock_guard lock(state->mutex);
            if (job->state != AssetJobState::running || job->cancel_flag.load(std::memory_order_acquire)) {
                return false;
            }
            job->progress = clamp_progress(progress);
            job->stage = bounded_text(stage, state->config.max_text_bytes);
            if (!diagnostic.empty()) {
                if (state->config.max_diagnostics == 0U) {
                    job->diagnostics_truncated = true;
                } else {
                    if (job->diagnostics.size() >= state->config.max_diagnostics) {
                        job->diagnostics.erase(job->diagnostics.begin());
                        job->diagnostics_truncated = true;
                    }
                    job->diagnostics.push_back(bounded_text(diagnostic, state->config.max_text_bytes));
                }
            }
            job->revision++;
            return true;
        };

        try {
            if (!job->executor) {
                execution.code = "asset.job.executor-missing";
                execution.detail = "No executor callback was injected for this asset Job.";
            } else if (job->cancel_flag.load(std::memory_order_acquire)) {
                execution.cancelled = true;
            } else {
                execution = job->executor(job->request, context);
            }
        } catch (const std::exception& error) {
            execution.code = "asset.job.executor-exception";
            execution.detail = error.what();
        } catch (...) {
            execution.code = "asset.job.executor-exception";
            execution.detail = "Asset Job executor threw a non-standard exception.";
        }

        {
            std::lock_guard lock(state->mutex);
            if (!execution.diagnostics.empty()) {
                for (const auto& diagnostic : execution.diagnostics) {
                    if (state->config.max_diagnostics == 0U) {
                        job->diagnostics_truncated = true;
                        break;
                    }
                    if (job->diagnostics.size() >= state->config.max_diagnostics) {
                        job->diagnostics.erase(job->diagnostics.begin());
                        job->diagnostics_truncated = true;
                    }
                    job->diagnostics.push_back(bounded_text(diagnostic, state->config.max_text_bytes));
                }
            }
            const bool cancelled = execution.cancelled || job->cancel_flag.load(std::memory_order_acquire);
            job->cancellation_requested = job->cancellation_requested || cancelled;
            if (cancelled) {
                job->state = AssetJobState::cancelled;
                job->stage = "cancelled";
                job->code = state->stopping ? "asset.job.shutdown" : "asset.job.cancelled";
                job->detail = state->stopping
                    ? "Asset Job cancelled during queue shutdown."
                    : "Asset Job cancelled by request.";
                job->artifact_uris.clear();
            } else if (execution.success) {
                job->state = AssetJobState::succeeded;
                job->progress = 1.0F;
                job->stage = "completed";
                job->code = execution.code.empty() ? "ok" : bounded_text(execution.code, state->config.max_text_bytes);
                job->detail = execution.detail.empty()
                    ? "Asset Job completed successfully."
                    : bounded_text(execution.detail, state->config.max_text_bytes);
                for (const auto& artifact : execution.artifact_uris) {
                    if (state->config.max_artifacts == 0U) {
                        job->artifacts_truncated = true;
                        break;
                    }
                    if (job->artifact_uris.size() >= state->config.max_artifacts) {
                        job->artifacts_truncated = true;
                        break;
                    }
                    job->artifact_uris.push_back(bounded_text(artifact, state->config.max_text_bytes));
                }
            } else {
                job->state = AssetJobState::failed;
                job->stage = "failed";
                job->code = execution.code.empty() ? "asset.job.executor-failed" :
                    bounded_text(execution.code, state->config.max_text_bytes);
                job->detail = execution.detail.empty()
                    ? "Asset Job executor reported failure."
                    : bounded_text(execution.detail, state->config.max_text_bytes);
            }
            job->revision++;
        }
    }
}

void AssetJobQueue::shutdown() noexcept {
    // Only one caller owns the join phase. A callback running on a worker may
    // call shutdown itself; a second caller then returns instead of waiting
    // on a lifecycle lock that the worker can never release.
    if (shutdown_claimed_.exchange(true, std::memory_order_acq_rel)) return;
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->stopping) {
            state_->accepting = false;
            state_->stopping = true;
            for (const auto& job_id : state_->pending) {
                const auto found = state_->jobs.find(job_id);
                if (found == state_->jobs.end()) continue;
                const auto& job = found->second;
                job->cancel_flag.store(true, std::memory_order_release);
                job->cancellation_requested = true;
                job->state = AssetJobState::cancelled;
                job->stage = "cancelled";
                job->code = "asset.job.shutdown";
                job->detail = "Asset Job cancelled during queue shutdown.";
                job->revision++;
            }
            state_->pending.clear();
            for (const auto& [unused_id, job] : state_->jobs) {
                static_cast<void>(unused_id);
                if (job->state == AssetJobState::running) {
                    job->cancel_flag.store(true, std::memory_order_release);
                    job->cancellation_requested = true;
                    job->revision++;
                }
            }
        }
    }
    state_->wake.notify_all();
    const auto caller = std::this_thread::get_id();
    for (auto& worker : workers_) {
        if (!worker.joinable()) continue;
        if (worker.get_id() == caller) {
            // The worker owns a shared state capture, so detaching here is
            // safe even if a callback destroys the queue from that worker.
            worker.detach();
        } else {
            worker.join();
        }
    }
    workers_.clear();
}

} // namespace noemancer
