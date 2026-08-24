#include "engine/project_hybrid_pixel_authoring.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace noemancer {
namespace {

using Json = nlohmann::json;
constexpr std::size_t hybrid_pixel_authoring_history_limit = 256U;

struct ManifestRead final {
    bool success{};
    std::string code;
    std::string detail;
    std::filesystem::path path;
    Json document;
    std::optional<HybridPixelProfile> profile;
    std::vector<ProjectHybridPixelDiagnostic> diagnostics;
};

struct ManifestWrite final {
    bool success{};
    std::string code;
    std::string detail;
    std::vector<ProjectHybridPixelDiagnostic> diagnostics;
};

void add_diagnostic(std::vector<ProjectHybridPixelDiagnostic>& diagnostics,
                    const ProjectHybridPixelDiagnosticSeverity severity,
                    std::string code, std::string path, std::string message) {
    diagnostics.push_back({severity, std::move(code), std::move(path), std::move(message)});
}

const char* severity_name(const ProjectHybridPixelDiagnosticSeverity severity) {
    return severity == ProjectHybridPixelDiagnosticSeverity::warning ? "warning" : "error";
}

Json profile_json(const HybridPixelProfile& profile) {
    return Json::parse(HybridPixelProfileCodec::write_canonical_json(profile), nullptr, false);
}

Json optional_profile_json(const std::optional<HybridPixelProfile>& profile) {
    return profile ? profile_json(*profile) : Json(nullptr);
}

bool profiles_equal(const std::optional<HybridPixelProfile>& left,
                    const std::optional<HybridPixelProfile>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    return HybridPixelProfileCodec::write_canonical_json(*left) ==
        HybridPixelProfileCodec::write_canonical_json(*right);
}

std::vector<ProjectHybridPixelDiagnostic> profile_diagnostics(
    const std::optional<HybridPixelProfile>& profile) {
    std::vector<ProjectHybridPixelDiagnostic> diagnostics;
    if (!profile) return diagnostics;
    for (const auto& error : HybridPixelProfileCodec::validate(*profile)) {
        add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            error.code, error.path, error.message);
    }
    return diagnostics;
}

std::filesystem::path absolute_manifest_path(const std::filesystem::path& manifest_path,
                                             std::error_code& error) {
    if (manifest_path.empty()) {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }
    return std::filesystem::absolute(manifest_path, error).lexically_normal();
}

ManifestRead read_manifest(const std::filesystem::path& manifest_path) {
    ManifestRead result;
    std::error_code error;
    result.path = absolute_manifest_path(manifest_path, error);
    if (error || result.path.empty() || !std::filesystem::is_regular_file(result.path, error) || error) {
        add_diagnostic(result.diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.manifest-not-found", manifest_path.generic_string(),
            "An existing noemancer.project/0.2 manifest is required.");
        result.code = "hybrid-pixel.manifest-not-found";
        result.detail = "The project manifest could not be opened for Hybrid Pixel authoring.";
        return result;
    }

    std::ifstream input(result.path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        add_diagnostic(result.diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.manifest-read-failed", result.path.generic_string(),
            "The project manifest could not be read without an I/O error.");
        result.code = "hybrid-pixel.manifest-read-failed";
        result.detail = "Project manifest read failed.";
        return result;
    }

    result.document = Json::parse(contents.str(), nullptr, false);
    if (result.document.is_discarded() || !result.document.is_object() ||
        !result.document.contains("schema") || !result.document.at("schema").is_string() ||
        result.document.at("schema").get<std::string>() != project_hybrid_pixel_manifest_schema) {
        add_diagnostic(result.diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.manifest-invalid", "/schema",
            "Expected a noemancer.project/0.2 manifest.");
        result.code = "hybrid-pixel.manifest-invalid";
        result.detail = "Project manifest schema is not writable by Hybrid Pixel authoring.";
        return result;
    }

    if (result.document.contains("hybridPixelProfile")) {
        const auto& source = result.document.at("hybridPixelProfile");
        if (!source.is_object()) {
            add_diagnostic(result.diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
                "hybrid-pixel.manifest-profile-invalid", "/hybridPixelProfile",
                "hybridPixelProfile must be an object when present.");
        } else {
            const auto parsed = HybridPixelProfileCodec::parse_json(source.dump());
            for (const auto& profile_error : parsed.errors) {
                const auto path = profile_error.path == "/" ? std::string{"/hybridPixelProfile"} :
                    std::string{"/hybridPixelProfile"} + profile_error.path;
                add_diagnostic(result.diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
                    profile_error.code, path, profile_error.message);
            }
            if (parsed.document) result.profile = *parsed.document;
        }
    }
    if (!result.diagnostics.empty()) {
        result.code = "hybrid-pixel.manifest-invalid";
        result.detail = "The project manifest contains an invalid Hybrid Pixel profile.";
        return result;
    }
    result.success = true;
    result.code = "ok";
    result.detail = "Project manifest is ready for Hybrid Pixel authoring.";
    return result;
}

