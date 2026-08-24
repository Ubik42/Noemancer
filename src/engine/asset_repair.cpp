#include "engine/asset_repair.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumItems = 4096U;
constexpr std::size_t kMaximumText = 16U * 1024U;
constexpr std::size_t kMinimumJsonBytes = 512U;

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string bounded_text(const std::string_view value, const std::size_t requested_limit) {
    const auto limit = std::clamp(requested_limit, std::size_t{32U}, kMaximumText);
    if (value.size() <= limit) return std::string(value);
    if (limit <= 3U) return std::string(value.substr(0U, limit));
    std::string result(value.substr(0U, limit - 3U));
    result += "...";
    return result;
}

bool looks_like_local_path(const std::string_view value) {
    if (value.empty()) return false;
    if (value.front() == '/' || value.starts_with("./") || value.starts_with("../") ||
        value.find('\\') != std::string_view::npos) return true;
    if (value.size() > 2U && std::isalpha(static_cast<unsigned char>(value.front())) != 0 &&
        value[1U] == ':') return true;
    return false;
}

std::string safe_detail(const std::string_view value, const std::size_t limit) {
    if (looks_like_local_path(value)) return "<path-redacted>";
    return bounded_text(value, limit);
}

std::uint64_t fnv1a_append(std::uint64_t hash, const std::string_view value) {
    std::uint64_t result = hash;
    for (const auto byte : value) {
        result ^= static_cast<std::uint8_t>(byte);
        result *= 1099511628211ULL;
    }
    result ^= 0xffU;
    result *= 1099511628211ULL;
    return result;
}

std::uint64_t fnv1a_append(std::uint64_t hash, const std::uint64_t value) {
    std::uint64_t result = hash;
    for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
        result ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
        result *= 1099511628211ULL;
    }
    result ^= 0xfeU;
    result *= 1099511628211ULL;
    return result;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::string digest(const std::string_view value) {
    return "fnv1a64:" + hex_u64(fnv1a_append(1469598103934665603ULL, value));
}

template <typename T>
void sort_unique(std::vector<T>& values) {
    std::ranges::sort(values);
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::string join_ids(const std::vector<std::string>& values, const std::string_view separator) {
    std::ostringstream output;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) output << separator;
        output << values[index];
    }
    return output.str();
}

std::string asset_fingerprint(const AssetRecord& asset) {
    if (!asset.content_hash.empty()) return asset.content_hash;
    std::ostringstream source;
    source << asset.id << '\n' << asset.uri << '\n' << asset.relative_path << '\n' << asset.import_state;
    return digest(source.str());
}

std::string canonical_asset_key(const AssetRecord& asset) {
    std::vector<std::string> dependencies = asset.dependencies;
    std::vector<std::string> tags = asset.tags;
    sort_unique(dependencies);
    sort_unique(tags);
    std::ostringstream source;
    source << asset.id << '\n' << asset.display_name << '\n' << asset.kind << '\n' << asset.uri << '\n'
        << asset.source_root << '\n' << asset.relative_path << '\n' << asset.extension << '\n'
        << asset.content_hash << '\n' << asset.hash_provenance << '\n' << asset.import_state << '\n'
        << asset.license << '\n' << asset.redistribution << '\n' << asset.source_bytes << '\n'
        << (asset.optional ? 1 : 0) << '\n' << (asset.available ? 1 : 0) << '\n'
        << join_ids(tags, "\x1f") << '\n' << join_ids(dependencies, "\x1f");
    return source.str();
}

