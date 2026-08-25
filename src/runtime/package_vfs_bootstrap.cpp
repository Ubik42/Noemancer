#include "runtime/package_vfs_bootstrap.hpp"

#include "engine/semantic_ui.hpp"
#include "engine/vfs_document_reader.hpp"

#include <string_view>

#include <nlohmann/json.hpp>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::string_view profile_uri = "package://config/game-profile.json";

[[nodiscard]] PackageVfsBootstrapResult failure(
    std::string code, std::string detail, std::shared_ptr<VirtualFileSystem> vfs = {}) {
    PackageVfsBootstrapResult result;
    result.receipt.code = std::move(code);
    result.receipt.detail = std::move(detail);
    result.receipt.profile_uri = std::string(profile_uri);
    result.vfs = std::move(vfs);
    result.receipt.vfs_revision = result.vfs ? result.vfs->revision() : 0U;
    return result;
}

[[nodiscard]] bool supported_schema(const std::string_view schema) noexcept {
    return schema == "noemancer.game-profile/0.1" || schema == "noemancer.game-profile/0.2" ||
        schema == "noemancer.game-profile/0.3" || schema == "noemancer.game-profile/0.4";
}

[[nodiscard]] bool safe_relative_path(const std::string_view text, const std::size_t max_bytes) {
    if (text.empty() || text.size() > max_bytes || text.find('\\') != std::string_view::npos ||
        text.find(':') != std::string_view::npos || text.front() == '/' || text.back() == '/') return false;
    std::size_t begin{};
    while (begin < text.size()) {
        const auto end = text.find('/', begin);
        const auto component = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (component.empty() || component == "." || component == "..") return false;
        for (const auto value : component) {
            if (static_cast<unsigned char>(value) < 0x20U || value == '?' || value == '#' || value == '%') return false;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return true;
}

[[nodiscard]] bool optional_string(const Json& profile, const std::string_view key) {
    const auto found = profile.find(std::string(key));
    return found == profile.end() || found->is_string();
}

[[nodiscard]] bool optional_package_path(
    const Json& profile, const std::string_view key, const std::size_t max_bytes) {
    const auto found = profile.find(std::string(key));
    return found == profile.end() || (found->is_string() &&
        (found->get_ref<const std::string&>().empty() ||
         safe_relative_path(found->get_ref<const std::string&>(), max_bytes)));
}

[[nodiscard]] std::string validate_profile(const Json& profile, const PackageVfsBootstrapLimits& limits) {
    if (!profile.is_object() || profile.size() > 64U) return "Profile must be a bounded JSON object.";
    const auto schema = profile.find("schema");
    if (schema == profile.end() || !schema->is_string() || !supported_schema(schema->get_ref<const std::string&>()))
        return "Profile schema is not one of the supported game-profile revisions.";
    const auto display_name = profile.find("displayName");
    if (display_name != profile.end() && (!display_name->is_string() ||
        display_name->get_ref<const std::string&>().empty() || display_name->get_ref<const std::string&>().size() > 256U))
        return "displayName must be a non-empty bounded string when present.";
    const auto startup_scene = profile.find("startupScene");
    if (startup_scene == profile.end() || !startup_scene->is_string() ||
        !safe_relative_path(startup_scene->get_ref<const std::string&>(), limits.max_relative_path_bytes))
        return "startupScene must be a canonical package-content-relative path.";
    const auto hud = profile.find("hudDocument");
    if (hud != profile.end() && (!hud->is_string() ||
        (!hud->get_ref<const std::string&>().empty() &&
         !safe_relative_path(hud->get_ref<const std::string&>(), limits.max_relative_path_bytes))))
        return "hudDocument must be empty or a canonical package-content-relative path.";

    constexpr std::string_view string_fields[] = {"id", "platform", "architecture", "configuration", "executable",
        "projectId", "targetProfile", "startupSceneGuid", "managedAssembly", "managedConfiguration", "assetRegistry"};
    for (const auto field : string_fields)
        if (!optional_string(profile, field)) return std::string(field) + " must be a string when present.";
    for (const auto field : {std::string_view{"managedAssembly"}, std::string_view{"assetRegistry"}})
        if (!optional_package_path(profile, field, limits.max_relative_path_bytes))
            return std::string(field) + " must be empty or a canonical package-relative path.";
    if (profile.contains("runtimeRequirements") && !profile.at("runtimeRequirements").is_array())
        return "runtimeRequirements must be an array when present.";
    if (profile.contains("inputActions") && !profile.at("inputActions").is_array())
        return "inputActions must be an array when present.";
    if (profile.contains("packagedAssets") && !profile.at("packagedAssets").is_array())
        return "packagedAssets must be an array when present.";
    if (profile.contains("hybridPixelProfile") && !profile.at("hybridPixelProfile").is_object())
        return "hybridPixelProfile must be an object when present.";
    if(profile.contains("skyAtmosphere")&&!profile.at("skyAtmosphere").is_object())
        return "skyAtmosphere must be an object when present.";
    if(profile.contains("skyEnvironment")&&!profile.at("skyEnvironment").is_object())
        return "skyEnvironment must be an object when present.";
    return {};
}

[[nodiscard]] VfsDocumentReadResult read_document(
    const VirtualFileSystem& vfs, std::string uri, const std::size_t budget) {
    return read_vfs_document(vfs, VfsDocumentReadRequest{
        .uri = std::move(uri), .kind = VfsDocumentKind::json, .byte_budget = budget});
}

[[nodiscard]] std::string mapped_read_code(const std::string_view role, const VfsDocumentReadResult& read) {
    if (read.code == "vfs.read-budget-exceeded") return "package.bootstrap." + std::string(role) + "-budget-exceeded";
    if (read.code == "vfs.not-found") return "package.bootstrap." + std::string(role) + "-missing";
    if (read.code == "vfs.document-json-invalid" || read.code == "vfs.document-utf8-invalid" ||
        read.code == "vfs.document-json-canonicalization-failed")
        return "package.bootstrap." + std::string(role) + "-invalid";
    return "package.bootstrap." + std::string(role) + "-read-failed";
}

} // namespace

std::string PackageVfsBootstrapReceipt::json() const {
    return Json{{"schema", "noemancer.package-vfs-bootstrap-receipt/0.1"}, {"success", success},
        {"code", code}, {"detail", detail}, {"mountId", mount_id}, {"vfsRevision", vfs_revision},
        {"profile", {{"uri", profile_uri}, {"sha256", profile_sha256}, {"bytes", profile_bytes}}},
        {"startupScene", {{"uri", startup_scene_uri}, {"sha256", startup_scene_sha256},
            {"bytes", startup_scene_bytes}}},
        {"hudDocument", {{"uri", hud_document_uri}, {"sha256", hud_document_sha256},
            {"bytes", hud_document_bytes}}}}.dump();
}

PackageVfsBootstrapResult bootstrap_package_vfs(
    const std::filesystem::path& game_profile_path, PackageVfsBootstrapLimits limits) {
    if (limits.profile_byte_budget == 0U || limits.scene_byte_budget == 0U || limits.hud_byte_budget == 0U ||
        limits.max_relative_path_bytes == 0U)
        return failure("package.bootstrap.limits-invalid", "Document and path budgets must be non-zero.");

    std::error_code error;
    const auto canonical_profile = std::filesystem::canonical(game_profile_path, error);
    if (error || !std::filesystem::is_regular_file(canonical_profile, error) || error)
        return failure("package.bootstrap.profile-missing", "The explicit game profile trust root is not a regular file.");
    if (canonical_profile.filename() != "game-profile.json" || canonical_profile.parent_path().filename() != "config")
        return failure("package.bootstrap.profile-path-invalid", "The trust root must be config/game-profile.json.");
    const auto package_root = canonical_profile.parent_path().parent_path();
    if (package_root.empty() || !std::filesystem::is_directory(package_root, error) || error)
        return failure("package.bootstrap.package-root-invalid", "The profile does not identify an accessible package root.");

    auto vfs = std::make_shared<VirtualFileSystem>(limits.vfs_limits);
    const auto mount = vfs->mount(VfsMountSpec{.id = "runtime.package", .virtual_root = "package://",
        .source_root = package_root, .kind = VfsMountKind::package_directory, .priority = 100, .read_only = true});
    if (!mount.success) return failure("package.bootstrap.mount-failed", mount.detail, std::move(vfs));

    auto profile_read = read_document(*vfs, std::string(profile_uri), limits.profile_byte_budget);
    if (!profile_read.success)
        return failure(mapped_read_code("profile", profile_read), profile_read.detail, std::move(vfs));
    auto profile_text = std::move(profile_read.canonical_json);
    auto profile = Json::parse(profile_text, nullptr, false);
    if (const auto detail = validate_profile(profile, limits); !detail.empty())
        return failure("package.bootstrap.profile-invalid", detail, std::move(vfs));

    const auto scene_uri = "package://content/" + profile.at("startupScene").get<std::string>();
    auto scene_read = read_document(*vfs, scene_uri, limits.scene_byte_budget);
    if (!scene_read.success)
        return failure(mapped_read_code("startup-scene", scene_read), scene_read.detail, std::move(vfs));
    auto scene = SceneDocumentCodec::parse_json(scene_read.canonical_json, scene_uri);
    if (!scene)
        return failure("package.bootstrap.startup-scene-invalid", "startupScene does not satisfy the Scene document schema.",
            std::move(vfs));
    if (profile.contains("startupSceneGuid") && !profile.at("startupSceneGuid").get<std::string>().empty() &&
        profile.at("startupSceneGuid").get<std::string>() != scene.document->scene_guid)
        return failure("package.bootstrap.startup-scene-guid-mismatch",
            "startupSceneGuid does not match the parsed startup Scene.", std::move(vfs));

    std::string hud_uri;
    std::string hud_text;
    VfsDocumentReadResult hud_read;
    const auto hud_relative = profile.value("hudDocument", std::string{});
    if (!hud_relative.empty()) {
        hud_uri = "package://content/" + hud_relative;
        hud_read = read_document(*vfs, hud_uri, limits.hud_byte_budget);
        if (!hud_read.success)
            return failure(mapped_read_code("hud-document", hud_read), hud_read.detail, std::move(vfs));
        hud_text = std::move(hud_read.canonical_json);
        const auto validation = Json::parse(semantic_ui_validation_json(hud_text), nullptr, false);
        if (!validation.is_object() || !validation.value("valid", false))
            return failure("package.bootstrap.hud-document-invalid",
                "hudDocument does not satisfy the Semantic UI document schema.", std::move(vfs));
    }

    PackageVfsBootstrapResult result;
    result.vfs = std::move(vfs);
    result.profile = std::move(profile);
    result.profile_text = std::move(profile_text);
    result.scene = std::move(*scene.document);
    result.hud_document = std::move(hud_text);
    result.display_name = result.profile.value("displayName", std::string{"Noemancer Player"});
    result.package_root = package_root;
    result.receipt.success = true;
    result.receipt.code = "ok";
    result.receipt.detail = "The package profile, startup Scene and optional HUD were validated through VFS.";
    result.receipt.vfs_revision = result.vfs->revision();
    result.receipt.profile_uri = std::string(profile_uri);
    result.receipt.startup_scene_uri = scene_uri;
    result.receipt.hud_document_uri = std::move(hud_uri);
    result.receipt.profile_sha256 = std::move(profile_read.sha256);
    result.receipt.startup_scene_sha256 = std::move(scene_read.sha256);
    result.receipt.hud_document_sha256 = std::move(hud_read.sha256);
    result.receipt.profile_bytes = profile_read.file.total_bytes;
    result.receipt.startup_scene_bytes = scene_read.file.total_bytes;
    result.receipt.hud_document_bytes = hud_read.file.total_bytes;
    return result;
}

} // namespace noemancer