bool atomic_replace(const std::filesystem::path& temporary,
                   const std::filesystem::path& destination, std::error_code& error) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return true;
#else
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

ManifestWrite write_manifest(const std::filesystem::path& manifest_path,
                             const std::optional<HybridPixelProfile>& profile,
                             const bool dry_run) {
    const auto read = read_manifest(manifest_path);
    if (!read.success) {
        return {false, read.code, read.detail, read.diagnostics};
    }
    auto document = read.document;
    if (profile) document["hybridPixelProfile"] = profile_json(*profile);
    else document.erase("hybridPixelProfile");
    if (dry_run) {
        return {true, "ok", "Project manifest validated; dry-run did not write.", {}};
    }

    static std::atomic_uint64_t write_sequence{1U};
    const auto temporary = read.path.parent_path() / ("." + read.path.filename().string() +
        ".hybrid-pixel-" + std::to_string(write_sequence.fetch_add(1U)) + ".tmp");
    const auto serialized = document.dump(2) + "\n";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::vector<ProjectHybridPixelDiagnostic> diagnostics;
            add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
                "hybrid-pixel.manifest-write-failed", temporary.generic_string(),
                "The sibling temporary project manifest could not be opened.");
            return {false, "hybrid-pixel.manifest-write-failed",
                "Project manifest temporary write failed.", std::move(diagnostics)};
        }
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            std::vector<ProjectHybridPixelDiagnostic> diagnostics;
            add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
                "hybrid-pixel.manifest-write-failed", temporary.generic_string(),
                "The sibling temporary project manifest could not be flushed.");
            return {false, "hybrid-pixel.manifest-write-failed",
                "Project manifest temporary write failed.", std::move(diagnostics)};
        }
    }
    std::error_code replace_error;
    if (!atomic_replace(temporary, read.path, replace_error)) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        std::vector<ProjectHybridPixelDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.manifest-commit-failed", read.path.generic_string(),
            "The project manifest could not be atomically replaced.");
        return {false, "hybrid-pixel.manifest-commit-failed",
            "Project manifest atomic replacement failed (error " +
                std::to_string(replace_error.value()) + ": " + replace_error.message() + ").",
            std::move(diagnostics)};
    }
    return {true, "ok", "Project manifest was atomically replaced.", {}};
}

Json diagnostics_json(const std::vector<ProjectHybridPixelDiagnostic>& diagnostics) {
    Json output = Json::array();
    for (const auto& diagnostic : diagnostics) {
        output.push_back({{"severity", severity_name(diagnostic.severity)},
            {"code", diagnostic.code}, {"path", diagnostic.path}, {"message", diagnostic.message}});
    }
    return output;
}

} // namespace

std::string ProjectHybridPixelEditReceipt::to_json() const {
    return Json{{"schemaVersion", project_hybrid_pixel_authoring_schema},
        {"success", success}, {"changed", changed}, {"persisted", persisted},
        {"operation", operation}, {"code", code}, {"detail", detail},
        {"revision", revision}, {"profile", optional_profile_json(profile)},
        {"canUndo", can_undo}, {"canRedo", can_redo},
        {"diagnostics", diagnostics_json(diagnostics)}}.dump();
}

ProjectHybridPixelAuthoring::ProjectHybridPixelAuthoring(
    std::optional<HybridPixelProfile> profile, const std::uint64_t revision)
    : profile_(std::move(profile)), revision_(revision) {}

ProjectHybridPixelAuthoring::ProjectHybridPixelAuthoring(
    std::optional<HybridPixelProfile> profile, std::filesystem::path manifest_path,
    const std::uint64_t revision)
    : profile_(std::move(profile)), manifest_path_(std::move(manifest_path)), revision_(revision) {}

