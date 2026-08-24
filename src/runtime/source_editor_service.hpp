#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace noemancer {

// Runtime-owned adapter for opening a validated project source in an external
// editor. Dry-run performs the complete path/adapter plan without launching.
[[nodiscard]] std::string launch_source_editor_json(
    const std::filesystem::path& project_root,
    const std::filesystem::path& source_path,
    std::uint32_t line,
    std::uint32_t column,
    bool dry_run = false);

} // namespace noemancer
