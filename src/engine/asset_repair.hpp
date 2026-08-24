#pragma once

#include "engine/asset_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

// Asset Browser diagnostics are deliberately plain data.  The service does
// not inspect the filesystem and does not own AssetRegistry or Job Queue
// state; callers provide the current snapshots and, when applying a plan,
// the host that owns the mutation.
struct AssetRepairJobObservation final {
    std::string asset_id;
    std::string kind; // import, inspect, cook, thumbnail, or a host-defined kind
    std::string state; // queued, running, succeeded, failed, cancelled
    std::string input_fingerprint;
    std::uint64_t source_revision{};
    std::string code;
    std::string detail;
};

struct AssetRepairInput final {
    std::vector<AssetRecord> assets;
    std::vector<AssetRepairJobObservation> jobs;
    std::uint64_t registry_revision{};
    std::size_t max_diagnostics{128U};
    std::size_t max_actions{128U};
    std::size_t max_text_bytes{512U};
    std::size_t max_json_bytes{32U * 1024U};
};

struct AssetRepairDiagnostic final {
    std::string code;
    std::string asset_id;
    std::string dependency_id;
    std::string detail;
    std::string input_fingerprint;
    std::uint64_t registry_revision{};
    bool blocking{true};
    std::vector<std::string> related_asset_ids;
};

enum class AssetRepairActionKind : std::uint8_t {
    rescan,
    reimport,
    inspect,
    retry_cook,
    reveal_dependent
};

struct AssetRepairAction final {
    std::string action_id;
    AssetRepairActionKind kind{AssetRepairActionKind::inspect};
    std::string asset_id;
    std::string related_asset_id;
    std::string diagnostic_code;
    std::string input_fingerprint;
    std::uint64_t base_revision{};
};

struct AssetRepairPlan final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string input_fingerprint;
    std::uint64_t registry_revision{};
    std::vector<AssetRepairAction> actions;
    bool actions_truncated{};
};

struct AssetRepairReport final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string input_fingerprint;
    std::uint64_t registry_revision{};
    std::vector<AssetRepairDiagnostic> diagnostics;
    bool diagnostics_truncated{};
    AssetRepairPlan plan;
};

struct AssetRepairHostResult final {
    bool success{};
    bool atomic{};
    bool mutated{};
    std::string code;
    std::string detail;
    std::uint64_t revision_after{};
    std::vector<std::string> applied_action_ids;
    std::vector<std::string> diagnostics;
};

struct AssetRepairHost final {
    // Both callbacks observe the same host authority.  They are called
    // immediately before execute, so a stale Editor/Agent plan is rejected
    // without allowing the host to mutate anything.
    std::function<std::uint64_t()> current_revision;
    std::function<std::string()> current_fingerprint;

    // The host owns all mutation.  It must implement the whole plan as one
    // atomic operation and report that fact in AssetRepairHostResult.  The
    // bool says whether this is validation-only (true) or a real commit.
    std::function<AssetRepairHostResult(const AssetRepairPlan&, bool dry_run)> execute;
};

struct AssetRepairApplyOptions final {
    bool dry_run{true};
    std::uint64_t expected_revision{};
    std::string expected_fingerprint;
    std::size_t max_text_bytes{512U};
    std::size_t max_json_bytes{16U * 1024U};
};

struct AssetRepairReceipt final {
    bool success{};
    bool dry_run{};
    bool mutated{};
    bool atomic{};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string input_fingerprint;
    std::uint64_t base_revision{};
    std::uint64_t revision_after{};
    std::vector<std::string> applied_action_ids;
    std::vector<std::string> diagnostics;
};

[[nodiscard]] std::string asset_repair_action_name(AssetRepairActionKind kind);

// The result is deterministic for equal input snapshots, independent of
// input vector order.  It never reads or writes a filesystem.
[[nodiscard]] AssetRepairReport diagnose_asset_repairs(const AssetRepairInput& input);

[[nodiscard]] AssetRepairReceipt apply_asset_repair_plan(
    const AssetRepairPlan& plan,
    const AssetRepairApplyOptions& options,
    const AssetRepairHost& host = {});

[[nodiscard]] std::string asset_repair_report_json(
    const AssetRepairReport& report,
    std::size_t max_bytes = 0U);

[[nodiscard]] std::string asset_repair_receipt_json(
    const AssetRepairReceipt& receipt,
    std::size_t max_bytes = 0U);

} // namespace noemancer
