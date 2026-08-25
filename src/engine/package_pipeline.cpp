#include "engine/package_pipeline.hpp"
#include "engine/hybrid_pixel_profile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kProjectSchemaLegacy = "noemancer.project/0.1";
constexpr std::string_view kProjectSchemaCurrent = "noemancer.project/0.2";
constexpr std::string_view kSceneSchema = "noemancer.scene/0.1";
constexpr std::string_view kCookManifestSchema = "noemancer.cook-manifest/0.1";

bool valid_identifier_token(const std::string_view value) {
    if (value.empty() || value.size() > 128U) return false;
    const auto first = static_cast<unsigned char>(value.front());
    const auto last = static_cast<unsigned char>(value.back());
    if (!std::isalnum(first) || !std::isalnum(last)) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '.' && character != '-' &&
            character != '_' && character != '+') return false;
    }
    return true;
}

bool valid_license_id(const std::string_view value) {
    // LicenseRef-* is the SPDX spelling for a project-defined license.  The
    // planner also accepts a safe token as custom metadata because existing
    // asset registries use stable IDs such as "project-original".
    if (value.starts_with("LicenseRef-"))
        return valid_identifier_token(value.substr(std::string_view{"LicenseRef-"}.size()));
    return valid_identifier_token(value);
}

bool known_spdx_id(const std::string_view value) {
    // This is intentionally a small, dependency-free set of identifiers used
    // by the engine and its current redistribution closure.  Unknown but
    // syntactically valid tokens remain valid custom identifiers; the ledger
    // makes that distinction explicit instead of pretending to resolve a
    // complete SPDX catalog inside the engine.
    static constexpr std::array<std::string_view, 19> ids{
        "0BSD", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "CC0",
        "CC0-1.0", "CC-BY-4.0", "FTL", "ISC", "MIT", "MPL-2.0", "Unicode-3.0",
        "Unlicense", "Zlib", "GPL-2.0-only", "GPL-3.0-only", "LGPL-2.1-only",
        "LGPL-3.0-only", "OFL-1.1"
    };
    return std::ranges::find(ids, value) != ids.end();
}

std::string license_identifier_kind(const PackageLicenseDescriptor& license) {
    return known_spdx_id(license.spdx_id) ? "spdx" : "custom";
}

bool non_empty_text(const std::string_view value) {
    return std::ranges::any_of(value, [](const char character) {
        return !std::isspace(static_cast<unsigned char>(character));
    });
}

bool valid_source_uri(const std::string_view value) {
    if (value.empty()) return false;
    const auto colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0U) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!std::isalpha(first)) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = value[index];
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) || std::iscntrl(byte)) return false;
        if (index < colon && !std::isalnum(byte) && character != '+' && character != '-' && character != '.')
            return false;
    }
    return colon + 1U < value.size();
}

std::string path_text(const std::filesystem::path& path) {
    return path.generic_string();
}

bool safe_relative_path(const std::filesystem::path& value) {
    if (value.empty() || value.is_absolute() || value.has_root_name() || value.has_root_directory())
        return false;
    const auto normalized = value.lexically_normal();
    if (normalized == "." || normalized.empty()) return false;
    const auto first = normalized.begin();
    return first == normalized.end() || *first != "..";
}

std::string slug(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '.' || character == '_' || character == '-')
            result.push_back(character);
        else
            result.push_back('_');
    }
    if (result.empty()) result = "item";
    return result;
}

std::string payload_extension(std::string_view format) {
    if (format.starts_with("noemancer/meshbin/")) return "meshbin";
    if (format == "noemancer.sprite-atlas-artifact/0.1") return "sprite-atlas.json";
    const auto slash = format.find_last_of("/\\");
    auto leaf = slash == std::string_view::npos ? format : format.substr(slash + 1U);
    while(leaf.starts_with('.'))leaf.remove_prefix(1U);
    return slug(leaf.empty() ? "payload" : leaf);
}

std::uint64_t fnv1a(const std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : value) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

std::string content_hash(const std::string_view value) {
    return "fnv1a64:" + hex_u64(fnv1a(value));
}

void add_diagnostic(PackagePlan& plan, std::string code, std::string path,
                    std::string message) {
    plan.diagnostics.push_back({std::move(code), std::move(path), std::move(message)});
}

void add_diagnostic(PackageReceipt& receipt, std::string code, std::string path,
                    std::string message) {
    receipt.diagnostics.push_back({std::move(code), std::move(path), std::move(message)});
}

bool is_safe_staging_path(const std::filesystem::path& path) {
    return safe_relative_path(path) && path.lexically_normal() == path;
}

std::string first_error_code(const PackagePlan& plan) {
    return plan.diagnostics.empty() ? "package.invalid" : plan.diagnostics.front().code;
}

std::string first_error_detail(const PackagePlan& plan) {
    return plan.diagnostics.empty() ? "Package plan is invalid." : plan.diagnostics.front().message;
}

struct PlanningContext final {
    PackagePlan& plan;
    const PackageInput& input;
    const PackageFileProbe& probe;
    std::set<std::string> entry_ids;
    std::set<std::string> staging_paths;
    std::map<std::string, PackageLicenseDescriptor> licenses;
    std::map<std::string, std::set<std::string>> entry_license_references;
    std::map<std::string, std::set<std::string>> required_license_roots;
};

