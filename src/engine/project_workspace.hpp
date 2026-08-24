#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace noemancer {

struct ProjectWorkspaceCreateRequest final {
    std::filesystem::path root;
    std::string name;
};

// Creates a complete starter project through a sibling staging directory and
// returns a stable JSON receipt. Existing destinations are never overwritten.
[[nodiscard]] std::string create_project_workspace_json(const ProjectWorkspaceCreateRequest& request);

} // namespace noemancer
