#pragma once

#include <filesystem>
#include <string>

namespace noemancer {

struct WindowsPackageOptions final {
    std::filesystem::path project_path;
    std::filesystem::path output_path;
    std::filesystem::path runtime_executable;
    std::string target_profile{"windows-x64-release"};
    bool dry_run{};
};

// Runs the Windows package boundary and returns a stable JSON envelope. The
// service delegates validation and the PackagePlan/Receipt contract to the
// engine package pipeline; this layer only supplies real file observations
// and performs the final host-owned atomic directory commit.
[[nodiscard]] std::string run_windows_package_json(const WindowsPackageOptions& options);

} // namespace noemancer
