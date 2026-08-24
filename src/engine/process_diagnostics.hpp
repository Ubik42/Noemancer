#pragma once

#include <string_view>

namespace noemancer {

// Optional persistent output for fatal process diagnostics.  The directory is
// deliberately opt-in: normal editor/test processes only write their compact
// JSON evidence to stderr and leave no files behind.  When a directory is
// supplied, one sidecar named `noemancer-<role>-<pid>.fatal.json` is written on
// the first fatal event.
struct process_diagnostics_options {
    std::string_view process_role{"unknown"};
    std::string_view report_directory{};
};

// Routes fatal diagnostics to stderr and disables modal OS/CRT error dialogs so
// headless tools and Agents always receive machine-capturable failure evidence.
void configure_process_diagnostics(std::string_view process_role);
void configure_process_diagnostics(const process_diagnostics_options& options);

} // namespace noemancer
