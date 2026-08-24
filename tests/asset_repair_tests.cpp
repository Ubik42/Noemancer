#include "engine/asset_repair.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

using noemancer::AssetRecord;
using noemancer::AssetRepairActionKind;
using noemancer::AssetRepairHost;
using noemancer::AssetRepairHostResult;
using noemancer::AssetRepairInput;
using noemancer::AssetRepairJobObservation;
using noemancer::AssetRepairApplyOptions;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "asset_repair check failed at line " << __LINE__ << ": " #condition "\n"; \
        return __LINE__; \
    } \
} while (false)

AssetRecord asset(std::string id, std::vector<std::string> dependencies = {},
    bool available = true, std::string import_state = "ready") {
    return AssetRecord{
        .id = std::move(id),
        .display_name = "Test Asset",
        .kind = "Texture",
        .uri = "asset://test",
        .source_root = "assets",
        .relative_path = "test.bin",
        .extension = ".bin",
        .content_hash = "sha256:test",
        .hash_provenance = "fixture",
        .license = "CC0",
        .redistribution = "allowed",
        .import_state = std::move(import_state),
        .source_bytes = 4U,
        .optional = false,
        .available = available,
        .tags = {},
        .dependencies = std::move(dependencies)
    };
}

bool has_code(const noemancer::AssetRepairReport& report, const std::string_view code) {
    return std::ranges::any_of(report.diagnostics, [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

bool has_action(const noemancer::AssetRepairReport& report, const AssetRepairActionKind kind) {
    return std::ranges::any_of(report.plan.actions, [kind](const auto& action) {
        return action.kind == kind;
    });
}

} // namespace

int main() {
    AssetRepairInput input;
    input.registry_revision = 7U;
    input.assets = {
        asset("asset.gone", {}, false),
        asset("asset.root", {"asset.mid"}),
        asset("asset.mid", {"asset.missing"}),
        asset("asset.import-failed", {}, true, "failed"),
        asset("asset.cook-failed", {}, true, "cook-failed"),
        asset("asset.cycle-a", {"asset.cycle-b"}),
        asset("asset.cycle-b", {"asset.cycle-a"})
    };
    input.jobs = {
        AssetRepairJobObservation{
            .asset_id = "asset.job-import",
            .kind = "import",
            .state = "failed",
            .input_fingerprint = "sha256:job",
            .source_revision = 7U,
            .code = "asset.import.failed",
            .detail = "Importer reported a recoverable failure."
        },
        AssetRepairJobObservation{
            .asset_id = "asset.job-cook",
            .kind = "cook",
            .state = "failed",
            .input_fingerprint = "sha256:job-cook",
            .source_revision = 7U,
            .code = "asset.cook.failed",
            .detail = "Cooker reported a recoverable failure."
        }
    };

    const auto report = noemancer::diagnose_asset_repairs(input);
    CHECK(report.valid);
    CHECK(report.registry_revision == 7U);
    CHECK(!report.input_fingerprint.empty());
    CHECK(has_code(report, "asset.source-unavailable"));
    CHECK(has_code(report, "asset.dependency-missing"));
    CHECK(has_code(report, "asset.dependency-cycle"));
    CHECK(has_code(report, "asset.import-failed"));
    CHECK(has_code(report, "asset.cook-failed"));
    CHECK(has_action(report, AssetRepairActionKind::rescan));
    CHECK(has_action(report, AssetRepairActionKind::reimport));
    CHECK(has_action(report, AssetRepairActionKind::retry_cook));
    CHECK(has_action(report, AssetRepairActionKind::inspect));
    CHECK(has_action(report, AssetRepairActionKind::reveal_dependent));

    auto reordered = input;
    std::reverse(reordered.assets.begin(), reordered.assets.end());
    std::reverse(reordered.jobs.begin(), reordered.jobs.end());
    const auto reordered_report = noemancer::diagnose_asset_repairs(reordered);
    CHECK(reordered_report.input_fingerprint == report.input_fingerprint);
    CHECK(reordered_report.plan.plan_id == report.plan.plan_id);
    CHECK(noemancer::asset_repair_report_json(report, 0U) ==
        noemancer::asset_repair_report_json(reordered_report, 0U));

    const auto report_json = nlohmann::json::parse(noemancer::asset_repair_report_json(report, 1024U));
    CHECK(report_json.at("schemaVersion") == "noemancer.asset-repair-report/0.1");
    CHECK(report_json.at("plan").at("planId") == report.plan.plan_id);

    std::size_t host_calls = 0U;
    AssetRepairHost dry_run_host{
        .current_revision = [] { return std::uint64_t{7U}; },
        .current_fingerprint = [fingerprint = report.input_fingerprint] { return fingerprint; },
        .execute = [&host_calls](const auto& plan, const bool dry_run) {
            ++host_calls;
            return AssetRepairHostResult{
                .success = dry_run,
                .atomic = true,
                .mutated = false,
                .code = "asset.repair.validated",
                .detail = "Host validation passed.",
                .revision_after = plan.registry_revision
            };
        }
    };
    const auto dry_run = noemancer::apply_asset_repair_plan(
        report.plan,
        AssetRepairApplyOptions{
            .dry_run = true,
            .expected_revision = 7U,
            .expected_fingerprint = report.input_fingerprint
        },
        dry_run_host);
    CHECK(dry_run.success);
    CHECK(dry_run.dry_run);
    CHECK(!dry_run.mutated);
    CHECK(host_calls == 1U);

    std::size_t stale_host_calls = 0U;
    const auto stale = noemancer::apply_asset_repair_plan(
        report.plan,
        AssetRepairApplyOptions{
            .dry_run = false,
            .expected_revision = 7U,
            .expected_fingerprint = report.input_fingerprint
        },
        AssetRepairHost{
            .current_revision = [] { return std::uint64_t{8U}; },
            .current_fingerprint = {},
            .execute = [&stale_host_calls](const auto&, bool) {
                ++stale_host_calls;
                return AssetRepairHostResult{.success = true, .atomic = true, .mutated = true};
            }
        });
    CHECK(!stale.success);
    CHECK(stale.code == "asset.repair.revision-conflict");
    CHECK(stale_host_calls == 0U);

    std::vector<std::string> applied;
    const auto committed = noemancer::apply_asset_repair_plan(
        report.plan,
        AssetRepairApplyOptions{
            .dry_run = false,
            .expected_revision = 7U,
            .expected_fingerprint = report.input_fingerprint
        },
        AssetRepairHost{
            .current_revision = [] { return std::uint64_t{7U}; },
            .current_fingerprint = [fingerprint = report.input_fingerprint] { return fingerprint; },
            .execute = [&applied](const auto& plan, const bool dry_run) {
                if (dry_run) return AssetRepairHostResult{
                    .success = false,
                    .atomic = true,
                    .mutated = false,
                    .code = "asset.repair.unexpected-dry-run"
                };
                for (const auto& action : plan.actions) applied.push_back(action.action_id);
                return AssetRepairHostResult{
                    .success = true,
                    .atomic = true,
                    .mutated = true,
                    .code = "ok",
                    .detail = "Host committed the complete plan.",
                    .revision_after = 8U,
                    .applied_action_ids = applied
                };
            }
        });
    CHECK(committed.success);
    CHECK(committed.mutated);
    CHECK(committed.atomic);
    CHECK(committed.revision_after == 8U);
    CHECK(committed.applied_action_ids.size() == report.plan.actions.size());

    const auto non_atomic = noemancer::apply_asset_repair_plan(
        report.plan,
        AssetRepairApplyOptions{
            .dry_run = false,
            .expected_revision = 7U,
            .expected_fingerprint = report.input_fingerprint
        },
        AssetRepairHost{
            .current_revision = [] { return std::uint64_t{7U}; },
            .execute = [](const auto& plan, bool) {
                std::vector<std::string> ids;
                for (const auto& action : plan.actions) ids.push_back(action.action_id);
                return AssetRepairHostResult{
                    .success = true,
                    .atomic = false,
                    .mutated = true,
                    .applied_action_ids = std::move(ids)
                };
            }
        });
    CHECK(!non_atomic.success);
    CHECK(non_atomic.code == "asset.repair.non-atomic-host");

    const auto receipt_json = nlohmann::json::parse(noemancer::asset_repair_receipt_json(dry_run, 0U));
    CHECK(receipt_json.at("schemaVersion") == "noemancer.asset-repair-receipt/0.1");
    return 0;
}