bool observe_file(PackageFileDescriptor& descriptor, PlanningContext& context,
                 const std::string_view missing_code, const std::string_view label) {
    if (descriptor.source_path.empty()) {
        if (!descriptor.required) return false;
        add_diagnostic(context.plan, std::string(missing_code), descriptor.id,
            std::string(label) + " source path is missing.");
        return false;
    }
    if (!descriptor.available) {
        if (!descriptor.required) return false;
        add_diagnostic(context.plan, std::string(missing_code), path_text(descriptor.source_path),
            std::string(label) + " is not available.");
        return false;
    }
    if (context.probe) {
        try {
            const auto expected_bytes = descriptor.bytes;
            const auto expected_hash = descriptor.content_hash;
            const auto observed = context.probe(descriptor.source_path);
            if (!observed || !observed->available) {
                if (!descriptor.required) return false;
                add_diagnostic(context.plan, std::string(missing_code),
                    path_text(descriptor.source_path), std::string(label) + " was not found by the file probe.");
                return false;
            }
            if ((!expected_hash.empty() && !observed->content_hash.empty() &&
                 expected_hash != observed->content_hash) ||
                (expected_bytes != 0U && observed->bytes != 0U && expected_bytes != observed->bytes)) {
                add_diagnostic(context.plan, "package.file-identity-mismatch",
                    path_text(descriptor.source_path),
                    std::string(label) + " no longer matches its planned bytes or content hash.");
                return false;
            }
            descriptor.available = true;
            descriptor.bytes = observed->bytes;
            if (!observed->content_hash.empty()) descriptor.content_hash = observed->content_hash;
        } catch (const std::exception& error) {
            add_diagnostic(context.plan, "package.file-probe-failed", path_text(descriptor.source_path),
                std::string(label) + " probe failed: " + error.what());
            return false;
        }
    }
    if (descriptor.content_hash.empty()) {
        add_diagnostic(context.plan, "package.content-hash-missing", descriptor.id,
            std::string(label) + " requires a content hash before packaging.");
        return false;
    }
    return true;
}

