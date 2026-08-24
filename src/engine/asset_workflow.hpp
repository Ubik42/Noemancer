#pragma once

#include "engine/asset_job_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace noemancer {

// Configuration for the real Asset Browser Import/Inspect executor.  The
// executor owns no registry state: each invocation builds an isolated
// AssetRegistry snapshot from these roots and only returns plain Job data.
struct AssetWorkflowConfig final {
    std::vector<std::filesystem::path> asset_roots;
    std::filesystem::path artifact_root;
    std::size_t max_artifact_bytes{32U * 1024U * 1024U};
    std::size_t max_diagnostics{8U};
    std::size_t max_diagnostic_bytes{512U};
};

// Creates a worker-safe executor for AssetJobKind::import and
// AssetJobKind::inspect.  Cook and thumbnail remain owned by their dedicated
// pipelines.  The returned function is safe to copy into AssetJobQueue and
// does not retain pointers into an Editor/AssetRegistry instance.
// `AssetJobRequest::source_revision` is carried as provenance only: an
// isolated AssetRegistry starts its own local revision sequence, so the
// live Editor authority must perform any optimistic revision comparison
// before committing a result.  This executor validates the source URI and
// content fingerprint against its isolated snapshot.
[[nodiscard]] AssetJobExecutor make_asset_workflow_executor(
    AssetWorkflowConfig config);

// Plain service facade useful to hosts that want to keep the configuration as
// an explicit value while injecting the same executor into several queues.
class AssetWorkflowService final {
public:
    explicit AssetWorkflowService(AssetWorkflowConfig config);

    [[nodiscard]] AssetJobExecutor executor() const;
    [[nodiscard]] const AssetWorkflowConfig& config() const noexcept;

private:
    AssetWorkflowConfig config_;
};

} // namespace noemancer
