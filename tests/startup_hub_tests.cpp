#include "editor/startup_hub.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using noemancer::StartupHubActionKind;
using noemancer::StartupHubModel;
using noemancer::StartupHubProjectStatus;
using noemancer::StartupHubRecentProject;

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "noemancer-startup-hub-test";
    const auto existing = root / "projects" / "alpha";
    const auto not_directory = root / "projects" / "not-a-project-directory";
    const auto missing = root / "projects" / "missing";
    const auto extra = root / "projects" / "extra";
    std::error_code filesystem_error;
    std::filesystem::remove_all(root, filesystem_error);
    std::filesystem::create_directories(existing, filesystem_error);
    std::filesystem::create_directories(extra, filesystem_error);
    std::filesystem::create_directories(not_directory.parent_path(), filesystem_error);
    std::filesystem::remove(not_directory, filesystem_error);
    { std::ofstream output(not_directory); output << "not a project directory"; }

    const auto existing_relative = std::filesystem::relative(existing, root).generic_string();
    const auto missing_relative = std::filesystem::relative(missing, root).generic_string();
    const auto not_directory_relative = std::filesystem::relative(not_directory, root).generic_string();
    const auto extra_relative = std::filesystem::relative(extra, root).generic_string();
    const noemancer::StartupHubOptions options{
        .max_recent_projects = 3U,
        .path_base = root.generic_string()};
    const std::vector<StartupHubRecentProject> input{
        {.path = existing_relative + "/../alpha", .display_name = "Duplicate", .last_opened_unix_seconds = 10U},
        {.path = existing_relative, .display_name = "Alpha", .last_opened_unix_seconds = 10U},
        {.path = missing_relative, .display_name = {}, .last_opened_unix_seconds = 30U},
        {.path = not_directory_relative, .display_name = "Not Directory", .last_opened_unix_seconds = 20U},
        {.path = extra_relative, .display_name = "Extra", .last_opened_unix_seconds = 1U}};

    StartupHubModel model(options);
    model.set_recent_projects(input);
    if (model.view().recent_projects.size() != 3U || model.view().selected_project_path.empty() ||
        model.view().recent_projects[0].status != StartupHubProjectStatus::missing ||
        model.view().recent_projects[1].status != StartupHubProjectStatus::not_directory ||
        model.view().recent_projects[2].status != StartupHubProjectStatus::available ||
        model.view().recent_projects[2].display_name != "Alpha" ||
        model.view().recent_projects[2].path.find("..") != std::string::npos) {
        std::cerr << "Startup Hub recent project normalization, status or stable sort failed\n";
        return 1;
    }
    if (model.view().recent_projects[0].last_opened_unix_seconds != 30U ||
        model.view().recent_projects[1].last_opened_unix_seconds != 20U ||
        model.view().recent_projects[2].last_opened_unix_seconds != 10U) {
        std::cerr << "Startup Hub recent project ordering was not deterministic\n";
        return 2;
    }

    StartupHubModel reversed(options);
    auto reversed_input = input;
    std::ranges::reverse(reversed_input);
    reversed.set_recent_projects(reversed_input);
    if (model.semantic_snapshot_json() != reversed.semantic_snapshot_json()) {
        std::cerr << "Startup Hub projection changed with input ordering\n";
        return 3;
    }

    if (!model.select_project(model.view().recent_projects[2].path) || !model.request_open_project()) {
        std::cerr << "Startup Hub could not queue an Open Project action\n";
        return 4;
    }
    const auto open = model.consume_request();
    if (!open || open->kind != StartupHubActionKind::open_project ||
        open->project_path != model.view().selected_project_path || open->request_id.empty()) {
        std::cerr << "Open Project request was not normalized or stable\n";
        return 5;
    }
    if (!model.request_new_project("new/../new-project") || model.request_empty_workspace()) {
        std::cerr << "Startup Hub pending-action serialization or New Project validation failed\n";
        return 6;
    }
    const auto new_project = model.consume_request();
    if (!new_project || new_project->kind != StartupHubActionKind::new_project ||
        new_project->project_name != "new-project" || new_project->project_path.find("..") != std::string::npos) {
        std::cerr << "New Project request did not use normalized path/name data\n";
        return 7;
    }
    if (!model.request_empty_workspace()) {
        std::cerr << "Empty Workspace action could not be queued\n";
        return 8;
    }
    const auto empty_workspace = model.consume_request();
    if (!empty_workspace || empty_workspace->kind != StartupHubActionKind::empty_workspace ||
        !empty_workspace->project_path.empty() || !empty_workspace->project_name.empty()) {
        std::cerr << "Empty Workspace request contained project-specific state\n";
        return 9;
    }

    const auto semantic = nlohmann::json::parse(model.semantic_snapshot_json(), nullptr, false);
    if (semantic.is_discarded() || semantic.value("schemaVersion", "") != "noemancer.startup-hub/0.1" ||
        semantic.at("recentProjects").size() != 3U || semantic.at("actions").size() != 3U ||
        semantic.at("brand").value("id", "") != "noemancer" ||
        semantic.at("recentProjects").at(0).value("status", "") != "missing" ||
        !semantic.at("recentProjects").at(2).value("selected", false)) {
        std::cerr << "Startup Hub semantic observation was incomplete\n";
        return 10;
    }

    const auto brand = model.brand();
    const auto svg = StartupHubModel::brand_svg();
    if (brand.logo.size() != 3U || svg.find("<svg") == std::string::npos ||
        svg.find("linearGradient") != std::string::npos || svg.find("filter") != std::string::npos ||
        svg.find("#D4A15D") == std::string::npos) {
        std::cerr << "Startup Hub brand did not expose flat self-authored vector data\n";
        return 11;
    }

    std::filesystem::remove_all(root, filesystem_error);
    std::cout << "Startup Hub model contract passed\n";
    return 0;
}
