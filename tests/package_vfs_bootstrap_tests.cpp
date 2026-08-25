#include "runtime/package_vfs_bootstrap.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

bool check(const bool condition, const std::string_view message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

struct Fixture final {
    Fixture() {
        std::error_code error;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path(error) /
            ("noemancer-package-vfs-bootstrap-tests-" + std::to_string(nonce));
        std::filesystem::create_directories(root, error);
        valid = !error;
    }
    ~Fixture() {
        if (!valid || root.empty() || !root.filename().string().starts_with("noemancer-package-vfs-bootstrap-tests-"))
            return;
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    bool write(const std::string_view relative, const std::string_view text) const {
        std::error_code error;
        const auto path = root / std::filesystem::path(relative);
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(output);
    }
    std::filesystem::path profile() const { return root / "config" / "game-profile.json"; }
    std::filesystem::path root;
    bool valid{};
};

std::string scene_json() {
    return R"({"schema":"noemancer.scene/0.1","sceneGuid":"scene.package.start","name":"Packaged Start","entities":[]})";
}

std::string hud_json() {
    return R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"game.hud","nodes":[{"id":"hud.root","parentId":null,"role":"panel"}]})";
}

std::string profile_json(std::string startup = "scenes/start.scene.json", std::string hud = "ui/hud.ui.json") {
    return Json{{"schema", "noemancer.game-profile/0.4"}, {"id", "game.fixture"},
        {"displayName", "Package Fixture"}, {"startupScene", std::move(startup)},
        {"startupSceneGuid", "scene.package.start"}, {"hudDocument", std::move(hud)},
        {"packagedAssets", Json::array({"texture.fixture.hero"})}, {"inputActions", Json::array()},
        {"runtimeRequirements", Json::array()}}.dump();
}

bool make_valid_package(Fixture& fixture) {
    return fixture.valid && fixture.write("config/game-profile.json", profile_json()) &&
        fixture.write("content/scenes/start.scene.json", scene_json()) &&
        fixture.write("content/ui/hud.ui.json", hud_json());
}

} // namespace