ProjectHybridPixelAuthoring::ProjectHybridPixelAuthoring(
    std::filesystem::path manifest_path, const std::uint64_t revision)
    : manifest_path_(std::move(manifest_path)), revision_(revision) {}

ProjectHybridPixelManifestLoadResult ProjectHybridPixelAuthoring::load_manifest(
    const std::filesystem::path& manifest_path) {
    const auto read = read_manifest(manifest_path);
    return {read.success, read.code, read.detail, read.path, read.profile, read.diagnostics};
}

std::vector<ProjectHybridPixelDiagnostic> ProjectHybridPixelAuthoring::validate() const {
    return profile_diagnostics(profile_);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::failure(
    const std::string_view operation, const std::string_view code,
    const std::string_view detail,
    std::vector<ProjectHybridPixelDiagnostic> diagnostics) const {
    return {false, false, false, std::string(operation), std::string(code), std::string(detail),
        revision_, profile_, can_undo(), can_redo(), std::move(diagnostics)};
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::success(
    const std::string_view operation, const std::string_view code,
    const std::string_view detail, const bool changed, const bool persisted,
    const std::uint64_t revision, std::optional<HybridPixelProfile> profile,
    std::vector<ProjectHybridPixelDiagnostic> diagnostics) const {
    return {true, changed, persisted, std::string(operation), std::string(code), std::string(detail),
        revision, std::move(profile), can_undo(), can_redo(), std::move(diagnostics)};
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::commit_candidate(
    std::optional<HybridPixelProfile> candidate,
    const ProjectHybridPixelEditOptions& options, const std::string_view operation,
    const HistoryDirection direction, std::optional<HistoryEntry> history_entry) {
    if (options.expected_revision && *options.expected_revision != revision_) {
        std::vector<ProjectHybridPixelDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.revision-conflict", "/revision",
            "Expected revision does not match the current Hybrid Pixel authoring revision.");
        return failure(operation, "hybrid-pixel.revision-conflict",
            "The Hybrid Pixel authoring revision changed; refresh before applying this edit.",
            std::move(diagnostics));
    }

    auto diagnostics = profile_diagnostics(candidate);
    if (!diagnostics.empty()) {
        return failure(operation, "hybrid-pixel.invalid-profile",
            "The Hybrid Pixel profile violates its engine-owned contract.", std::move(diagnostics));
    }
    if (profiles_equal(profile_, candidate)) {
        return success(operation, "hybrid-pixel.no-change",
            "The requested Hybrid Pixel edit produced no change.", false, false,
            revision_, profile_);
    }

    if (options.dry_run) {
        if (!manifest_path_.empty()) {
            const auto manifest = write_manifest(manifest_path_, candidate, true);
            if (!manifest.success) {
                return failure(operation, manifest.code, manifest.detail, manifest.diagnostics);
            }
        }
        return success(operation, "hybrid-pixel.edit.dry-run",
            "The Hybrid Pixel edit was validated; memory and disk were not changed.",
            true, false, revision_, std::move(candidate));
    }
    if (manifest_path_.empty()) {
        std::vector<ProjectHybridPixelDiagnostic> missing;
        add_diagnostic(missing, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.manifest-not-found", "/manifestPath",
            "A manifest path is required before a mutating Hybrid Pixel edit can commit.");
        return failure(operation, "hybrid-pixel.manifest-not-found",
            "An existing noemancer.project/0.2 manifest is required before commit.",
            std::move(missing));
    }
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        std::vector<ProjectHybridPixelDiagnostic> exhausted;
        add_diagnostic(exhausted, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.revision-exhausted", "/revision",
            "The Hybrid Pixel authoring revision cannot advance further.");
        return failure(operation, "hybrid-pixel.revision-exhausted",
            "The Hybrid Pixel authoring revision cannot advance further.", std::move(exhausted));
    }

    const auto manifest = write_manifest(manifest_path_, candidate, false);
    if (!manifest.success) return failure(operation, manifest.code, manifest.detail, manifest.diagnostics);

    const auto before = profile_;
    const HistoryEntry entry = history_entry.value_or(HistoryEntry{before, candidate});
    profile_ = std::move(candidate);
    ++revision_;
    switch (direction) {
    case HistoryDirection::replace:
        undo_.push_back(entry);
        redo_.clear();
        break;
    case HistoryDirection::undo:
        if (!undo_.empty()) undo_.pop_back();
        redo_.push_back(entry);
        break;
    case HistoryDirection::redo:
        if (!redo_.empty()) redo_.pop_back();
        undo_.push_back(entry);
        break;
    }
    const auto trim_history=[](auto& history) {
        if(history.size()>hybrid_pixel_authoring_history_limit)
            history.erase(history.begin(),history.begin()+
                static_cast<typename std::remove_reference_t<decltype(history)>::difference_type>(
                    history.size()-hybrid_pixel_authoring_history_limit));
    };
    trim_history(undo_);
    trim_history(redo_);
    return success(operation, "hybrid-pixel.edit.committed",
        "The Hybrid Pixel profile was atomically persisted and published at the new revision.",
        true, true, revision_, profile_);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::replace(
    std::optional<HybridPixelProfile> profile, const ProjectHybridPixelEditOptions options) {
    return commit_candidate(std::move(profile), options, "replace", HistoryDirection::replace);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::apply(
    HybridPixelProfile profile, const ProjectHybridPixelEditOptions options) {
    return commit_candidate(std::move(profile), options, "apply", HistoryDirection::replace);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::apply(
    std::optional<HybridPixelProfile> profile, const ProjectHybridPixelEditOptions options) {
    return commit_candidate(std::move(profile), options, "apply", HistoryDirection::replace);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::disable(
    const ProjectHybridPixelEditOptions options) {
    if (!profile_) return commit_candidate(std::nullopt, options, "disable", HistoryDirection::replace);
    auto disabled = *profile_;
    disabled.enabled = false;
    return commit_candidate(std::move(disabled), options, "disable", HistoryDirection::replace);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::remove(
    const ProjectHybridPixelEditOptions options) {
    return commit_candidate(std::nullopt, options, "remove", HistoryDirection::replace);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::undo(
    const ProjectHybridPixelEditOptions options) {
    if (options.expected_revision && *options.expected_revision != revision_) {
        std::vector<ProjectHybridPixelDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.revision-conflict", "/revision",
            "Expected revision does not match the current Hybrid Pixel authoring revision.");
        return failure("undo", "hybrid-pixel.revision-conflict",
            "The Hybrid Pixel authoring revision changed; refresh before undo.",
            std::move(diagnostics));
    }
    if (undo_.empty()) {
        std::vector<ProjectHybridPixelDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.undo-empty", "/history",
            "There is no committed Hybrid Pixel edit to undo.");
        return failure("undo", "hybrid-pixel.undo-empty",
            "There is no committed Hybrid Pixel edit to undo.", std::move(diagnostics));
    }
    const auto entry = undo_.back();
    return commit_candidate(entry.before, options, "undo", HistoryDirection::undo, entry);
}

ProjectHybridPixelEditReceipt ProjectHybridPixelAuthoring::redo(
    const ProjectHybridPixelEditOptions options) {
    if (options.expected_revision && *options.expected_revision != revision_) {
        std::vector<ProjectHybridPixelDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.revision-conflict", "/revision",
            "Expected revision does not match the current Hybrid Pixel authoring revision.");
        return failure("redo", "hybrid-pixel.revision-conflict",
            "The Hybrid Pixel authoring revision changed; refresh before redo.",
            std::move(diagnostics));
    }
    if (redo_.empty()) {
        std::vector<ProjectHybridPixelDiagnostic> diagnostics;
        add_diagnostic(diagnostics, ProjectHybridPixelDiagnosticSeverity::error,
            "hybrid-pixel.redo-empty", "/history",
            "There is no undone Hybrid Pixel edit to redo.");
        return failure("redo", "hybrid-pixel.redo-empty",
            "There is no undone Hybrid Pixel edit to redo.", std::move(diagnostics));
    }
    const auto entry = redo_.back();
    return commit_candidate(entry.after, options, "redo", HistoryDirection::redo, entry);
}

std::string ProjectHybridPixelAuthoring::observe_json() const {
    return Json{{"schemaVersion", project_hybrid_pixel_authoring_schema},
        {"revision", revision_}, {"manifestPath", manifest_path_.generic_string()},
        {"profilePresent", profile_.has_value()}, {"enabled", profile_ ? profile_->enabled : false},
        {"profile", optional_profile_json(profile_)}, {"canUndo", can_undo()},
        {"canRedo", can_redo()}, {"diagnostics", diagnostics_json(validate())}}.dump();
}

} // namespace noemancer