bool add_entry(PlanningContext& context, PackageFileDescriptor descriptor,
               std::string role, std::filesystem::path default_staging_path,
               std::vector<std::string> dependencies = {}) {
    if (descriptor.id.empty()) {
        add_diagnostic(context.plan, "package.entry-id-missing", path_text(default_staging_path),
            "Package entry ID is required.");
        return false;
    }
    if (descriptor.staging_path.empty()) descriptor.staging_path = std::move(default_staging_path);
    descriptor.staging_path = descriptor.staging_path.lexically_normal();
    if (!is_safe_staging_path(descriptor.staging_path)) {
        add_diagnostic(context.plan, "package.path-escape", path_text(descriptor.staging_path),
            "Staging paths must be relative, normalized and remain inside the package root.");
        return false;
    }
    if (!observe_file(descriptor, context, "package.file-missing", role)) return false;
    const auto staging_key = path_text(descriptor.staging_path);
    if (!context.entry_ids.insert(descriptor.id).second) {
        add_diagnostic(context.plan, "package.entry-duplicate", descriptor.id,
            "Package entry IDs must be unique.");
        return false;
    }
    if (!context.staging_paths.insert(staging_key).second) {
        add_diagnostic(context.plan, "package.path-collision", staging_key,
            "Two package entries resolve to the same staging path.");
        return false;
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    context.plan.entries.push_back({std::move(descriptor.id), std::move(role),
        std::move(descriptor.source_path), std::move(descriptor.staging_path),
        std::move(descriptor.content_hash), descriptor.bytes, std::move(descriptor.license_id),
        std::move(dependencies)});
    return true;
}

std::string default_artifact_source(const PackageCookArtifact& artifact) {
    if (!artifact.source_path.empty()) return path_text(artifact.source_path);
    return artifact.payload_uri;
}

std::filesystem::path default_artifact_stage(const PackageCookArtifact& artifact) {
    return std::filesystem::path("content") / "assets" /
        (slug(artifact.asset_id) + "." + payload_extension(artifact.payload_format));
}

bool path_is_inside(const std::filesystem::path& root, const std::filesystem::path& value) {
    if (root.empty() || value.empty()) return false;
    const auto root_normalized = root.lexically_normal();
    const auto value_normalized = value.lexically_normal();
    auto root_it = root_normalized.begin();
    auto value_it = value_normalized.begin();
    for (; root_it != root_normalized.end() && value_it != value_normalized.end(); ++root_it, ++value_it) {
        if (*root_it != *value_it) return false;
    }
    return root_it == root_normalized.end();
}

void validate_project(const PackageInput& input, PackagePlan& plan) {
    const auto& project = input.project;
    if (project.schema != kProjectSchemaLegacy && project.schema != kProjectSchemaCurrent)
        add_diagnostic(plan, "package.project-schema-invalid", "/project/schema",
            "Package input requires noemancer.project/0.1 or noemancer.project/0.2.");
    if (project.project_id.empty())
        add_diagnostic(plan, "package.project-id-missing", "/project/projectId", "Project ID is required.");
    if (project.name.empty())
        add_diagnostic(plan, "package.project-name-missing", "/project/name", "Project name is required.");
    if (project.root.empty())
        add_diagnostic(plan, "package.project-root-missing", "/project/root", "Project root is required.");
    if (project.startup_scene.empty() || !safe_relative_path(project.startup_scene))
        add_diagnostic(plan, "package.startup-scene-invalid", "/project/startupScene",
            "Startup scene must be a safe project-relative path.");
    const auto& scene = input.startup_scene;
    if (scene.schema != kSceneSchema || scene.scene_guid.empty())
        add_diagnostic(plan, "package.startup-scene-invalid", "/startupScene",
            "A parsed startup scene with a stable scene GUID is required.");
    if (!project.root.empty() && !project.startup_scene.empty()) {
        const auto scene_path = (project.root / project.startup_scene).lexically_normal();
        if (!path_is_inside(project.root, scene_path))
            add_diagnostic(plan, "package.path-escape", "/project/startupScene",
                "Startup scene resolves outside the project root.");
    }
    if(project.hud_document) {
        if(!safe_relative_path(*project.hud_document))add_diagnostic(plan,"package.hud-document-invalid","/project/hudDocument",
            "HUD document must be a safe project-relative path.");
        else if(input.hud_document_file.source_path.empty())add_diagnostic(plan,"package.hud-document-missing","/hudDocument",
            "A project declaring hudDocument requires its validated source file.");
        else if(!path_is_inside(project.root,input.hud_document_file.source_path))add_diagnostic(plan,"package.path-escape","/hudDocument/sourcePath",
            "HUD document source must stay inside the project root.");
    }
    if (project.hybrid_pixel_profile) {
        if (project.schema != kProjectSchemaCurrent) {
            add_diagnostic(plan, "package.hybrid-pixel-profile-schema",
                "/project/hybridPixelProfile",
                "Hybrid Pixel packaging requires noemancer.project/0.2.");
        }
        for (const auto& error : HybridPixelProfileCodec::validate(*project.hybrid_pixel_profile)) {
            const auto path = error.path == "/"
                ? std::string{"/project/hybridPixelProfile"}
                : std::string{"/project/hybridPixelProfile"} + error.path;
            add_diagnostic(plan, "package.hybrid-pixel-profile-invalid", path, error.message);
        }
    }
    if(project.sky_atmosphere) {
        if(project.schema!=kProjectSchemaCurrent)add_diagnostic(plan,"package.sky-atmosphere-schema",
            "/project/skyAtmosphere","Sky Atmosphere packaging requires noemancer.project/0.2.");
        for(const auto& error:SkyAtmosphereSettingsCodec::validate(*project.sky_atmosphere)) {
            const auto path=error.path=="/"?std::string{"/project/skyAtmosphere"}:
                std::string{"/project/skyAtmosphere"}+error.path;
            add_diagnostic(plan,"package.sky-atmosphere-invalid",path,error.message);
        }
    }
    if(project.sky_environment) {
        if(project.schema!=kProjectSchemaCurrent)add_diagnostic(plan,"package.sky-environment-schema",
            "/project/skyEnvironment","Sky Environment packaging requires noemancer.project/0.2.");
        for(const auto& error:SkyEnvironmentCodec::validate(*project.sky_environment)) {
            const auto path=error.path=="/"?std::string{"/project/skyEnvironment"}:
                std::string{"/project/skyEnvironment"}+error.path;
            add_diagnostic(plan,"package.sky-environment-invalid",path,error.message);
        }
    }
}

void validate_game_profile(const PackageInput& input, PackagePlan& plan) {
    const auto& profile = input.game_profile;
    if (profile.id.empty()) add_diagnostic(plan, "package.game-profile-invalid", "/gameProfile/id",
        "Game Profile ID is required.");
    if (profile.display_name.empty()) add_diagnostic(plan, "package.game-profile-invalid", "/gameProfile/displayName",
        "Game Profile display name is required.");
    if (profile.platform != "windows" || profile.architecture != "x64")
        add_diagnostic(plan, "package.game-profile-invalid", "/gameProfile/platform",
            "The P1 package lane currently accepts only windows/x64.");
    if (profile.configuration.empty()) add_diagnostic(plan, "package.game-profile-invalid", "/gameProfile/configuration",
        "Game Profile configuration is required.");
    if (profile.executable_name.empty() || profile.executable_name.find_first_of("/\\") != std::string::npos)
        add_diagnostic(plan, "package.game-profile-invalid", "/gameProfile/executableName",
            "Executable name must be a single file name.");
}

void validate_cook_manifest(const PackageInput& input, PackagePlan& plan) {
    const auto& manifest = input.cook_manifest;
    if (manifest.schema != kCookManifestSchema)
        add_diagnostic(plan, "package.cook-manifest-schema-invalid", "/cookManifest/schema",
            "Package input requires noemancer.cook-manifest/0.1.");
    if (manifest.content_hash.empty())
        add_diagnostic(plan, "package.cook-manifest-hash-missing", "/cookManifest/contentHash",
            "Cook manifest content hash is required for package identity.");
    if (manifest.target_profile.empty() || manifest.target_profile != input.target_profile.id)
        add_diagnostic(plan, "package.target-profile-mismatch", "/cookManifest/targetProfile",
            "Cook manifest target profile must match the selected package target.");
    std::set<std::string> ids;
    for (std::size_t index = 0; index < manifest.outputs.size(); ++index) {
        const auto& artifact = manifest.outputs[index];
        const auto path = "/cookManifest/outputs/" + std::to_string(index);
        if (artifact.asset_id.empty()) add_diagnostic(plan, "package.artifact-id-missing", path,
            "Cook artifact asset ID is required.");
        else if (!ids.insert(artifact.asset_id).second) add_diagnostic(plan, "package.artifact-duplicate", path,
            "Cook artifact asset IDs must be unique.");
        if (artifact.payload_format.empty()) add_diagnostic(plan, "package.artifact-format-missing", path,
            "Cook artifact payload format is required.");
        if (artifact.content_hash.empty()) add_diagnostic(plan, "package.artifact-hash-missing", path,
            "Cook artifact payload hash is required before package planning.");
        const auto source_extension = artifact.source_path.extension().generic_string();
        if (artifact.payload_format == "meshopt/meshbin" ||
            artifact.payload_format == "gltf/binary" || artifact.payload_format == "fbx/binary" ||
            artifact.payload_format == ".glb" || artifact.payload_format == ".gltf" ||
            artifact.payload_format == ".fbx" ||
            source_extension == ".glb" || source_extension == ".gltf" || source_extension == ".fbx") {
            add_diagnostic(plan, "package.source-geometry-forbidden", path,
                "Game packages require noemancer/meshbin/0.2 and may not distribute source GLB/FBX geometry.");
        }
        for (const auto& dependency : artifact.dependencies) {
            if (dependency.empty()) add_diagnostic(plan, "package.artifact-dependency-invalid", path,
                "Cook artifact dependencies must have non-empty asset IDs.");
        }
    }
}

void validate_target_profile(const PackageInput& input, PackagePlan& plan) {
    std::string code;
    std::string detail;
    if (!validate_cook_platform_profile(input.target_profile, code, detail))
        add_diagnostic(plan, "package.target-profile-invalid", "/targetProfile", detail);
}

std::string profile_json(const PackageInput& input) {
    const auto& profile = input.game_profile;
    const auto& project = input.project;
    const auto managed_assembly=input.script.assembly.source_path.empty()?std::string{}:
        (std::filesystem::path("managed")/input.script.assembly.source_path.filename()).generic_string();
    Json input_actions=Json::array();
    for(const auto& action:project.input_actions) {
        Json bindings=Json::array();
        for(const auto& binding:action.bindings)bindings.push_back({{"source",binding.source},
            {"scale",binding.scale},{"deadZone",binding.dead_zone}});
        input_actions.push_back({{"id",action.id},{"kind",action.kind==InputActionKind::button?"button":"axis1d"},
            {"bindings",std::move(bindings)}});
    }
    Json runtime_requirements=Json::array();
    for(const auto& requirement:input.runtime.requirements)runtime_requirements.push_back({
        {"id",requirement.id},{"displayName",requirement.display_name},{"version",requirement.version},
        {"architecture",requirement.architecture},{"bundled",requirement.bundled}});
    Json output = {
        {"schema", "noemancer.game-profile/0.4"},
        {"id", profile.id},
        {"displayName", profile.display_name},
        {"platform", profile.platform},
        {"architecture", profile.architecture},
        {"configuration", profile.configuration},
        {"executable", profile.executable_name},
        {"projectId", project.project_id},
        {"targetProfile", input.target_profile.id},
        {"startupScene", path_text(project.startup_scene)},
        {"startupSceneGuid", input.startup_scene.scene_guid},
        {"managedAssembly",managed_assembly},
        {"managedConfiguration",profile.configuration=="debug"?"Debug":"Release"},
        {"hudDocument",project.hud_document?path_text(*project.hud_document):std::string{}},
        {"assetRegistry","content/assets/registry.json"},
        {"runtimeRequirements",std::move(runtime_requirements)},
        {"packagedAssets",project.packaged_assets},
        {"inputActions",std::move(input_actions)}
    };
    if (project.hybrid_pixel_profile) {
        // The package owns a JSON object, not an escaped JSON string.  Parse
        // the engine codec's canonical text once so the persisted Player
        // contract cannot drift from the authoring contract.
        output["hybridPixelProfile"] = Json::parse(
            HybridPixelProfileCodec::write_canonical_json(*project.hybrid_pixel_profile));
    }
    if(project.sky_atmosphere)output["skyAtmosphere"]=Json::parse(
        SkyAtmosphereSettingsCodec::write_canonical_json(*project.sky_atmosphere));
    if(project.sky_environment)output["skyEnvironment"]=Json::parse(
        SkyEnvironmentCodec::write_canonical_json(*project.sky_environment));
    return output.dump();
}

std::string content_registry_json(const PlanningContext& context) {
    std::vector<Json> ordered_assets;
    for(const auto& entry:context.plan.entries) {
        if(entry.role!="cook-artifact")continue;
        const auto found=std::ranges::find(context.input.cook_manifest.outputs,entry.id,&PackageCookArtifact::asset_id);
        if(found==context.input.cook_manifest.outputs.end())continue;
        const auto relative=entry.staging_path.lexically_relative(std::filesystem::path("content")/"assets");
        ordered_assets.push_back({
            {"id",found->asset_id},
            {"displayName",found->display_name.empty()?found->asset_id:found->display_name},
            {"kind",found->kind.empty()?"Cooked":found->kind},
            {"uri","asset://"+path_text(relative)},
            {"path",path_text(relative)},
            {"contentHash",entry.content_hash},
            {"license",found->license_id},
            {"redistribution",found->redistribution.empty()?"unknown":found->redistribution},
            {"streamingPolicy",{{"mode",found->streaming_mode},{"importance",found->streaming_importance},
                {"priority",found->streaming_priority}}},
            {"tags",found->tags},
            {"dependencies",entry.dependencies}
        });
    }
    std::ranges::sort(ordered_assets,{},[](const Json& asset){return asset.at("id").get<std::string>();});
    Json assets=Json::array();
    for(auto& asset:ordered_assets)assets.push_back(std::move(asset));
    return Json{{"schema","noemancer.assets/0.1"},{"assets",std::move(assets)}}.dump();
}

std::string licenses_json(const std::vector<PackageLicenseLedgerEntry>& licenses) {
    Json output = Json::array();
    for (const auto& license : licenses) {
        const auto& descriptor = license.descriptor;
        output.push_back({
            {"id", descriptor.id},
            {"name", descriptor.name},
            {"spdxId", descriptor.spdx_id},
            {"identifierKind", license.identifier_kind},
            {"scope", license.scope},
            {"sourceUri", descriptor.source_uri},
            {"thirdParty", descriptor.third_party},
            {"redistributable", descriptor.redistributable},
            {"notice", descriptor.notice},
            {"entryReferences", license.entry_references},
            {"requiredRoots", license.required_roots}
        });
    }
    return Json{{"schema", "noemancer.third-party-licenses/0.1"}, {"licenses", std::move(output)}}.dump();
}

std::string notices_text(const std::vector<PackageLicenseLedgerEntry>& licenses) {
    std::string output = "Noemancer third-party notices\n\n";
    for (const auto& license : licenses) {
        const auto& descriptor = license.descriptor;
        output += "== " + descriptor.id + " | " + descriptor.name;
        if (!descriptor.spdx_id.empty()) output += " | " + descriptor.spdx_id;
        output += " ==\n";
        output += descriptor.notice;
        if (output.empty() || output.back() != '\n') output.push_back('\n');
        output.push_back('\n');
    }
    return output;
}

void collect_licenses(PlanningContext& context) {
    for (std::size_t index = 0; index < context.input.licenses.size(); ++index) {
        const auto& license = context.input.licenses[index];
        const auto path = "/licenses/" + std::to_string(index);
        if (license.id.empty()) {
            add_diagnostic(context.plan, "package.license-id-missing", path,
                "License IDs are required.");
            continue;
        }
        if (license.name.empty()) {
            add_diagnostic(context.plan, "package.license-name-missing", path + "/name",
                "A package license name is required for a human-readable ledger.");
        }
        if (license.spdx_id.empty()) {
            add_diagnostic(context.plan, "package.license-spdx-missing", path + "/spdxId",
                "A SPDX identifier or safe custom LicenseRef identifier is required.");
        } else if (!valid_license_id(license.spdx_id)) {
            add_diagnostic(context.plan, "package.license-spdx-invalid", path + "/spdxId",
                "License identifiers must be SPDX-like tokens or LicenseRef-* custom identifiers.");
        }
        if (!license.source_uri.empty() && !valid_source_uri(license.source_uri)) {
            add_diagnostic(context.plan, "package.license-source-uri-invalid", path + "/sourceUri",
                "License sourceUri must be a non-empty URI with a valid scheme and no whitespace.");
        }
        if (license.third_party && license.source_uri.empty()) {
            add_diagnostic(context.plan, "package.license-source-uri-missing", path + "/sourceUri",
                "Every third-party license requires a sourceUri for provenance review.");
        }
        if (license.third_party && !non_empty_text(license.notice)) {
            add_diagnostic(context.plan, "package.notice-missing", path + "/notice",
                "Every third-party license requires a non-empty NOTICE text.");
        }
        const auto [found, inserted] = context.licenses.emplace(license.id, license);
        if (!inserted) {
            add_diagnostic(context.plan, "package.license-duplicate", path,
                "License IDs must be unique; duplicate roots would make the package ledger ambiguous.");
        }
    }
}

void validate_entry_licenses(PlanningContext& context) {
    std::set<std::string> referenced;
    for (const auto& entry : context.plan.entries) {
        if (entry.license_id == "generated") continue;
        if (entry.license_id.empty()) {
            add_diagnostic(context.plan, "package.license-missing", path_text(entry.staging_path),
                "Every redistributed source or cooked artifact requires a license ID.");
            continue;
        }
        referenced.insert(entry.license_id);
        const auto license = context.licenses.find(entry.license_id);
        if (license == context.licenses.end()) {
            add_diagnostic(context.plan, "package.license-not-found", entry.license_id,
                "The entry references a license that is absent from the package license manifest.");
            continue;
        }
        if (!license->second.redistributable)
            add_diagnostic(context.plan, "package.license-not-redistributable", entry.license_id,
                "The referenced license does not allow redistribution in a game package.");
        context.entry_license_references[entry.license_id].insert(entry.id);
        if (license->second.third_party && !non_empty_text(license->second.notice))
            add_diagnostic(context.plan, "package.notice-missing", entry.license_id,
                "Every third-party license included in a package requires a NOTICE text.");
    }
    std::set<std::string> required_roots_seen;
    for (std::size_t index = 0; index < context.input.required_license_ids.size(); ++index) {
        const auto& id = context.input.required_license_ids[index];
        if (id.empty()) {
            add_diagnostic(context.plan, "package.license-missing", "/requiredLicenseIds/" + std::to_string(index),
                "A Cook provenance license root cannot be empty.");
            continue;
        }
        if (!required_roots_seen.insert(id).second) {
            add_diagnostic(context.plan, "package.license-required-root-duplicate",
                "/requiredLicenseIds/" + std::to_string(index),
                "Required license roots must be unique so provenance is not counted twice.");
        }
        referenced.insert(id);
        const auto license = context.licenses.find(id);
        if (license == context.licenses.end()) {
            add_diagnostic(context.plan, "package.license-not-found", id,
                "A Cook provenance license is absent from the package license manifest.");
            continue;
        }
        if (!license->second.redistributable)
            add_diagnostic(context.plan, "package.license-not-redistributable", id,
                "A Cook provenance license does not allow redistribution.");
        context.required_license_roots[id].insert(id);
        if (license->second.third_party && !non_empty_text(license->second.notice))
            add_diagnostic(context.plan, "package.notice-missing", id,
                "Every third-party Cook provenance license requires a NOTICE text.");
    }
    context.plan.licenses.clear();
    context.plan.license_ledger.clear();
    for (const auto& id : referenced) {
        const auto license = context.licenses.find(id);
        if (license == context.licenses.end()) continue;
        context.plan.licenses.push_back(license->second);
        PackageLicenseLedgerEntry ledger;
        ledger.descriptor = license->second;
        ledger.identifier_kind = license_identifier_kind(license->second);
        ledger.scope = license->second.third_party ? "third-party" : "project-owned";
        const auto entry_references = context.entry_license_references.find(id);
        if (entry_references != context.entry_license_references.end())
            ledger.entry_references.assign(entry_references->second.begin(), entry_references->second.end());
        const auto required_roots = context.required_license_roots.find(id);
        if (required_roots != context.required_license_roots.end())
            ledger.required_roots.assign(required_roots->second.begin(), required_roots->second.end());
        context.plan.license_ledger.push_back(std::move(ledger));
    }
}

void add_generated_entry(PlanningContext& context, std::string id, std::string role,
                         std::filesystem::path staging_path, const std::string& text) {
    PackageFileDescriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.source_path = std::filesystem::path("generated://package");
    descriptor.staging_path = std::move(staging_path);
    descriptor.content_hash = content_hash(text);
    descriptor.bytes = text.size();
    descriptor.license_id = "generated";
    add_entry(context, std::move(descriptor), std::move(role), {}, {});
}

void add_startup_scene(PlanningContext& context) {
    const auto& project = context.input.project;
    auto descriptor = context.input.startup_scene_file;
    if (descriptor.id.empty()) descriptor.id = "startup.scene";
    if (descriptor.source_path.empty()) descriptor.source_path = project.root / project.startup_scene;
    if (descriptor.staging_path.empty()) descriptor.staging_path = std::filesystem::path("content") / project.startup_scene;
    add_entry(context, std::move(descriptor), "startup-scene", {}, {});
}

void add_project_hud(PlanningContext& context) {
    if(!context.input.project.hud_document)return;
    auto descriptor=context.input.hud_document_file;
    if(descriptor.id.empty())descriptor.id="project.hud";
    if(descriptor.source_path.empty())descriptor.source_path=context.input.project.root/ *context.input.project.hud_document;
    if(descriptor.staging_path.empty())descriptor.staging_path=std::filesystem::path("content")/ *context.input.project.hud_document;
    add_entry(context,std::move(descriptor),"project-ui",{},{});
}

void add_runtime(PlanningContext& context) {
    auto executable = context.input.runtime.executable;
    if (executable.id.empty()) executable.id = "runtime.executable";
    if (executable.source_path.empty()) {
        add_diagnostic(context.plan, "package.runtime-executable-missing", "/runtime/executable",
            "Runtime executable source path is required.");
    } else {
        if (executable.staging_path.empty())
            executable.staging_path = std::filesystem::path("bin") / context.input.game_profile.executable_name;
        add_entry(context, std::move(executable), "runtime-executable", {}, {});
    }
    for (std::size_t index = 0; index < context.input.runtime.support_files.size(); ++index) {
        auto support = context.input.runtime.support_files[index];
        if (support.id.empty()) support.id = "runtime.support." + std::to_string(index);
        const auto filename = support.source_path.filename();
        if (support.staging_path.empty()) support.staging_path = std::filesystem::path("bin") / filename;
        add_entry(context, std::move(support), "runtime-support", {}, {});
    }
}

void add_script(PlanningContext& context) {
    if (!context.input.project.script_project) return;
    auto assembly = context.input.script.assembly;
    if (assembly.id.empty()) assembly.id = "project.script";
    if (assembly.source_path.empty()) {
        add_diagnostic(context.plan, "package.script-assembly-missing", "/script/assembly",
            "A project with scriptProject requires a built script assembly.");
    } else {
        if (assembly.staging_path.empty()) {
            auto filename = assembly.source_path.filename();
            if (filename.empty()) filename = "Game.dll";
            assembly.staging_path = std::filesystem::path("managed") / filename;
        }
        add_entry(context, std::move(assembly), "script-assembly", {}, {});
    }
    for (std::size_t index = 0; index < context.input.script.support_files.size(); ++index) {
        auto support = context.input.script.support_files[index];
        if (support.id.empty()) support.id = "script.support." + std::to_string(index);
        if (support.staging_path.empty()) support.staging_path = std::filesystem::path("managed") / support.source_path.filename();
        add_entry(context, std::move(support), "script-support", {}, {});
    }
}

void add_artifact(PlanningContext& context, const PackageCookArtifact& artifact) {
    PackageFileDescriptor descriptor;
    descriptor.id = artifact.asset_id;
    descriptor.source_path = artifact.source_path.empty()
        ? std::filesystem::path(default_artifact_source(artifact)) : artifact.source_path;
    descriptor.staging_path = artifact.staging_path.empty() ? default_artifact_stage(artifact) : artifact.staging_path;
    descriptor.content_hash = artifact.content_hash;
    descriptor.bytes = artifact.bytes;
    descriptor.license_id = artifact.license_id;
    descriptor.available = artifact.available;
    descriptor.required = true;
    add_entry(context, std::move(descriptor), "cook-artifact", {}, artifact.dependencies);
}

void add_game_generated_files(PlanningContext& context) {
    context.plan.game_profile_json = profile_json(context.input);
    context.plan.content_registry_json = content_registry_json(context);
    context.plan.third_party_license_json = licenses_json(context.plan.license_ledger);
    context.plan.notice_text = notices_text(context.plan.license_ledger);
    add_generated_entry(context, "game.profile", "game-profile",
        std::filesystem::path("config") / "game-profile.json", context.plan.game_profile_json);
    add_generated_entry(context,"content.registry","asset-registry",
        std::filesystem::path("content")/"assets"/"registry.json",context.plan.content_registry_json);
    add_generated_entry(context, "licenses.manifest", "license-manifest",
        std::filesystem::path("licenses") / "THIRD_PARTY.json", context.plan.third_party_license_json);
    add_generated_entry(context, "licenses.notice", "notice-manifest",
        std::filesystem::path("licenses") / "NOTICE.txt", context.plan.notice_text);
}

void close_artifacts(PlanningContext& context) {
    const auto& outputs = context.input.cook_manifest.outputs;
    std::map<std::string, const PackageCookArtifact*> by_id;
    for (const auto& artifact : outputs) {
        if (!artifact.asset_id.empty()) by_id.emplace(artifact.asset_id, &artifact);
    }
    std::set<std::string> roots;
    for (const auto& artifact : outputs) if (artifact.required) roots.insert(artifact.asset_id);
    for (const auto& root : context.input.startup_asset_ids) {
        if (by_id.find(root) == by_id.end()) {
            add_diagnostic(context.plan, "package.startup-asset-missing", root,
                "Startup asset root is absent from the Cook manifest.");
        } else {
            roots.insert(root);
        }
    }
    std::map<std::string, std::uint8_t> state;
    std::set<std::string> closure;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        const auto found = by_id.find(id);
        if (found == by_id.end()) return;
        if (state[id] == 1U) {
            add_diagnostic(context.plan, "package.artifact-cycle", id,
                "Cook artifact dependencies must form an acyclic closure.");
            return;
        }
        if (state[id] == 2U) return;
        state[id] = 1U;
        closure.insert(id);
        std::vector<std::string> dependencies = found->second->dependencies;
        std::sort(dependencies.begin(), dependencies.end());
        for (const auto& dependency : dependencies) {
            if (by_id.find(dependency) == by_id.end()) {
                add_diagnostic(context.plan, "package.artifact-dependency-missing", id,
                    "Cook artifact dependency is absent: " + dependency);
            } else {
                visit(dependency);
            }
        }
        state[id] = 2U;
    };
    for (const auto& root : roots) visit(root);
    context.plan.asset_closure.assign(closure.begin(), closure.end());
    for (const auto& id : context.plan.asset_closure) add_artifact(context, *by_id.at(id));
}