int main() {
    bool valid = true;

    Fixture package;
    valid = check(make_valid_package(package), "Could not construct the package fixture.") && valid;
    const auto loaded = noemancer::bootstrap_package_vfs(package.profile());
    const auto receipt = Json::parse(loaded.receipt.json(), nullptr, false);
    valid = check(loaded && loaded.vfs && loaded.package_root == std::filesystem::canonical(package.root) &&
            loaded.display_name == "Package Fixture" && loaded.scene.scene_guid == "scene.package.start" &&
            loaded.scene.source_uri == "package://content/scenes/start.scene.json" &&
            Json::parse(loaded.hud_document) == Json::parse(hud_json()) &&
            loaded.profile.at("schema") == "noemancer.game-profile/0.4" &&
            loaded.receipt.vfs_revision == 1U && !loaded.receipt.profile_sha256.empty() &&
            !loaded.receipt.startup_scene_sha256.empty() && !loaded.receipt.hud_document_sha256.empty() &&
            loaded.vfs->stat("package://config/game-profile.json").success &&
            receipt.value("schema", "") == "noemancer.package-vfs-bootstrap-receipt/0.1" &&
            receipt.value("success", false) && loaded.receipt.json().find(package.root.generic_string()) == std::string::npos,
        "A valid package did not bootstrap entirely through stable VFS identities.") && valid;

    Fixture no_hud;
    valid = check(no_hud.valid && no_hud.write("config/game-profile.json", profile_json("scenes/start.scene.json", "")) &&
            no_hud.write("content/scenes/start.scene.json", scene_json()) &&
            noemancer::bootstrap_package_vfs(no_hud.profile()).receipt.success,
        "An intentionally absent optional HUD was rejected.") && valid;

    Fixture legacy;
    valid = check(legacy.valid && legacy.write("config/game-profile.json",
            R"({"schema":"noemancer.game-profile/0.1","startupScene":"scenes/start.scene.json"})") &&
            legacy.write("content/scenes/start.scene.json", scene_json()) &&
            noemancer::bootstrap_package_vfs(legacy.profile()).display_name == "Noemancer Player",
        "A supported legacy profile did not retain the default display name.") && valid;

    const auto missing = noemancer::bootstrap_package_vfs(package.root / "config" / "missing.json");
    valid = check(!missing && missing.receipt.code == "package.bootstrap.profile-missing",
        "A missing profile trust root was not rejected deterministically.") && valid;

    Fixture wrong_location;
    valid = check(wrong_location.valid && wrong_location.write("other/game-profile.json", profile_json()) &&
            !noemancer::bootstrap_package_vfs(wrong_location.root / "other" / "game-profile.json") &&
            noemancer::bootstrap_package_vfs(wrong_location.root / "other" / "game-profile.json").receipt.code ==
                "package.bootstrap.profile-path-invalid",
        "A profile outside config/game-profile.json was accepted as a package trust root.") && valid;

    Fixture traversal;
    valid = check(traversal.valid && traversal.write("config/game-profile.json", profile_json("../secret.scene.json")) &&
            !noemancer::bootstrap_package_vfs(traversal.profile()) &&
            noemancer::bootstrap_package_vfs(traversal.profile()).receipt.code == "package.bootstrap.profile-invalid",
        "A traversing startupScene was accepted.") && valid;

    Fixture adapter_escape;
    auto adapter_escape_profile = Json::parse(profile_json());
    adapter_escape_profile["managedAssembly"] = "../outside/game.dll";
    valid = check(adapter_escape.valid && adapter_escape.write("config/game-profile.json", adapter_escape_profile.dump()) &&
            !noemancer::bootstrap_package_vfs(adapter_escape.profile()) &&
            noemancer::bootstrap_package_vfs(adapter_escape.profile()).receipt.code == "package.bootstrap.profile-invalid",
        "A traversing package adapter path was accepted.") && valid;

    Fixture bad_schema;
    valid = check(bad_schema.valid && bad_schema.write("config/game-profile.json",
            R"({"schema":"foreign.profile/9","displayName":"Bad","startupScene":"scenes/start.scene.json"})") &&
            !noemancer::bootstrap_package_vfs(bad_schema.profile()) &&
            noemancer::bootstrap_package_vfs(bad_schema.profile()).receipt.code == "package.bootstrap.profile-invalid",
        "An unsupported profile schema was accepted.") && valid;

    Fixture missing_scene;
    valid = check(missing_scene.valid && missing_scene.write("config/game-profile.json", profile_json()) &&
            !noemancer::bootstrap_package_vfs(missing_scene.profile()) &&
            noemancer::bootstrap_package_vfs(missing_scene.profile()).receipt.code ==
                "package.bootstrap.startup-scene-missing",
        "A missing startup Scene was not reported through the VFS contract.") && valid;

    Fixture malformed_scene;
    valid = check(malformed_scene.valid && malformed_scene.write("config/game-profile.json", profile_json()) &&
            malformed_scene.write("content/scenes/start.scene.json", "not-json") &&
            !noemancer::bootstrap_package_vfs(malformed_scene.profile()) &&
            noemancer::bootstrap_package_vfs(malformed_scene.profile()).receipt.code ==
                "package.bootstrap.startup-scene-invalid",
        "A malformed startup Scene was accepted.") && valid;

    Fixture malformed_hud;
    valid = check(malformed_hud.valid && malformed_hud.write("config/game-profile.json", profile_json()) &&
            malformed_hud.write("content/scenes/start.scene.json", scene_json()) &&
            malformed_hud.write("content/ui/hud.ui.json",
                R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"bad","nodes":[{"id":"dup","role":"panel"},{"id":"dup","role":"panel"}]})") &&
            !noemancer::bootstrap_package_vfs(malformed_hud.profile()) &&
            noemancer::bootstrap_package_vfs(malformed_hud.profile()).receipt.code ==
                "package.bootstrap.hud-document-invalid",
        "An invalid Semantic UI HUD was accepted.") && valid;

    auto tight_profile = noemancer::PackageVfsBootstrapLimits{};
    tight_profile.profile_byte_budget = 8U;
    const auto profile_budget = noemancer::bootstrap_package_vfs(package.profile(), tight_profile);
    valid = check(!profile_budget && profile_budget.receipt.code == "package.bootstrap.profile-budget-exceeded",
        "The profile byte budget was not enforced before parsing.") && valid;

    auto tight_scene = noemancer::PackageVfsBootstrapLimits{};
    tight_scene.scene_byte_budget = 8U;
    const auto scene_budget = noemancer::bootstrap_package_vfs(package.profile(), tight_scene);
    valid = check(!scene_budget && scene_budget.receipt.code == "package.bootstrap.startup-scene-budget-exceeded",
        "The startup Scene byte budget was not enforced before parsing.") && valid;

    auto tight_hud = noemancer::PackageVfsBootstrapLimits{};
    tight_hud.hud_byte_budget = 8U;
    const auto hud_budget = noemancer::bootstrap_package_vfs(package.profile(), tight_hud);
    valid = check(!hud_budget && hud_budget.receipt.code == "package.bootstrap.hud-document-budget-exceeded",
        "The HUD byte budget was not enforced before parsing.") && valid;

    return valid ? 0 : 1;
}