std::string input_fingerprint(const AssetRepairInput& input) {
    std::vector<const AssetRecord*> assets;
    assets.reserve(input.assets.size());
    for (const auto& asset : input.assets) assets.push_back(&asset);
    std::ranges::sort(assets, [](const auto* left, const auto* right) {
        if (left->id != right->id) return left->id < right->id;
        return canonical_asset_key(*left) < canonical_asset_key(*right);
    });
    std::vector<const AssetRepairJobObservation*> jobs;
    jobs.reserve(input.jobs.size());
    for (const auto& job : input.jobs) jobs.push_back(&job);
    std::ranges::sort(jobs, [](const auto* left, const auto* right) {
        return std::tie(left->asset_id, left->kind, left->state, left->input_fingerprint,
            left->source_revision, left->code, left->detail) <
            std::tie(right->asset_id, right->kind, right->state, right->input_fingerprint,
                right->source_revision, right->code, right->detail);
    });
    std::ostringstream source;
    source << "noemancer.asset-repair-input/0.1\n" << assets.size() << '\n';
    for (const auto* asset : assets) source << canonical_asset_key(*asset) << '\n';
    source << "jobs=" << jobs.size() << '\n';
    for (const auto* job : jobs) {
        source << job->asset_id << '\n' << job->kind << '\n' << job->state << '\n'
            << job->input_fingerprint << '\n' << job->source_revision << '\n'
            << job->code << '\n' << job->detail << '\n';
    }
    return digest(source.str());
}

std::string action_key(const AssetRepairActionKind kind, const std::string_view asset_id,
    const std::string_view related_id, const std::string_view diagnostic_code,
    const std::string_view fingerprint, const std::uint64_t revision) {
    std::ostringstream source;
    source << static_cast<unsigned int>(kind) << '\n' << asset_id << '\n' << related_id << '\n'
        << diagnostic_code << '\n' << fingerprint << '\n' << revision;
    return digest(source.str());
}

bool diagnostic_less(const AssetRepairDiagnostic& left, const AssetRepairDiagnostic& right) {
    return std::tie(left.code, left.asset_id, left.dependency_id, left.input_fingerprint,
        left.detail, left.related_asset_ids) <
        std::tie(right.code, right.asset_id, right.dependency_id, right.input_fingerprint,
            right.detail, right.related_asset_ids);
}

bool action_less(const AssetRepairAction& left, const AssetRepairAction& right) {
    return std::tie(left.kind, left.asset_id, left.related_asset_id, left.diagnostic_code,
        left.input_fingerprint, left.base_revision, left.action_id) <
        std::tie(right.kind, right.asset_id, right.related_asset_id, right.diagnostic_code,
            right.input_fingerprint, right.base_revision, right.action_id);
}

Json diagnostic_json(const AssetRepairDiagnostic& diagnostic, const std::size_t text_limit) {
    return {
        {"code", diagnostic.code},
        {"assetId", bounded_text(diagnostic.asset_id, text_limit)},
        {"dependencyId", bounded_text(diagnostic.dependency_id, text_limit)},
        {"detail", safe_detail(diagnostic.detail, text_limit)},
        {"inputFingerprint", bounded_text(diagnostic.input_fingerprint, text_limit)},
        {"registryRevision", diagnostic.registry_revision},
        {"blocking", diagnostic.blocking},
        {"relatedAssetIds", diagnostic.related_asset_ids}
    };
}

Json action_json(const AssetRepairAction& action, const std::size_t text_limit) {
    return {
        {"actionId", action.action_id},
        {"kind", asset_repair_action_name(action.kind)},
        {"assetId", bounded_text(action.asset_id, text_limit)},
        {"relatedAssetId", bounded_text(action.related_asset_id, text_limit)},
        {"diagnosticCode", action.diagnostic_code},
        {"inputFingerprint", bounded_text(action.input_fingerprint, text_limit)},
        {"baseRevision", action.base_revision}
    };
}

Json plan_json(const AssetRepairPlan& plan, const std::size_t text_limit) {
    Json actions = Json::array();
    for (const auto& action : plan.actions) actions.push_back(action_json(action, text_limit));
    return {
        {"schemaVersion", "noemancer.asset-repair-plan/0.1"},
        {"valid", plan.valid},
        {"code", plan.code},
        {"detail", safe_detail(plan.detail, text_limit)},
        {"planId", plan.plan_id},
        {"inputFingerprint", plan.input_fingerprint},
        {"registryRevision", plan.registry_revision},
        {"actions", std::move(actions)},
        {"actionsTruncated", plan.actions_truncated}
    };
}