std::string plan_material(const PackagePlan& plan) {
    std::ostringstream material;
    material << plan.project_id << '\n' << plan.target_profile << '\n' << plan.game_profile << '\n'
             << path_text(plan.staging_root) << '\n';
    for (const auto& asset : plan.asset_closure) material << "asset:" << asset << '\n';
    for (const auto& entry : plan.entries) {
        material << "entry:" << entry.id << '|' << entry.role << '|'
                 << path_text(entry.staging_path) << '|' << entry.content_hash << '|'
                 << entry.bytes << '|' << entry.license_id << '\n';
        for (const auto& dependency : entry.dependencies) material << "dep:" << dependency << '\n';
    }
    for (const auto& license : plan.license_ledger) {
        const auto& descriptor = license.descriptor;
        material << "license:" << descriptor.id << '|' << descriptor.name << '|'
                 << descriptor.spdx_id << '|' << descriptor.source_uri << '|'
                 << descriptor.third_party << '|' << descriptor.redistributable << '|'
                 << descriptor.notice << '|' << license.identifier_kind << '|'
                 << license.scope << '\n';
        for (const auto& entry : license.entry_references)
            material << "license-entry:" << entry << '\n';
        for (const auto& root : license.required_roots)
            material << "license-root:" << root << '\n';
    }
    return material.str();
}

