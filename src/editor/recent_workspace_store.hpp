#pragma once

#include "editor/startup_hub.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace noemancer {

struct RecentWorkspaceStoreOptions final {
    std::filesystem::path storage_path;
    std::size_t max_entries{12U};
    std::size_t max_file_bytes{64U * 1024U};
};

struct RecentWorkspaceStoreReceipt final {
    bool success{};
    std::string code;
    std::string detail;
    std::uint64_t revision{};
    std::vector<StartupHubRecentProject> projects;
    bool recovered{};
};

struct RecentWorkspaceStoreSnapshot final {
    std::uint64_t revision{};
    std::vector<StartupHubRecentProject> projects;
    bool loaded{};
    bool healthy{};
    std::string code{"recent-workspaces.unloaded"};
    std::string detail{"The recent workspace store has not been loaded."};
};

class RecentWorkspaceStore final {
public:
    explicit RecentWorkspaceStore(RecentWorkspaceStoreOptions options);

    [[nodiscard]] const RecentWorkspaceStoreOptions& options() const noexcept;
    [[nodiscard]] RecentWorkspaceStoreReceipt load();
    [[nodiscard]] RecentWorkspaceStoreReceipt record_opened(
        std::string_view path, std::string_view display_name, std::uint64_t unix_seconds);
    [[nodiscard]] const RecentWorkspaceStoreSnapshot& snapshot() const noexcept;
    [[nodiscard]] std::string observation_json() const;

private:
    RecentWorkspaceStoreOptions options_;
    RecentWorkspaceStoreSnapshot snapshot_;
};

} // namespace noemancer