Json report_json(const AssetRepairReport& report, const std::size_t text_limit) {
    Json diagnostics = Json::array();
    for (const auto& diagnostic : report.diagnostics) diagnostics.push_back(diagnostic_json(diagnostic, text_limit));
    return {
        {"schemaVersion", "noemancer.asset-repair-report/0.1"},
        {"valid", report.valid},
        {"code", report.code},
        {"detail", safe_detail(report.detail, text_limit)},
        {"inputFingerprint", report.input_fingerprint},
        {"registryRevision", report.registry_revision},
        {"diagnostics", std::move(diagnostics)},
        {"diagnosticsTruncated", report.diagnostics_truncated},
        {"plan", plan_json(report.plan, text_limit)}
    };
}

Json receipt_json(const AssetRepairReceipt& receipt, const std::size_t text_limit) {
    return {
        {"schemaVersion", "noemancer.asset-repair-receipt/0.1"},
        {"success", receipt.success},
        {"dryRun", receipt.dry_run},
        {"mutated", receipt.mutated},
        {"atomic", receipt.atomic},
        {"code", receipt.code},
        {"detail", safe_detail(receipt.detail, text_limit)},
        {"planId", receipt.plan_id},
        {"inputFingerprint", receipt.input_fingerprint},
        {"baseRevision", receipt.base_revision},
        {"revisionAfter", receipt.revision_after},
        {"appliedActionIds", receipt.applied_action_ids},
        {"diagnostics", receipt.diagnostics}
    };
}

} // namespace

std::string asset_repair_action_name(const AssetRepairActionKind kind) {
    switch (kind) {
    case AssetRepairActionKind::rescan: return "rescan";
    case AssetRepairActionKind::reimport: return "reimport";
    case AssetRepairActionKind::inspect: return "inspect";
    case AssetRepairActionKind::retry_cook: return "retry-cook";
    case AssetRepairActionKind::reveal_dependent: return "reveal-dependent";
    }
    return "inspect";
}

