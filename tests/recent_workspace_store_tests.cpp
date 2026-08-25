#include "editor/recent_workspace_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using noemancer::RecentWorkspaceStore;
using noemancer::RecentWorkspaceStoreOptions;
using noemancer::StartupHubRecentProject;

void require(const bool condition, const std::string_view detail) {
    if (!condition) throw std::runtime_error(std::string(detail));
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "Could not open a recent-workspace fixture for writing.");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "Could not write a recent-workspace fixture completely.");
}

bool same_path(std::string left, std::string right) {
    left = std::filesystem::path(left).lexically_normal().generic_string();
    right = std::filesystem::path(right).lexically_normal().generic_string();
#ifdef _WIN32
    std::ranges::transform(left, left.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    std::ranges::transform(right, right.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return left == right;
}

const StartupHubRecentProject* find_project(
    const std::vector<StartupHubRecentProject>& projects,
    const std::filesystem::path& path) {
    const auto found = std::ranges::find_if(projects, [&](const StartupHubRecentProject& project) {
        return same_path(project.path, path.generic_string());
    });
    return found == projects.end() ? nullptr : &*found;
}

int run() {
    const auto nonce = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("noemancer-recent-workspace-store-" + nonce);
    struct Cleanup final {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::filesystem::create_directories(root);

    const auto storage = root / "recent-workspaces.json";
    RecentWorkspaceStore store({.storage_path = storage, .max_entries = 3U,
                                .max_file_bytes = 64U * 1024U});
    const auto initially_loaded = store.load();
    require(initially_loaded.success ||
        (store.snapshot().healthy && store.snapshot().projects.empty()),
        "A missing persistence file was not treated as an empty healthy store.");
    require(store.snapshot().loaded && store.snapshot().projects.empty(),
        "Initial load did not publish an empty loaded snapshot.");

    const auto project_a = root / "A";
    const auto project_b = root / "B";
    const auto project_c = root / "C";
    const auto project_d = root / "D";
    for (const auto& path : {project_a, project_b, project_c, project_d})
        std::filesystem::create_directories(path);
    const auto boundary_revision = store.snapshot().revision;
    const auto empty_path = store.record_opened({}, "Invalid", 1U);
    const auto long_name = store.record_opened(project_a.generic_string(), std::string(257U, 'n'), 1U);
    require(!empty_path.success && !long_name.success &&
        store.snapshot().revision == boundary_revision && store.snapshot().projects.empty(),
        "Invalid path/name boundaries changed the committed snapshot.");
    const auto a = store.record_opened(project_a.generic_string(), "Alpha", 100U);
    const auto b = store.record_opened(project_b.generic_string(), "Bravo", 200U);
    const auto c = store.record_opened(project_c.generic_string(), "Charlie", 150U);
    require(a.success && b.success && c.success && a.revision < b.revision && b.revision < c.revision,
        "Recording three workspaces did not produce monotonic committed revisions.");
    require(store.snapshot().projects.size() == 3U &&
        same_path(store.snapshot().projects[0].path, project_b.generic_string()) &&
        same_path(store.snapshot().projects[1].path, project_c.generic_string()) &&
        same_path(store.snapshot().projects[2].path, project_a.generic_string()),
        "Recent workspaces were not sorted by last-opened time.");

    RecentWorkspaceStore restarted({.storage_path = storage, .max_entries = 3U,
                                    .max_file_bytes = 64U * 1024U});
    const auto restarted_load = restarted.load();
    require(restarted_load.success && restarted.snapshot().healthy &&
        restarted.snapshot().revision == store.snapshot().revision &&
        restarted.snapshot().projects.size() == 3U,
        "A new Store instance did not recover the committed snapshot.");

    auto duplicate_path = project_a.generic_string();
#ifdef _WIN32
    std::ranges::transform(duplicate_path, duplicate_path.begin(), [](const unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
#else
    duplicate_path = (project_a / ".." / "A").generic_string();
#endif
    const auto updated = restarted.record_opened(duplicate_path, "Alpha Updated", 300U);
    require(updated.success && restarted.snapshot().projects.size() == 3U,
        "Recording a normalized duplicate created another recent entry.");
    const auto* updated_a = find_project(restarted.snapshot().projects, project_a);
    require(updated_a != nullptr && updated_a->display_name == "Alpha Updated" &&
        updated_a->last_opened_unix_seconds == 300U,
        "The duplicate record did not update its existing entry.");

    const auto clipped = restarted.record_opened(project_d.generic_string(), "Delta", 400U);
    require(clipped.success && restarted.snapshot().projects.size() == 3U &&
        same_path(restarted.snapshot().projects[0].path, project_d.generic_string()) &&
        same_path(restarted.snapshot().projects[1].path, project_a.generic_string()) &&
        same_path(restarted.snapshot().projects[2].path, project_b.generic_string()) &&
        find_project(restarted.snapshot().projects, project_c) == nullptr,
        "max_entries did not evict the stable least-recent entry.");

    RecentWorkspaceStore stable_order({.storage_path = root / "stable-order.json", .max_entries = 4U,
                                       .max_file_bytes = 64U * 1024U});
    require(stable_order.load().success &&
        stable_order.record_opened(project_b.generic_string(), "Bravo", 500U).success &&
        stable_order.record_opened(project_a.generic_string(), "Alpha", 500U).success,
        "Tie-order fixture could not be recorded.");
    const auto order_before = stable_order.snapshot().projects;
    RecentWorkspaceStore stable_order_restart({.storage_path = root / "stable-order.json", .max_entries = 4U,
                                               .max_file_bytes = 64U * 1024U});
    require(stable_order_restart.load().success &&
        stable_order_restart.snapshot().projects.size() == order_before.size(),
        "Stable tie ordering was not persisted.");
    for (std::size_t index = 0U; index < order_before.size(); ++index)
        require(same_path(stable_order_restart.snapshot().projects[index].path, order_before[index].path),
            "Equal timestamps changed order across a Store restart.");

    const auto observation = nlohmann::json::parse(restarted.observation_json(), nullptr, false);
    require(observation.is_object() &&
        observation.value("schemaVersion", std::string{}) == "noemancer.recent-workspaces/0.1" &&
        observation.value("revision", std::uint64_t{}) == restarted.snapshot().revision,
        "Observation schema or revision does not describe the committed snapshot.");
    require(restarted.observation_json().find(storage.generic_string()) == std::string::npos,
        "Observation leaked the persistence file's absolute storage path.");
    require(restarted.observation_json().find(storage.filename().generic_string()) == std::string::npos,
        "Observation exposed the private persistence filename.");

    const auto malformed_path = root / "malformed.json";
    write_text(malformed_path, "{ definitely not json");
    RecentWorkspaceStore malformed({.storage_path = malformed_path, .max_entries = 3U,
                                    .max_file_bytes = 64U * 1024U});
    const auto malformed_load = malformed.load();
    require(!malformed_load.success && !malformed.snapshot().healthy &&
        !malformed_load.code.empty() && !malformed_load.detail.empty(),
        "Malformed JSON did not produce a bounded diagnostic.");
    const auto recovered = malformed.record_opened(project_a.generic_string(), "Recovered", 600U);
    require(recovered.success && recovered.recovered && malformed.snapshot().healthy &&
        malformed.snapshot().projects.size() == 1U,
        "A record after malformed JSON did not atomically recover the store.");
    RecentWorkspaceStore recovered_restart({.storage_path = malformed_path, .max_entries = 3U,
                                            .max_file_bytes = 64U * 1024U});
    require(recovered_restart.load().success && recovered_restart.snapshot().projects.size() == 1U,
        "Recovered persistence was not valid after restart.");

    const auto oversized_path = root / "oversized.json";
    write_text(oversized_path, std::string(1025U, 'x'));
    RecentWorkspaceStore oversized_store({.storage_path = oversized_path, .max_entries = 3U,
                                          .max_file_bytes = 1024U});
    const auto oversized_load = oversized_store.load();
    require(!oversized_load.success && !oversized_store.snapshot().healthy &&
        !oversized_load.code.empty() && !oversized_load.detail.empty(),
        "An over-budget persistence document did not fail with a diagnostic.");

    const auto wrong_type_path = root / "wrong-type.json";
    write_text(wrong_type_path,
        R"({"schemaVersion":"noemancer.recent-workspaces/0.1","revision":"old","projects":{}})");
    RecentWorkspaceStore wrong_type({.storage_path = wrong_type_path, .max_entries = 3U,
                                     .max_file_bytes = 64U * 1024U});
    const auto wrong_type_load = wrong_type.load();
    require(!wrong_type_load.success && !wrong_type.snapshot().healthy &&
        !wrong_type_load.code.empty() && !wrong_type_load.detail.empty(),
        "Wrong persistence field types did not fail with a diagnostic.");

    const auto directory_target = root / "storage-is-directory";
    std::filesystem::create_directories(directory_target);
    RecentWorkspaceStore unwritable({.storage_path = directory_target, .max_entries = 3U,
                                     .max_file_bytes = 64U * 1024U});
    const auto before_failed_write = unwritable.snapshot();
    const auto failed_write = unwritable.record_opened(project_a.generic_string(), "Must Not Publish", 700U);
    require(!failed_write.success && unwritable.snapshot().revision == before_failed_write.revision &&
        unwritable.snapshot().projects.size() == before_failed_write.projects.size() &&
        std::filesystem::is_directory(directory_target),
        "A persistence write failure published its uncommitted candidate.");
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& exception) {
        std::cerr << "recent_workspace_store_tests: " << exception.what() << '\n';
        return 100;
    } catch (...) {
        std::cerr << "recent_workspace_store_tests: unknown failure\n";
        return 101;
    }
}