void finalize_plan(PackagePlan& plan) {
    std::sort(plan.entries.begin(), plan.entries.end(), [](const auto& left, const auto& right) {
        const auto left_path = path_text(left.staging_path);
        const auto right_path = path_text(right.staging_path);
        return left_path == right_path ? left.id < right.id : left_path < right_path;
    });
    std::sort(plan.asset_closure.begin(), plan.asset_closure.end());
    plan.content_hash = content_hash(plan_material(plan));
    plan.plan_id = "package-" + hex_u64(fnv1a(plan.content_hash));
    if (!plan.diagnostics.empty()) {
        plan.valid = false;
        plan.code = first_error_code(plan);
        plan.detail = first_error_detail(plan);
    } else {
        plan.valid = true;
        plan.code = "ok";
        plan.detail = "Windows package plan is closed, licensed and ready for an atomic commit callback.";
    }
}

Json diagnostic_json(const PackageDiagnostic& diagnostic) {
    return Json{{"code", diagnostic.code}, {"path", diagnostic.path}, {"message", diagnostic.message}};
}

Json stage_entry_json(const PackageStageEntry& entry) {
    return Json{
        {"id", entry.id},
        {"role", entry.role},
        {"source", path_text(entry.source_path)},
        {"staging", path_text(entry.staging_path)},
        {"contentHash", entry.content_hash},
        {"bytes", entry.bytes},
        {"license", entry.license_id},
        {"dependencies", entry.dependencies}
    };
}