AssetRepairReport diagnose_asset_repairs(const AssetRepairInput& input) {
    AssetRepairReport report;
    report.registry_revision = input.registry_revision;
    report.input_fingerprint = input_fingerprint(input);
    report.plan.registry_revision = input.registry_revision;
    report.plan.input_fingerprint = report.input_fingerprint;

    const auto text_limit = std::clamp(input.max_text_bytes, std::size_t{32U}, kMaximumText);
    std::vector<const AssetRecord*> sorted_assets;
    sorted_assets.reserve(input.assets.size());
    for (const auto& asset : input.assets) sorted_assets.push_back(&asset);
    std::ranges::sort(sorted_assets, [](const auto* left, const auto* right) {
        if (left->id != right->id) return left->id < right->id;
        return canonical_asset_key(*left) < canonical_asset_key(*right);
    });

    std::map<std::string, const AssetRecord*> by_id;
    std::set<std::string> diagnostic_keys;
    auto add_diagnostic = [&](AssetRepairDiagnostic diagnostic) {
        diagnostic.detail = safe_detail(diagnostic.detail, text_limit);
        diagnostic.input_fingerprint = bounded_text(diagnostic.input_fingerprint, text_limit);
        sort_unique(diagnostic.related_asset_ids);
        std::ostringstream key;
        key << diagnostic.code << '\n' << diagnostic.asset_id << '\n' << diagnostic.dependency_id << '\n'
            << diagnostic.input_fingerprint << '\n' << join_ids(diagnostic.related_asset_ids, "\x1f");
        if (diagnostic_keys.insert(key.str()).second) report.diagnostics.push_back(std::move(diagnostic));
    };

    for (const auto* asset : sorted_assets) {
        if (asset->id.empty()) {
            add_diagnostic({
                .code = "asset.invalid-id",
                .asset_id = {},
                .detail = "Asset ID is required for dependency diagnostics.",
                .registry_revision = input.registry_revision
            });
            continue;
        }
        if (!by_id.emplace(asset->id, asset).second) {
            add_diagnostic({
                .code = "asset.duplicate-id",
                .asset_id = asset->id,
                .detail = "Multiple Asset Records use the same stable ID.",
                .input_fingerprint = asset_fingerprint(*asset),
                .registry_revision = input.registry_revision
            });
        }
    }

    std::map<std::string, std::vector<std::string>> graph;
    std::map<std::string, std::vector<std::string>> reverse_graph;
    for (const auto& [id, asset] : by_id) {
        auto& dependencies = graph[id];
        dependencies = asset->dependencies;
        sort_unique(dependencies);
        for (const auto& dependency : dependencies) {
            if (!by_id.contains(dependency)) {
                add_diagnostic({
                    .code = "asset.dependency-missing",
                    .asset_id = id,
                    .dependency_id = dependency,
                    .detail = "The asset declares a dependency that is not present in the Asset Registry.",
                    .input_fingerprint = asset_fingerprint(*asset),
                    .registry_revision = input.registry_revision
                });
            } else {
                reverse_graph[dependency].push_back(id);
            }
        }
        sort_unique(reverse_graph[id]);

        const auto state = lower(asset->import_state);
        const bool unavailable = !asset->available || state == "missing" || state == "missing-optional";
        if (unavailable) {
            add_diagnostic({
                .code = "asset.source-unavailable",
                .asset_id = id,
                .detail = asset->optional ?
                    "The optional asset source is unavailable." :
                    "The asset source is unavailable.",
                .input_fingerprint = asset_fingerprint(*asset),
                .registry_revision = input.registry_revision,
                .blocking = !asset->optional
            });
        }

        const bool failed = state.find("fail") != std::string::npos ||
            state.find("error") != std::string::npos || state.find("invalid") != std::string::npos;
        if (failed) {
            const bool cook = state.find("cook") != std::string::npos;
            add_diagnostic({
                .code = cook ? "asset.cook-failed" : "asset.import-failed",
                .asset_id = id,
                .detail = cook ? "The asset Cook state reports a failure." : "The asset Import state reports a failure.",
                .input_fingerprint = asset_fingerprint(*asset),
                .registry_revision = input.registry_revision
            });
        }
    }

    std::vector<const AssetRepairJobObservation*> sorted_jobs;
    sorted_jobs.reserve(input.jobs.size());
    for (const auto& job : input.jobs) sorted_jobs.push_back(&job);
    std::ranges::sort(sorted_jobs, [](const auto* left, const auto* right) {
        return std::tie(left->asset_id, left->kind, left->state, left->input_fingerprint,
            left->source_revision, left->code, left->detail) <
            std::tie(right->asset_id, right->kind, right->state, right->input_fingerprint,
                right->source_revision, right->code, right->detail);
    });
    for (const auto* job : sorted_jobs) {
        if (lower(job->state) != "failed") continue;
        const auto* asset = by_id.contains(job->asset_id) ? by_id.at(job->asset_id) : nullptr;
        const auto kind = lower(job->kind);
        const bool cook = kind == "cook" || kind.find("cook") != std::string::npos;
        const bool import = kind == "import" || kind.find("import") != std::string::npos;
        if (!cook && !import) continue;
        add_diagnostic({
            .code = cook ? "asset.cook-failed" : "asset.import-failed",
            .asset_id = job->asset_id,
            .detail = job->detail.empty() ?
                (cook ? "The asset Cook Job failed." : "The asset Import Job failed.") : job->detail,
            .input_fingerprint = job->input_fingerprint.empty() && asset != nullptr ?
                asset_fingerprint(*asset) : job->input_fingerprint,
            .registry_revision = input.registry_revision
        });
    }

    // Report one deterministic diagnostic per directed cycle.  The action is
    // intentionally inspect-only: breaking a cycle requires a user decision,
    // so the repair service must not guess which dependency to remove.
    std::map<std::string, std::uint8_t> colors;
    std::vector<std::string> stack;
    std::map<std::string, std::size_t> stack_index;
    std::set<std::string> cycle_keys;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        colors[id] = 1U;
        stack_index[id] = stack.size();
        stack.push_back(id);
        for (const auto& dependency : graph[id]) {
            if (!by_id.contains(dependency)) continue;
            if (colors[dependency] == 0U) {
                visit(dependency);
            } else if (colors[dependency] == 1U) {
                const auto begin = stack_index[dependency];
                std::vector<std::string> cycle(stack.begin() + static_cast<std::ptrdiff_t>(begin), stack.end());
                if (cycle.empty()) continue;
                const auto minimum = std::ranges::min_element(cycle);
                std::rotate(cycle.begin(), minimum, cycle.end());
                const auto key = join_ids(cycle, "\x1f");
                if (cycle_keys.insert(key).second) {
                    std::vector<std::string> path = cycle;
                    path.push_back(cycle.front());
                    const auto* asset = by_id.at(cycle.front());
                    add_diagnostic({
                        .code = "asset.dependency-cycle",
                        .asset_id = cycle.front(),
                        .dependency_id = cycle.size() > 1U ? cycle[1U] : cycle.front(),
                        .detail = "Dependency cycle: " + join_ids(path, " -> ") + ".",
                        .input_fingerprint = asset_fingerprint(*asset),
                        .registry_revision = input.registry_revision,
                        .related_asset_ids = cycle
                    });
                }
            }
        }
        stack.pop_back();
        stack_index.erase(id);
        colors[id] = 2U;
    };
    for (const auto& [id, unused] : graph) {
        static_cast<void>(unused);
        if (colors[id] == 0U) visit(id);
    }

    std::ranges::sort(report.diagnostics, diagnostic_less);
    if (report.diagnostics.size() > std::min(input.max_diagnostics, kMaximumItems)) {
        report.diagnostics.resize(std::min(input.max_diagnostics, kMaximumItems));
        report.diagnostics_truncated = true;
    }

    std::vector<AssetRepairAction> all_actions;
    std::set<std::string> action_keys;
    auto add_action = [&](const AssetRepairActionKind kind, const AssetRepairDiagnostic& diagnostic,
        const std::string_view asset_id, const std::string_view related_id = std::string_view{}) {
        AssetRepairAction action;
        action.kind = kind;
        action.asset_id = std::string(asset_id);
        action.related_asset_id = std::string(related_id);
        action.diagnostic_code = diagnostic.code;
        // Actions carry the complete input fingerprint, not just the source
        // hash of one diagnostic.  This makes every action independently
        // rejectable when the Browser snapshot changes; the diagnostic keeps
        // its narrower source fingerprint for display.
        action.input_fingerprint = report.input_fingerprint;
        action.base_revision = input.registry_revision;
        action.action_id = "asset-repair-action-" + action_key(kind, action.asset_id,
            action.related_asset_id, action.diagnostic_code, action.input_fingerprint, action.base_revision).substr(8U);
        if (action_keys.insert(action.action_id).second) all_actions.push_back(std::move(action));
    };
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.code == "asset.source-unavailable") {
            add_action(AssetRepairActionKind::rescan, diagnostic, diagnostic.asset_id);
        } else if (diagnostic.code == "asset.dependency-missing") {
            add_action(AssetRepairActionKind::rescan, diagnostic, diagnostic.asset_id, diagnostic.dependency_id);
            add_action(AssetRepairActionKind::reveal_dependent, diagnostic, diagnostic.asset_id, diagnostic.dependency_id);
        } else if (diagnostic.code == "asset.dependency-cycle") {
            add_action(AssetRepairActionKind::inspect, diagnostic, diagnostic.asset_id, diagnostic.dependency_id);
        } else if (diagnostic.code == "asset.import-failed") {
            add_action(AssetRepairActionKind::reimport, diagnostic, diagnostic.asset_id);
        } else if (diagnostic.code == "asset.cook-failed") {
            add_action(AssetRepairActionKind::retry_cook, diagnostic, diagnostic.asset_id);
        }
    }
    std::ranges::sort(all_actions, action_less);
    const auto action_limit = std::min(input.max_actions, kMaximumItems);
    if (all_actions.size() > action_limit) {
        all_actions.resize(action_limit);
        report.plan.actions_truncated = true;
    }
    report.plan.actions = std::move(all_actions);
    report.plan.valid = true;
    report.plan.code = report.diagnostics.empty() ? "ok" : "asset.repair-actions-ready";
    report.plan.detail = report.diagnostics.empty() ?
        "No asset repair diagnostics were found." : "Safe asset repair actions are available for host application.";
    std::ostringstream plan_source;
    plan_source << report.input_fingerprint << '\n' << report.registry_revision << '\n';
    for (const auto& action : report.plan.actions) plan_source << action.action_id << '\n';
    report.plan.plan_id = "asset-repair-plan-" + digest(plan_source.str()).substr(8U);
    report.valid = true;
    report.code = report.diagnostics.empty() ? "ok" : "asset.repair-diagnostics-found";
    report.detail = report.diagnostics.empty() ?
        "No asset repair diagnostics were found." : "Asset diagnostics were analyzed without filesystem mutation.";
    return report;
}

