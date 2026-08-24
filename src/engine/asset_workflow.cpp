#include "engine/asset_workflow.hpp"

#include "engine/asset_registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kMaximumDiagnostics = 64U;
constexpr std::size_t kMaximumDiagnosticBytes = 4096U;
constexpr std::size_t kMaximumArtifactBytes = 256U * 1024U * 1024U;

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = kFnvOffset;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::string bounded(std::string_view value, std::size_t limit) {
    if (limit == 0U) return {};
    if (value.size() <= limit) return std::string(value);
    if (limit <= 3U) return std::string(value.substr(0U, limit));
    std::string result(value.substr(0U, limit - 3U));
    result += "...";
    return result;
}

bool looks_like_physical_path(std::string_view value) {
    if (value.empty()) return false;
    if (value.front() == '/' || value.front() == '\\') return true;
    if (value.starts_with("file://")) return true;
    if (value.size() >= 3U && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
        value[1] == ':' && (value[2] == '/' || value[2] == '\\')) return true;
    if (value.find('\\') != std::string_view::npos) return true;
    for (std::size_t index = 1U; index + 2U < value.size(); ++index) {
        if (std::isalpha(static_cast<unsigned char>(value[index - 1U])) != 0 &&
            value[index] == ':' && (value[index + 1U] == '/' || value[index + 1U] == '\\')) {
            return true;
        }
    }
    return false;
}

std::string redact_text(std::string_view value) {
    return looks_like_physical_path(value) ? "<path-redacted>" : std::string(value);
}

bool is_path_field(std::string_view key) {
    return key == "sourceRoot" || key == "sourcePath" || key == "absolutePath" ||
        key == "filesystemPath";
}

Json redact_json(const Json& value, std::string_view key = {}) {
    if (value.is_object()) {
        Json result = Json::object();
        for (const auto& [name, child] : value.items()) {
            result[name] = redact_json(child, name);
        }
        return result;
    }
    if (value.is_array()) {
        Json result = Json::array();
        for (const auto& child : value) result.push_back(redact_json(child));
        return result;
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (is_path_field(key) || looks_like_physical_path(text)) return "<path-redacted>";
        return text;
    }
    return value;
}

std::string redact_and_bound(std::string_view value, std::size_t limit) {
    return bounded(redact_text(value), limit);
}

std::vector<std::filesystem::path> normalized_roots(const AssetWorkflowConfig& config) {
    std::vector<std::filesystem::path> roots;
    roots.reserve(config.asset_roots.size());
    for (const auto& root : config.asset_roots) {
        if (root.empty()) continue;
        roots.push_back(std::filesystem::absolute(root).lexically_normal());
    }
    return roots;
}

std::filesystem::path artifact_root_for(
    const AssetWorkflowConfig& config,
    const std::vector<std::filesystem::path>& roots) {
    if (!config.artifact_root.empty()) {
        return std::filesystem::absolute(config.artifact_root).lexically_normal();
    }
    if (!roots.empty()) return (roots.front().parent_path() / "generated" / "asset-workflow").lexically_normal();
    return (std::filesystem::current_path() / "generated" / "asset-workflow").lexically_normal();
}

std::size_t diagnostic_limit(const AssetWorkflowConfig& config) {
    return std::min(config.max_diagnostics, kMaximumDiagnostics);
}

std::size_t diagnostic_bytes(const AssetWorkflowConfig& config) {
    return std::clamp(config.max_diagnostic_bytes, std::size_t{32U}, kMaximumDiagnosticBytes);
}

AssetJobExecutionResult failure(
    std::string code,
    std::string detail,
    const AssetWorkflowConfig& config,
    std::vector<std::string> diagnostics = {}) {
    AssetJobExecutionResult result;
    result.code = std::move(code);
    result.detail = redact_and_bound(detail, diagnostic_bytes(config));
    const auto limit = diagnostic_limit(config);
    for (auto& diagnostic : diagnostics) {
        if (result.diagnostics.size() >= limit) break;
        result.diagnostics.push_back(redact_and_bound(diagnostic, diagnostic_bytes(config)));
    }
    return result;
}

AssetJobExecutionResult cancelled(const AssetWorkflowConfig& config) {
    return failure("asset.workflow.cancelled", "Asset workflow cancelled before its snapshot was committed.", config);
}