Json license_json(const PackageLicenseDescriptor& license) {
    return Json{
        {"id", license.id},
        {"name", license.name},
        {"spdxId", license.spdx_id},
        {"notice", license.notice},
        {"sourceUri", license.source_uri},
        {"thirdParty", license.third_party},
        {"redistributable", license.redistributable}
    };
}

Json license_ledger_json(const PackageLicenseLedgerEntry& license) {
    auto output = license_json(license.descriptor);
    output["identifierKind"] = license.identifier_kind;
    output["scope"] = license.scope;
    output["entryReferences"] = license.entry_references;
    output["requiredRoots"] = license.required_roots;
    return output;
}

} // namespace

std::vector<std::string> collect_scene_asset_ids(const SceneDocument& scene) {
    std::set<std::string> result;
    for(const auto& entity:scene.entities) {
        if(entity.mesh_renderer&&!entity.mesh_renderer->mesh_asset.empty())result.insert(entity.mesh_renderer->mesh_asset);
        if(entity.sprite_renderer&&!entity.sprite_renderer->sprite_asset.empty())result.insert(entity.sprite_renderer->sprite_asset);
        if(entity.tilemap_renderer&&!entity.tilemap_renderer->tilemap_asset.empty())result.insert(entity.tilemap_renderer->tilemap_asset);
        if(entity.animation_player) {
            if(!entity.animation_player->clip_asset.empty())result.insert(entity.animation_player->clip_asset);
            if(!entity.animation_player->state_machine_asset.empty())result.insert(entity.animation_player->state_machine_asset);
            if(!entity.animation_player->animation_graph_asset.empty())result.insert(entity.animation_player->animation_graph_asset);
            if(!entity.animation_player->next_clip_asset.empty())result.insert(entity.animation_player->next_clip_asset);
        }
        if(entity.pbr_material&&!entity.pbr_material->base_color_texture.empty())result.insert(entity.pbr_material->base_color_texture);
        if(entity.managed_script&&!entity.managed_script->assembly_asset.empty())result.insert(entity.managed_script->assembly_asset);
    }
    return {result.begin(),result.end()};
}