AssetRepairReceipt apply_asset_repair_plan(
    const AssetRepairPlan& plan,
    const AssetRepairApplyOptions& options,
    const AssetRepairHost& host) {
    AssetRepairReceipt receipt;
    receipt.dry_run = options.dry_run;
    receipt.plan_id = plan.plan_id;
    receipt.input_fingerprint = plan.input_fingerprint;
    receipt.base_revision = plan.registry_revision;
    const auto text_limit = std::clamp(options.max_text_bytes, std::size_t{32U}, kMaximumText);

    const auto expected_revision = options.expected_revision == 0U ?
        plan.registry_revision : options.expected_revision;
    const auto expected_fingerprint = options.expected_fingerprint.empty() ?
        plan.input_fingerprint : options.expected_fingerprint;
    if (!plan.valid || plan.plan_id.empty() || plan.input_fingerprint.empty()) {
        receipt.code = "asset.repair.plan-invalid";
        receipt.detail = "Only a complete Asset Repair plan can be applied.";
        return receipt;
    }
    if (expected_revision != plan.registry_revision || expected_fingerprint != plan.input_fingerprint) {
        receipt.code = "asset.repair.plan-integrity-error";
        receipt.detail = "The requested revision or input fingerprint does not match the plan.";
        return receipt;
    }

    std::unordered_set<std::string> action_ids;
    for (const auto& action : plan.actions) {
        if (action.action_id.empty() || !action_ids.insert(action.action_id).second ||
            action.base_revision != plan.registry_revision ||
            (!action.input_fingerprint.empty() && action.input_fingerprint != plan.input_fingerprint)) {
            receipt.code = "asset.repair.plan-invalid";
            receipt.detail = "The plan contains an invalid or stale action.";
            return receipt;
        }
    }
    if (plan.actions.empty()) {
        receipt.success = true;
        receipt.atomic = true;
        receipt.code = "asset.repair.no-op";
        receipt.detail = "The plan contains no repair actions.";
        receipt.revision_after = plan.registry_revision;
        return receipt;
    }

    if (host.current_revision) {
        const auto current_revision = host.current_revision();
        if (current_revision != expected_revision) {
            receipt.code = "asset.repair.revision-conflict";
            receipt.detail = "The Asset Registry changed after the repair plan was created.";
            receipt.revision_after = current_revision;
            return receipt;
        }
    } else if (!options.dry_run) {
        receipt.code = "asset.repair.host-revision-unavailable";
        receipt.detail = "A real repair requires a host revision callback.";
        return receipt;
    }
    if (host.current_fingerprint) {
        const auto current_fingerprint = host.current_fingerprint();
        if (!current_fingerprint.empty() && current_fingerprint != expected_fingerprint) {
            receipt.code = "asset.repair.fingerprint-conflict";
            receipt.detail = "The Asset Browser inputs changed after the repair plan was created.";
            return receipt;
        }
    }

    if (!host.execute) {
        if (!options.dry_run) {
            receipt.code = "asset.repair.host-unavailable";
            receipt.detail = "A real repair requires a host mutation callback; no filesystem mutation was attempted.";
            return receipt;
        }
        receipt.success = true;
        receipt.code = "asset.repair.dry-run";
        receipt.detail = "Repair plan validated without invoking a mutation host.";
        receipt.revision_after = expected_revision;
        return receipt;
    }

    AssetRepairHostResult host_result;
    try {
        host_result = host.execute(plan, options.dry_run);
    } catch (...) {
        receipt.code = "asset.repair.host-exception";
        receipt.detail = "The repair host threw; no successful receipt was produced.";
        return receipt;
    }
    receipt.atomic = host_result.atomic;
    receipt.revision_after = host_result.revision_after;
    receipt.diagnostics.reserve(std::min(host_result.diagnostics.size(), std::size_t{64U}));
    for (const auto& diagnostic : host_result.diagnostics) {
        if (receipt.diagnostics.size() >= 64U) break;
        receipt.diagnostics.push_back(safe_detail(diagnostic, text_limit));
    }
    if (!host_result.success) {
        receipt.code = host_result.code.empty() ? "asset.repair.host-failed" : host_result.code;
        receipt.detail = host_result.detail.empty() ? "The host rejected the repair plan." : host_result.detail;
        return receipt;
    }
    if (host_result.mutated && options.dry_run) {
        receipt.code = "asset.repair.dry-run-mutated";
        receipt.detail = "The host reported mutation during a dry-run; the receipt is rejected.";
        return receipt;
    }
    if (!options.dry_run && (!host_result.atomic || !host_result.mutated)) {
        receipt.code = "asset.repair.non-atomic-host";
        receipt.detail = "The host did not prove one atomic mutation for the complete repair plan.";
        return receipt;
    }
    std::ranges::sort(host_result.applied_action_ids);
    if (!options.dry_run && host_result.applied_action_ids != [&] {
            std::vector<std::string> expected;
            expected.reserve(plan.actions.size());
            for (const auto& action : plan.actions) expected.push_back(action.action_id);
            std::ranges::sort(expected);
            return expected;
        }()) {
        receipt.code = "asset.repair.partial-commit";
        receipt.detail = "The host did not acknowledge every planned action; atomic success is not reported.";
        return receipt;
    }
    receipt.success = true;
    receipt.mutated = host_result.mutated;
    receipt.applied_action_ids = std::move(host_result.applied_action_ids);
    receipt.code = options.dry_run ? "asset.repair.dry-run" : "ok";
    receipt.detail = options.dry_run ? "Repair plan validated by the host without mutation." :
        "Asset repair plan committed atomically by the host.";
    return receipt;
}