bool report_or_cancel(
    AssetJobContext& context,
    const AssetWorkflowConfig& config,
    float progress,
    std::string_view stage,
    std::string_view diagnostic = {}) {
    if (context.cancellation_requested()) return false;
    if (!context.report(progress, stage, redact_and_bound(diagnostic, diagnostic_bytes(config)))) return false;
    return !context.cancellation_requested();
}

struct SnapshotResult final {
    std::optional<AssetRegistry> registry;
    std::vector<std::filesystem::path> roots;
    std::string error_code;
    std::string error_detail;
};

SnapshotResult make_snapshot(
    const AssetWorkflowConfig& config,
    AssetJobContext& context) {
    SnapshotResult result;
    result.roots = normalized_roots(config);
    if (result.roots.empty()) {
        result.error_code = "asset.workflow.configuration-invalid";
        result.error_detail = "At least one asset root is required.";
        return result;
    }
    if (!report_or_cancel(context, config, 0.05F, "snapshot", "Building an isolated Asset Registry snapshot.")) {
        result.error_code = "asset.workflow.cancelled";
        result.error_detail = "Asset workflow cancelled while building its snapshot.";
        return result;
    }

    // AssetRegistry's constructor performs the first refresh.  Additional
    // roots are added through its own adapter so registry parsing, source
    // discovery and content hashing are not duplicated here.
    result.registry.emplace(result.roots.front());
    for (std::size_t index = 1U; index < result.roots.size(); ++index) {
        if (context.cancellation_requested()) {
            result.error_code = "asset.workflow.cancelled";
            result.error_detail = "Asset workflow cancelled while adding an asset root.";
            result.registry.reset();
            return result;
        }
        static_cast<void>(result.registry->add_root(result.roots[index]));
    }
    return result;
}

bool source_fingerprint_matches(const AssetJobRequest& request, const AssetRecord& asset) {
    return !request.input_fingerprint.empty() &&
        !asset.content_hash.empty() && request.input_fingerprint == asset.content_hash;
}

AssetJobExecutionResult validate_record(
    const AssetJobRequest& request,
    const AssetRegistry& registry,
    const AssetWorkflowConfig& config) {
    const auto* asset = registry.find(request.asset_id);
    if (asset == nullptr) {
        return failure("asset.workflow.asset-not-found", "The requested asset record does not exist.", config);
    }
    if (!request.source_uri.empty() && request.source_uri != asset->uri) {
        return failure("asset.workflow.source-mismatch",
            "The asset source URI no longer matches the requested record.", config);
    }
    if (!source_fingerprint_matches(request, *asset)) {
        return failure("asset.workflow.fingerprint-stale",
            "The asset source fingerprint no longer matches the requested record.", config);
    }
    if (!asset->available) {
        return failure("asset.workflow.source-unavailable",
            "The asset source is unavailable.", config);
    }
    return {};
}

bool write_artifact(
    const AssetWorkflowConfig& config,
    const std::vector<std::filesystem::path>& roots,
    std::string_view kind,
    std::string_view asset_id,
    std::string_view payload,
    std::string& artifact_uri,
    std::string& error) {
    const auto max_bytes = std::min(config.max_artifact_bytes, kMaximumArtifactBytes);
    if (payload.size() > max_bytes) {
        error = "Inspection payload exceeds the configured artifact size limit.";
        return false;
    }
    const auto root = artifact_root_for(config, roots);
    const auto content_key = hex_u64(fnv1a(std::string(kind) + "\n" + std::string(asset_id) + "\n" + std::string(payload)));
    const auto directory = root / std::string(kind);
    const auto destination = directory / (content_key + ".json");
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        error = "Unable to create the asset workflow artifact directory.";
        return false;
    }
    if (!std::filesystem::is_regular_file(destination, filesystem_error)) {
        filesystem_error.clear();
        auto temporary = destination;
        temporary += ".tmp-" + hex_u64(fnv1a(content_key + ":temporary"));
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "Unable to open the asset workflow artifact staging file.";
                return false;
            }
            output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            output.flush();
            if (!output) {
                output.close();
                std::filesystem::remove(temporary, filesystem_error);
                error = "Unable to write the asset workflow artifact.";
                return false;
            }
        }
        std::filesystem::rename(temporary, destination, filesystem_error);
        if (filesystem_error) {
            // Another identical worker may have won the content-addressed
            // race.  Keep its immutable artifact and only fail if it did not.
            std::error_code existing_error;
            const bool existing = std::filesystem::is_regular_file(destination, existing_error);
            std::filesystem::remove(temporary, existing_error);
            if (!existing) {
                error = "Unable to commit the asset workflow artifact.";
                return false;
            }
        }
    }
    artifact_uri = "generated://asset-workflow/" + std::string(kind) + "/" + content_key + ".json";
    return true;
}