PackagePlan plan_package(const PackageInput& input, const PackageFileProbe& probe) {
    PackagePlan plan;
    plan.dry_run = input.dry_run;
    plan.project_id = input.project.project_id;
    plan.target_profile = input.target_profile.id;
    plan.game_profile = input.game_profile.id;
    plan.staging_root = input.staging_root.lexically_normal();

    if (input.staging_root.empty())
        add_diagnostic(plan, "package.staging-root-missing", "/stagingRoot", "Package staging root is required.");
    validate_project(input, plan);
    validate_game_profile(input, plan);
    validate_target_profile(input, plan);
    validate_cook_manifest(input, plan);

    PlanningContext context{plan, input, probe};
    collect_licenses(context);

    // Keep the package source boundary explicit.  Runtime files may be
    // produced outside the project root, while the startup scene remains a
    // project-owned relative path.
    add_startup_scene(context);
    add_project_hud(context);
    add_runtime(context);
    add_script(context);
    close_artifacts(context);
    validate_entry_licenses(context);
    add_game_generated_files(context);

    finalize_plan(plan);
    return plan;
}

PackageReceipt commit_package(const PackagePlan& plan, const PackageCommitCallback& commit) {
    PackageReceipt receipt;
    receipt.dry_run = plan.dry_run;
    receipt.plan_id = plan.plan_id;
    receipt.content_hash = plan.content_hash;
    receipt.entries = plan.entries;
    if (!plan.valid) {
        receipt.code = "package.plan-invalid";
        receipt.detail = "An invalid PackagePlan cannot be committed.";
        add_diagnostic(receipt, "package.plan-invalid", plan.plan_id, receipt.detail);
        return receipt;
    }
    if (plan.dry_run) {
        receipt.success = true;
        receipt.code = "package.dry-run";
        receipt.detail = "Package plan validated; no files were copied or committed.";
        return receipt;
    }
    if (!commit) {
        receipt.code = "package.commit-callback-missing";
        receipt.detail = "A non-dry-run package requires an atomic commit callback.";
        add_diagnostic(receipt, receipt.code, plan.plan_id, receipt.detail);
        return receipt;
    }
    PackageCommitResult result;
    try {
        result = commit(PackageCommitRequest{plan.plan_id, plan.content_hash, plan.staging_root, plan.entries});
    } catch (const std::exception& error) {
        receipt.code = "package.commit-callback-failed";
        receipt.detail = error.what();
        add_diagnostic(receipt, receipt.code, plan.plan_id, receipt.detail);
        return receipt;
    }
    receipt.code = result.code.empty() ? (result.success ? "ok" : "package.commit-failed") : result.code;
    receipt.detail = result.detail;
    receipt.atomic = result.atomic;
    receipt.commit_id = result.commit_id;
    if (!result.success) {
        add_diagnostic(receipt, receipt.code, plan.plan_id,
            result.detail.empty() ? "Atomic package commit callback reported failure." : result.detail);
        return receipt;
    }
    if (!result.atomic) {
        receipt.code = "package.commit-not-atomic";
        receipt.detail = "Package commit callback succeeded without proving atomicity.";
        add_diagnostic(receipt, receipt.code, plan.plan_id, receipt.detail);
        return receipt;
    }
    receipt.success = true;
    receipt.committed = true;
    if (receipt.detail.empty()) receipt.detail = "Package committed atomically by the host callback.";
    return receipt;
}

