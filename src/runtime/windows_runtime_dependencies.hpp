#pragma once

#include <string>

namespace noemancer {

// Runtime-owned observation of the native loader result. This is deliberately
// structured evidence: Package planning declares intent, while this probe
// proves where the current process actually resolved its VC Runtime modules.
[[nodiscard]] std::string native_runtime_dependencies_json();

} // namespace noemancer
