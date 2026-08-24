#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace noemancer {

enum class AssetJobKind : std::uint8_t {
    import,
    inspect,
    cook,
    thumbnail
};

enum class AssetJobState : std::uint8_t {
    queued,
    running,
    succeeded,
    failed,
    cancelled
};

struct AssetJobRequest final {
    AssetJobKind kind{AssetJobKind::inspect};
    std::string asset_id;

    // These fields are handed to the injected executor only. They are not
    // emitted by observation_json, so a worker cannot accidentally expose
    // local filesystem details to an Agent/UI observer.
    std::string source_uri;
    std::string plan_payload;

    // A content or plan fingerprint makes the job identity stable without
    // depending on a process-local counter or a worker/thread identifier.
    std::string input_fingerprint;
    std::string target_profile;
    std::uint64_t source_revision{};
    std::string description;
};

struct AssetJobExecutionResult final {
    bool success{};
    bool cancelled{};
    std::string code;
    std::string detail;
    std::vector<std::string> diagnostics;
    std::vector<std::string> artifact_uris;
};

class AssetJobContext final {
public:
    AssetJobContext() = default;

    [[nodiscard]] bool cancellation_requested() const;

    // Progress is clamped to [0, 1]. The report is rejected once the job is
    // terminal or cancellation has been requested.
    [[nodiscard]] bool report(
        float progress,
        std::string_view stage,
        std::string_view diagnostic = {}) const;

private:
    using CancellationFn = std::function<bool()>;
    using ReportFn = std::function<bool(float, std::string_view, std::string_view)>;

    CancellationFn cancellation_;
    ReportFn report_;

    friend class AssetJobQueue;
};

using AssetJobExecutor = std::function<AssetJobExecutionResult(
    const AssetJobRequest&, AssetJobContext&)>;

struct AssetJobQueueConfig final {
    std::size_t worker_count{1};
    std::size_t max_queued_jobs{64};
    std::size_t max_diagnostics{16};
    std::size_t max_artifacts{16};
    std::size_t max_text_bytes{512};
    std::size_t max_observation_bytes{16U * 1024U};
};

struct AssetJobOperationResult final {
    bool accepted{};
    bool duplicate{};
    std::string job_id;
    std::string code;
    std::string detail;
};

struct AssetJobSnapshot final {
    std::string job_id;
    AssetJobKind kind{AssetJobKind::inspect};
    AssetJobState state{AssetJobState::queued};
    std::string asset_id;
    std::string input_fingerprint;
    std::string target_profile;
    std::uint64_t source_revision{};
    std::uint64_t revision{};
    std::uint32_t attempt{1};
    float progress{};
    bool cancellation_requested{};
    std::string stage;
    std::string code;
    std::string detail;
    std::vector<std::string> diagnostics;
    std::vector<std::string> artifact_uris;
    bool diagnostics_truncated{};
    bool artifacts_truncated{};
};

class AssetJobQueue final {
public:
    explicit AssetJobQueue(AssetJobQueueConfig config = {});
    ~AssetJobQueue();

    AssetJobQueue(const AssetJobQueue&) = delete;
    AssetJobQueue& operator=(const AssetJobQueue&) = delete;
    AssetJobQueue(AssetJobQueue&&) = delete;
    AssetJobQueue& operator=(AssetJobQueue&&) = delete;

    // A duplicate request returns the same stable job ID and does not enqueue
    // a second execution. Use retry() after a failed terminal job.
    [[nodiscard]] AssetJobOperationResult submit(
        const AssetJobRequest& request,
        AssetJobExecutor executor = {});

    [[nodiscard]] AssetJobOperationResult retry(std::string_view job_id);
    [[nodiscard]] AssetJobOperationResult cancel(std::string_view job_id);

    [[nodiscard]] std::optional<AssetJobSnapshot> snapshot(
        std::string_view job_id) const;

    // This is the bounded Agent/UI observation surface. It intentionally
    // omits source_uri, plan_payload, descriptions, worker IDs and paths.
    [[nodiscard]] std::string observation_json(
        std::string_view job_id,
        std::size_t max_bytes = 0) const;

    [[nodiscard]] std::size_t job_count() const;

    // Idempotent. Queued jobs become cancelled immediately; running jobs are
    // asked to stop through AssetJobContext before workers are joined.
    void shutdown() noexcept;

    [[nodiscard]] static std::string kind_name(AssetJobKind kind);
    [[nodiscard]] static std::string state_name(AssetJobState state);

private:
    struct SharedState;

    static void worker_loop(const std::shared_ptr<SharedState>& state);
    [[nodiscard]] static std::string stable_job_id(const AssetJobRequest& request);

    std::shared_ptr<SharedState> state_;
    std::vector<std::thread> workers_;
    std::atomic<bool> shutdown_claimed_{};
};

} // namespace noemancer
