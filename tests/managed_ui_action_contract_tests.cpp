#include "engine/scripting_runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using Json = nlohmann::json;

bool contains(const std::filesystem::path& path, const std::string_view needle) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    const std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return source.find(needle) != std::string::npos;
}
} // namespace

int main() {
#ifndef NOEMANCER_SOURCE_DIR
    const auto source = std::filesystem::path(NOEMANCER_SOURCE_DIR);
    if (!contains(source / "managed/Noemancer.Managed/ScriptBehaviour.cs",
                  "public virtual void OnUiAction(in ScriptContext context)")) return 1;
    if (!contains(source / "managed/Noemancer.ManagedHost/EntryPoint.cs", "\"OnUiAction\"")) return 2;
    if (!contains(source / "managed/Noemancer.ManagedHost/EntryPoint.cs",
                  "case \"OnUiAction\": instance.OnUiAction(in context); break;")) return 3;
#endif

    noemancer::ManagedScriptRuntime runtime;
    const auto abi = Json::parse(runtime.abi_json());
    if (abi.at("schemaVersion") != "noemancer.managed-script-abi/0.5") return 4;
    const auto& callbacks = abi.at("callbacks");
    if (!callbacks.is_array() ||
        std::ranges::none_of(callbacks, [](const Json& callback) { return callback == "OnUiAction"; })) return 5;

    std::cout << "Managed OnUiAction ABI and host dispatch contract is present\n";
    return 0;
}