AssetJobExecutionResult execute_workflow(
    const AssetWorkflowConfig& config,
    const AssetJobRequest& request,
    AssetJobContext& context) {
    if (request.kind != AssetJobKind::import && request.kind != AssetJobKind::inspect) {
        return failure("asset.workflow.kind-unsupported",
            "The Asset Workflow executor only handles Import and Inspect jobs.", config);
    }
    if (request.asset_id.empty()) {
        return failure("asset.workflow.invalid-request", "An asset ID is required.", config);
    }

    auto snapshot = make_snapshot(config, context);
    if (!snapshot.registry.has_value()) {
        if (snapshot.error_code == "asset.workflow.cancelled") return cancelled(config);
        return failure(snapshot.error_code.empty() ? "asset.workflow.snapshot-failed" : snapshot.error_code,
            snapshot.error_detail.empty() ? "Unable to build an Asset Registry snapshot." : snapshot.error_detail,
            config);
    }
    const auto& registry = *snapshot.registry;
    if (!registry.errors().empty()) {
        return failure("asset.workflow.registry-refresh-failed",
            "The isolated Asset Registry refresh completed with errors.", config, registry.errors());
    }
    if (!report_or_cancel(context, config, 0.24F, "resolve", "Resolving the requested asset record.")) {
        return cancelled(config);
    }
    const auto validation = validate_record(request, registry, config);
    if (!validation.code.empty()) return validation;

    if (request.kind == AssetJobKind::import) {
        if (!report_or_cancel(context, config, 0.72F, "import", "Asset source refreshed and record validated.")) {
            return cancelled(config);
        }
        return AssetJobExecutionResult{
            .success = true,
            .code = "asset.import.ok",
            .detail = "Asset source refreshed and the current record was validated.",
            .artifact_uris = {"asset://" + request.asset_id}
        };
    }

    if (!report_or_cancel(context, config, 0.52F, "inspect", "Running the canonical Asset Registry inspection.")) {
        return cancelled(config);
    }
    Json inspection;
    try {
        inspection = Json::parse(registry.inspect_json(request.asset_id), nullptr, false);
    } catch (...) {
        return failure("asset.workflow.inspect-invalid", "Asset inspection returned invalid JSON.", config);
    }
    if (inspection.is_discarded()) {
        return failure("asset.workflow.inspect-invalid", "Asset inspection returned invalid JSON.", config);
    }
    inspection = redact_json(inspection);
    const auto payload = inspection.dump(2) + "\n";
    if (!report_or_cancel(context, config, 0.82F, "artifact", "Persisting bounded inspection evidence.")) {
        return cancelled(config);
    }
    std::string artifact_uri;
    std::string artifact_error;
    if (!write_artifact(config, snapshot.roots, "inspect", request.asset_id, payload,
        artifact_uri, artifact_error)) {
        return failure("asset.workflow.artifact-write-failed", artifact_error, config);
    }
    if (!report_or_cancel(context, config, 1.0F, "complete", "Inspection artifact committed.")) {
        return cancelled(config);
    }
    return AssetJobExecutionResult{
        .success = inspection.value("valid", false),
        .code = inspection.value("code", inspection.value("valid", false) ? "asset.inspect.ok" : "asset.inspect-invalid"),
        .detail = inspection.value("valid", false)
            ? "Asset inspection completed; evidence is available through the returned artifact URI."
            : "Asset inspection completed with validation errors; evidence is available through the returned artifact URI.",
        .artifact_uris = {std::move(artifact_uri)},
    };
}

} // namespace

AssetJobExecutor make_asset_workflow_executor(AssetWorkflowConfig config) {
    return [config = std::move(config)](const AssetJobRequest& request, AssetJobContext& context) {
        return execute_workflow(config, request, context);
    };
}

AssetWorkflowService::AssetWorkflowService(AssetWorkflowConfig config)
    : config_(std::move(config)) {}

AssetJobExecutor AssetWorkflowService::executor() const {
    return make_asset_workflow_executor(config_);
}

const AssetWorkflowConfig& AssetWorkflowService::config() const noexcept {
    return config_;
}

} // namespace noemancer
