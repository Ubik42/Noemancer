#include "runtime/source_editor_service.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

} // namespace

int main() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path();
    const auto root = temp / ("noemancer-source-editor-" + unique);
    const auto outside = temp / ("noemancer-source-editor-outside-" + unique + ".cs");
    const auto source = root / "scripts" / "Gameplay.cs";
    const auto project = root / "Gameplay.csproj";
    const auto unsupported = root / "notes.txt";
    write_text(source, "namespace Game; public sealed class Gameplay {}\n");
    write_text(project, "<Project Sdk=\"Microsoft.NET.Sdk\" />\n");
    write_text(unsupported, "not source\n");
    write_text(outside, "namespace Outside;\n");

    const auto source_receipt = nlohmann::json::parse(
        noemancer::launch_source_editor_json(root, "scripts/Gameplay.cs", 7U, 4U, true));
    if (!source_receipt.value("success", false) || !source_receipt.value("dryRun", false) ||
        source_receipt.value("code", std::string{}) != "source-editor.plan-ready" ||
        source_receipt.value("projectRelativePath", std::string{}) != "scripts/Gameplay.cs" ||
        source_receipt.value("line", 0U) != 7U || source_receipt.value("column", 0U) != 4U) {
        std::cerr << source_receipt.dump() << '\n';
        return 1;
    }
    const auto project_receipt = nlohmann::json::parse(
        noemancer::launch_source_editor_json(root, project, 0U, 0U, true));
    if (!project_receipt.value("success", false) || project_receipt.value("line", 0U) != 1U ||
        project_receipt.value("column", 0U) != 1U) {
        std::cerr << project_receipt.dump() << '\n';
        return 2;
    }
    const auto outside_receipt = nlohmann::json::parse(
        noemancer::launch_source_editor_json(root, outside, 1U, 1U, true));
    if (outside_receipt.value("success", true) || !outside_receipt.value("dryRun", false) ||
        outside_receipt.value("code", std::string{}) != "source-editor.path-invalid") {
        std::cerr << outside_receipt.dump() << '\n';
        return 3;
    }
    const auto unsupported_receipt = nlohmann::json::parse(
        noemancer::launch_source_editor_json(root, unsupported, 1U, 1U, true));
    if (unsupported_receipt.value("success", true) ||
        unsupported_receipt.value("code", std::string{}) != "source-editor.type-not-supported") {
        std::cerr << unsupported_receipt.dump() << '\n';
        return 4;
    }
    const auto missing_receipt = nlohmann::json::parse(
        noemancer::launch_source_editor_json(root, root / "Missing.cs", 1U, 1U, true));
    if (missing_receipt.value("success", true) ||
        missing_receipt.value("code", std::string{}) != "source-editor.path-invalid") {
        std::cerr << missing_receipt.dump() << '\n';
        return 5;
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove(outside, cleanup_error);
    return cleanup_error ? 6 : 0;
}
