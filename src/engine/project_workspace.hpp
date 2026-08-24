#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace noemancer {

// Workspace presets are deliberately represented as stable plain-data
// strings.  This keeps the request aggregate-compatible for existing callers
// while leaving room for CLI/Editor surfaces to discover the same values
// without a second schema.
inline constexpr std::string_view project_workspace_preset_starter = "starter";
inline constexpr std::string_view project_workspace_preset_hybrid_pixel = "hybrid-pixel";

struct ProjectWorkspaceCreateRequest final {
    std::filesystem::path root;
    std::string name;
    // Omitting the field preserves the original starter workspace exactly.
    std::string preset{std::string(project_workspace_preset_starter)};
};

// Creates a complete starter project through a sibling staging directory and
// returns a stable JSON receipt. Existing destinations are never overwritten.
[[nodiscard]] std::string create_project_workspace_json(const ProjectWorkspaceCreateRequest& request);

} // namespace noemancer
