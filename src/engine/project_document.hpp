#pragma once

#include "engine/scene_document.hpp"
#include "engine/gameplay_runtime.hpp"
#include "engine/hybrid_pixel_profile.hpp"
#include "engine/virtual_file_system.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace noemancer {

struct ProjectDocument final {
    std::string schema{"noemancer.project/0.2"};
    std::string project_id;
    std::string name;
    std::filesystem::path root;
    std::filesystem::path startup_scene;
    std::vector<std::filesystem::path> asset_roots;
    // Stable Asset IDs that scripts, data tables or dynamic spawns may use
    // without a direct reference from the startup Scene.
    std::vector<std::string> packaged_assets;
    std::optional<std::filesystem::path> script_project;
    std::optional<std::filesystem::path> hud_document;
    std::string hud_document_json;
    std::vector<InputActionDefinition> input_actions;
    // One engine-owned profile authority.  The project manifest embeds the
    // complete value rather than a second project-specific pixel settings
    // shape, so Editor/Runtime/Agent can all consume the same plain data.
    std::optional<HybridPixelProfile> hybrid_pixel_profile;
};

struct ProjectLoadError final {
    std::string code;
    std::string path;
    std::string message;
};

struct ProjectLoadResult final {
    std::optional<ProjectDocument> project;
    std::optional<SceneDocument> startup_scene;
    std::vector<ProjectLoadError> errors;

    [[nodiscard]] explicit operator bool() const noexcept {
        return project.has_value() && startup_scene.has_value() && errors.empty();
    }
};

struct ProjectVfsLoadOptions final {
    std::size_t manifest_byte_budget{1024U * 1024U};
    std::size_t referenced_document_byte_budget{4U * 1024U * 1024U};
};

[[nodiscard]] ProjectLoadResult load_project(const std::filesystem::path& project_path);
// Loads the manifest and its Scene/HUD documents exclusively through VFS.
// trusted_project_root is an independently established authority used only
// for ProjectDocument relative-path semantics and later authoring; it is never
// derived from, or used to resolve, the virtual URI.
[[nodiscard]] ProjectLoadResult load_project(
    const VirtualFileSystem& vfs,
    std::string_view manifest_uri,
    const std::filesystem::path& trusted_project_root,
    ProjectVfsLoadOptions options = {});
[[nodiscard]] std::string project_load_errors_json(const ProjectLoadResult& result);

} // namespace noemancer
