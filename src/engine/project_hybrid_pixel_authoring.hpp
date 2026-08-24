#pragma once

#include "engine/hybrid_pixel_profile.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noemancer {

inline constexpr std::string_view project_hybrid_pixel_authoring_schema =
    "noemancer.project-hybrid-pixel-authoring/0.1";
inline constexpr std::string_view project_hybrid_pixel_manifest_schema =
    "noemancer.project/0.2";

enum class ProjectHybridPixelDiagnosticSeverity : std::uint8_t {
    error,
    warning
};

struct ProjectHybridPixelDiagnostic final {
    ProjectHybridPixelDiagnosticSeverity severity{ProjectHybridPixelDiagnosticSeverity::error};
    std::string code;
    std::string path;
    std::string message;
};

struct ProjectHybridPixelEditOptions final {
    std::optional<std::uint64_t> expected_revision;
    bool dry_run{};
};

struct ProjectHybridPixelEditReceipt final {
    bool success{};
    bool changed{};
    bool persisted{};
    std::string operation;
    std::string code;
    std::string detail;
    std::uint64_t revision{};
    std::optional<HybridPixelProfile> profile;
    bool can_undo{};
    bool can_redo{};
    std::vector<ProjectHybridPixelDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
    [[nodiscard]] std::string to_json() const;
};

struct ProjectHybridPixelManifestLoadResult final {
    bool success{};
    std::string code;
    std::string detail;
    std::filesystem::path manifest_path;
    std::optional<HybridPixelProfile> profile;
    std::vector<ProjectHybridPixelDiagnostic> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

// Engine-owned project authoring authority for the optional Hybrid Pixel
// profile.  The value is plain data; persistence is limited to replacing the
// hybridPixelProfile member of an existing noemancer.project/0.2 manifest.
// Unknown project fields, including inputActions, are carried through as
// opaque JSON values.  A non-empty manifest path enables atomic persistence;
// a path-free instance remains useful for validation and dry-run planning.
class ProjectHybridPixelAuthoring final {
public:
    explicit ProjectHybridPixelAuthoring(
        std::optional<HybridPixelProfile> profile = {}, std::uint64_t revision = 1U);
    ProjectHybridPixelAuthoring(std::optional<HybridPixelProfile> profile,
                                std::filesystem::path manifest_path,
                                std::uint64_t revision = 1U);
    explicit ProjectHybridPixelAuthoring(std::filesystem::path manifest_path,
                                         std::uint64_t revision = 1U);

    [[nodiscard]] static ProjectHybridPixelAuthoring load(
        std::optional<HybridPixelProfile> profile, std::filesystem::path manifest_path,
        std::uint64_t revision = 1U) {
        return ProjectHybridPixelAuthoring(std::move(profile), std::move(manifest_path), revision);
    }
    [[nodiscard]] static ProjectHybridPixelAuthoring load(
        std::filesystem::path manifest_path, std::optional<HybridPixelProfile> profile = {},
        std::uint64_t revision = 1U) {
        return ProjectHybridPixelAuthoring(std::move(profile), std::move(manifest_path), revision);
    }

    // Reads only the project manifest and returns its optional profile.  The
    // caller can pass the result to load()/the constructor after handling the
    // stable diagnostics; unknown fields are never rejected here.
    [[nodiscard]] static ProjectHybridPixelManifestLoadResult load_manifest(
        const std::filesystem::path& manifest_path);

    [[nodiscard]] const std::optional<HybridPixelProfile>& profile() const noexcept {
        return profile_;
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const std::filesystem::path& manifest_path() const noexcept {
        return manifest_path_;
    }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

    [[nodiscard]] std::vector<ProjectHybridPixelDiagnostic> validate() const;

    // replace(optional) is the single state transition contract.  apply is a
    // convenient non-optional spelling; replace(nullopt)/remove erase the
    // optional manifest member, whereas disable preserves it with enabled=false.
    [[nodiscard]] ProjectHybridPixelEditReceipt replace(
        std::optional<HybridPixelProfile> profile,
        ProjectHybridPixelEditOptions options = {});
    [[nodiscard]] ProjectHybridPixelEditReceipt apply(
        HybridPixelProfile profile, ProjectHybridPixelEditOptions options = {});
    [[nodiscard]] ProjectHybridPixelEditReceipt apply(
        std::optional<HybridPixelProfile> profile, ProjectHybridPixelEditOptions options = {});
    [[nodiscard]] ProjectHybridPixelEditReceipt disable(
        ProjectHybridPixelEditOptions options = {});
    [[nodiscard]] ProjectHybridPixelEditReceipt remove(
        ProjectHybridPixelEditOptions options = {});
    [[nodiscard]] ProjectHybridPixelEditReceipt undo(
        ProjectHybridPixelEditOptions options = {});
    [[nodiscard]] ProjectHybridPixelEditReceipt redo(
        ProjectHybridPixelEditOptions options = {});

    [[nodiscard]] std::string observe_json() const;
    [[nodiscard]] std::string serialize_json() const { return observe_json(); }

private:
    struct HistoryEntry final {
        std::optional<HybridPixelProfile> before;
        std::optional<HybridPixelProfile> after;
    };

    enum class HistoryDirection : std::uint8_t {
        replace,
        undo,
        redo
    };

    [[nodiscard]] ProjectHybridPixelEditReceipt commit_candidate(
        std::optional<HybridPixelProfile> candidate,
        const ProjectHybridPixelEditOptions& options,
        std::string_view operation, HistoryDirection direction,
        std::optional<HistoryEntry> history_entry = {});

    [[nodiscard]] ProjectHybridPixelEditReceipt failure(
        std::string_view operation, std::string_view code, std::string_view detail,
        std::vector<ProjectHybridPixelDiagnostic> diagnostics = {}) const;
    [[nodiscard]] ProjectHybridPixelEditReceipt success(
        std::string_view operation, std::string_view code, std::string_view detail,
        bool changed, bool persisted, std::uint64_t revision,
        std::optional<HybridPixelProfile> profile,
        std::vector<ProjectHybridPixelDiagnostic> diagnostics = {}) const;

    std::optional<HybridPixelProfile> profile_;
    std::filesystem::path manifest_path_;
    std::uint64_t revision_{1U};
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
};

// Naming aliases keep the transaction boundary discoverable to callers that
// use the existing ProjectInputEditSession terminology.
using ProjectHybridPixelEditSession = ProjectHybridPixelAuthoring;
using HybridPixelProjectAuthoring = ProjectHybridPixelAuthoring;

} // namespace noemancer