std::string asset_repair_report_json(const AssetRepairReport& report, const std::size_t max_bytes) {
    const auto text_limit = std::size_t{512U};
    Json document = report_json(report, text_limit);
    if (max_bytes == 0U || document.dump().size() <= max_bytes) return document.dump();
    const auto limit = std::max(max_bytes, kMinimumJsonBytes);
    auto& diagnostics = document.at("diagnostics");
    auto& actions = document.at("plan").at("actions");
    while (document.dump().size() > limit && (!diagnostics.empty() || !actions.empty())) {
        if (!actions.empty()) actions.erase(actions.end() - 1);
        else diagnostics.erase(diagnostics.end() - 1);
        document["diagnosticsTruncated"] = true;
        document["plan"]["actionsTruncated"] = true;
    }
    auto encoded = document.dump();
    if (encoded.size() <= limit) return encoded;
    // The fixed envelope is always valid JSON; a caller can inspect the
    // stable code and request a larger bound if it needs the full detail.
    return Json{
        {"schemaVersion", "noemancer.asset-repair-report/0.1"},
        {"valid", report.valid},
        {"code", report.code},
        {"detail", "Asset repair report exceeded the requested observation bound."},
        {"inputFingerprint", report.input_fingerprint},
        {"registryRevision", report.registry_revision},
        {"diagnostics", Json::array()},
        {"diagnosticsTruncated", true},
        {"plan", {
            {"schemaVersion", "noemancer.asset-repair-plan/0.1"},
            {"valid", report.plan.valid},
            {"code", report.plan.code},
            {"detail", "Asset repair plan exceeded the requested observation bound."},
            {"planId", report.plan.plan_id},
            {"inputFingerprint", report.plan.input_fingerprint},
            {"registryRevision", report.plan.registry_revision},
            {"actions", Json::array()},
            {"actionsTruncated", true}
        }}
    }.dump();
}

std::string asset_repair_receipt_json(const AssetRepairReceipt& receipt, const std::size_t max_bytes) {
    auto document = receipt_json(receipt, std::size_t{512U});
    auto encoded = document.dump();
    if (max_bytes == 0U || encoded.size() <= max_bytes) return encoded;
    const auto limit = std::max(max_bytes, kMinimumJsonBytes);
    while (encoded.size() > limit && !document.at("diagnostics").empty()) {
        document.at("diagnostics").erase(document.at("diagnostics").end() - 1);
        encoded = document.dump();
    }
    if (encoded.size() <= limit) return encoded;
    document["diagnostics"] = Json::array();
    document["detail"] = "Asset repair receipt exceeded the requested observation bound.";
    return document.dump();
}

} // namespace noemancer