std::string package_plan_json(const PackagePlan& plan) {
    Json entries = Json::array();
    for (const auto& entry : plan.entries) entries.push_back(stage_entry_json(entry));
    Json licenses = Json::array();
    for (const auto& license : plan.licenses) licenses.push_back(license_json(license));
    Json license_ledger = Json::array();
    for (const auto& license : plan.license_ledger) license_ledger.push_back(license_ledger_json(license));
    Json diagnostics = Json::array();
    for (const auto& diagnostic : plan.diagnostics) diagnostics.push_back(diagnostic_json(diagnostic));
    return Json{
        {"schema", plan.schema},
        {"valid", plan.valid},
        {"dryRun", plan.dry_run},
        {"code", plan.code},
        {"detail", plan.detail},
        {"planId", plan.plan_id},
        {"contentHash", plan.content_hash},
        {"projectId", plan.project_id},
        {"targetProfile", plan.target_profile},
        {"gameProfile", plan.game_profile},
        {"stagingRoot", path_text(plan.staging_root)},
        {"assetClosure", plan.asset_closure},
        {"entries", std::move(entries)},
        {"licenses", std::move(licenses)},
        // Closed provenance is separate from the compatibility descriptor list.
        {"licenseLedger", std::move(license_ledger)},
        {"generated", {
            {"gameProfile", plan.game_profile_json},
            {"assetRegistry",plan.content_registry_json},
            {"thirdPartyLicenses", plan.third_party_license_json},
            {"notice", plan.notice_text}
        }},
        {"diagnostics", std::move(diagnostics)}
    }.dump();
}

std::string package_receipt_json(const PackageReceipt& receipt) {
    Json entries = Json::array();
    for (const auto& entry : receipt.entries) entries.push_back(stage_entry_json(entry));
    Json diagnostics = Json::array();
    for (const auto& diagnostic : receipt.diagnostics) diagnostics.push_back(diagnostic_json(diagnostic));
    return Json{
        {"schema", receipt.schema},
        {"success", receipt.success},
        {"dryRun", receipt.dry_run},
        {"committed", receipt.committed},
        {"atomic", receipt.atomic},
        {"code", receipt.code},
        {"detail", receipt.detail},
        {"planId", receipt.plan_id},
        {"contentHash", receipt.content_hash},
        {"commitId", receipt.commit_id},
        {"entries", std::move(entries)},
        {"diagnostics", std::move(diagnostics)}
    }.dump();
}

} // namespace noemancer
