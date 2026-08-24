#pragma once

#include "engine/asset_cook_pipeline.hpp"
#include "engine/project_document.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace noemancer {

[[nodiscard]] std::vector<std::string> collect_scene_asset_ids(const SceneDocument& scene);

// Package input is deliberately plain data.  CPack, MSBuild, file-system
// copying and signing are not part of the engine contract; callers provide
// observations and an atomic commit callback at the integration boundary.
struct PackageFileDescriptor final {
    std::string id;
    std::filesystem::path source_path;
    std::filesystem::path staging_path;
    std::string content_hash;
    std::uintmax_t bytes{};
    std::string license_id;
    bool available{true};
    bool required{true};
};

struct PackageCookArtifact final {
    std::string asset_id;
    std::string display_name;
    std::string kind;
    std::string payload_uri;
    std::string payload_format;
    std::filesystem::path source_path;
    std::filesystem::path staging_path;
    std::string content_hash;
    std::uintmax_t bytes{};
    std::string license_id;
    std::string redistribution;
    std::string streaming_mode{"stream"};
    std::string streaming_importance{"normal"};
    std::uint32_t streaming_priority{500U};
    bool available{true};
    bool required{true};
    std::vector<std::string> dependencies;
    std::vector<std::string> tags;
};

struct PackageCookManifest final {
    std::string schema{"noemancer.cook-manifest/0.1"};
    std::string content_hash;
    std::string target_profile;
    std::vector<PackageCookArtifact> outputs;
};

struct PackageLicenseDescriptor final {
    std::string id;
    std::string name;
    std::string spdx_id;
    std::string notice;
    std::string source_uri;
    bool third_party{true};
    bool redistributable{true};
};

// The descriptor is the input contract.  A ledger entry is the closed,
// package-scoped view emitted after planning: references are sorted and are
// deliberately kept as plain IDs so the generated NOTICE/JSON artifacts can
// be inspected without understanding the planner's internal graph.
struct PackageLicenseLedgerEntry final {
    PackageLicenseDescriptor descriptor;
    std::string identifier_kind; // "spdx" or "custom"
    std::string scope; // "project-owned" or "third-party"
    std::vector<std::string> entry_references;
    std::vector<std::string> required_roots;
};

struct PackageGameProfile final {
    std::string id{"windows-x64-release"};
    std::string display_name{"Noemancer Game"};
    std::string platform{"windows"};
    std::string architecture{"x64"};
    std::string configuration{"release"};
    std::string executable_name{"game.exe"};
};

struct PackageRuntimeInput final {
    struct Requirement final {
        std::string id;
        std::string display_name;
        std::string version;
        std::string architecture;
        bool bundled{};
    };
    PackageFileDescriptor executable;
    std::vector<PackageFileDescriptor> support_files;
    std::vector<Requirement> requirements;
};

struct PackageScriptInput final {
    PackageFileDescriptor assembly;
    std::vector<PackageFileDescriptor> support_files;
};

struct PackageInput final {
    ProjectDocument project;
    SceneDocument startup_scene;
    CookPlatformProfile target_profile;
    PackageCookManifest cook_manifest;
    PackageGameProfile game_profile;
    PackageRuntimeInput runtime;
    PackageScriptInput script;
    PackageFileDescriptor startup_scene_file;
    PackageFileDescriptor hud_document_file;
    // Empty means all required Cook outputs are roots.  Non-empty roots are
    // added to that set and their transitive dependencies are closed.
    std::vector<std::string> startup_asset_ids;
    std::vector<PackageLicenseDescriptor> licenses;
    // Cook-only sources can impose attribution without becoming runtime
    // package entries. These explicit roots keep their notices in the closure.
    std::vector<std::string> required_license_ids;
    std::filesystem::path staging_root;
    bool dry_run{true};
};

struct PackageFileObservation final {
    std::uintmax_t bytes{};
    std::string content_hash;
    bool available{true};
};

// The probe is an optional seam for the host's VFS/build service.  The
// package planner never opens or copies a file by itself.
using PackageFileProbe = std::function<std::optional<PackageFileObservation>(
    const std::filesystem::path& source_path)>;

struct PackageDiagnostic final {
    std::string code;
    std::string path;
    std::string message;
};

struct PackageStageEntry final {
    std::string id;
    std::string role;
    std::filesystem::path source_path;
    std::filesystem::path staging_path;
    std::string content_hash;
    std::uintmax_t bytes{};
    std::string license_id;
    std::vector<std::string> dependencies;
};

struct PackagePlan final {
    bool valid{};
    bool dry_run{true};
    std::string schema{"noemancer.package-plan/0.1"};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string content_hash;
    std::string project_id;
    std::string target_profile;
    std::string game_profile;
    std::filesystem::path staging_root;
    std::vector<std::string> asset_closure;
    std::vector<PackageStageEntry> entries;
    std::vector<PackageLicenseDescriptor> licenses;
    std::vector<PackageLicenseLedgerEntry> license_ledger;
    std::string game_profile_json;
    std::string content_registry_json;
    std::string third_party_license_json;
    std::string notice_text;
    std::vector<PackageDiagnostic> diagnostics;
};

struct PackageCommitRequest final {
    std::string plan_id;
    std::string content_hash;
    std::filesystem::path staging_root;
    std::vector<PackageStageEntry> entries;
};

struct PackageCommitResult final {
    bool success{};
    bool atomic{};
    std::string code;
    std::string detail;
    std::string commit_id;
};

using PackageCommitCallback = std::function<PackageCommitResult(const PackageCommitRequest& request)>;

struct PackageReceipt final {
    bool success{};
    bool dry_run{};
    bool committed{};
    bool atomic{};
    std::string schema{"noemancer.package-receipt/0.1"};
    std::string code;
    std::string detail;
    std::string plan_id;
    std::string content_hash;
    std::string commit_id;
    std::vector<PackageStageEntry> entries;
    std::vector<PackageDiagnostic> diagnostics;
};

[[nodiscard]] PackagePlan plan_package(
    const PackageInput& input, const PackageFileProbe& probe = {});

[[nodiscard]] PackageReceipt commit_package(
    const PackagePlan& plan, const PackageCommitCallback& commit = {});

[[nodiscard]] std::string package_plan_json(const PackagePlan& plan);
[[nodiscard]] std::string package_receipt_json(const PackageReceipt& receipt);

} // namespace noemancer
