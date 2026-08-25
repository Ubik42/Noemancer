#include "engine/package_pipeline.hpp"
#include "engine/hybrid_pixel_profile.hpp"
#include "engine/process_diagnostics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

const noemancer::PackageStageEntry* find_entry(
    const noemancer::PackagePlan& plan, const std::string& id) {
    for (const auto& entry : plan.entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

bool has_diagnostic(const noemancer::PackagePlan& plan, const std::string& code) {
    for (const auto& diagnostic : plan.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

const noemancer::PackageLicenseLedgerEntry* find_license_ledger(
    const noemancer::PackagePlan& plan, const std::string& id) {
    for (const auto& license : plan.license_ledger) {
        if (license.descriptor.id == id) return &license;
    }
    return nullptr;
}

noemancer::PackageInput make_input() {
    using namespace noemancer;
    PackageInput input;
    // Keep this base fixture on project/0.1 to prove legacy package-input
    // compatibility; hybrid_input below exercises the current project/0.2
    // profile path and both paths must emit Game Profile 0.4.
    input.project.schema = "noemancer.project/0.1";
    input.project.project_id = "package.fixture";
    input.project.name = "Package Fixture";
    input.project.root = std::filesystem::path("D:/fixture/package-game");
    input.project.startup_scene = std::filesystem::path("scenes/start.scene.json");
    input.project.asset_roots = {std::filesystem::path("assets")};
    input.project.packaged_assets = {"sprite.hero"};
    input.project.script_project = std::filesystem::path("scripts/Game.csproj");
    input.project.hud_document = std::filesystem::path("ui/gameplay.ui.json");
    input.project.hud_document_json = R"({"schemaVersion":"noemancer.ui-document/0.1","documentId":"fixture.hud","nodes":[]})";
    input.project.input_actions = default_input_action_definitions();
    input.startup_scene.schema = "noemancer.scene/0.1";
    input.startup_scene.scene_guid = "scene.package.fixture";
    input.startup_scene.name = "Start";

    input.target_profile = cook_platform_profile("windows-x64-release");
    input.cook_manifest.schema = "noemancer.cook-manifest/0.1";
    input.cook_manifest.content_hash = "sha256:cook-fixture";
    input.cook_manifest.target_profile = input.target_profile.id;
    input.cook_manifest.outputs = {
        PackageCookArtifact{
            .asset_id = "sprite.hero",
            .display_name = "Hero Sprite",
            .kind = "Sprite",
            .payload_uri = "generated://cook-cache/hero/payload.meshbin",
            .payload_format = "noemancer/meshbin/0.2",
            .source_path = std::filesystem::path("generated/hero.meshbin"),
            .content_hash = "sha256:hero",
            .bytes = 120U,
            .license_id = "project-original",
            .redistribution = "project-only",
            .streaming_mode = "stream",
            .streaming_importance = "critical",
            .streaming_priority = 900U,
            .available = true,
            .required = true,
            .dependencies = {"shared.material"}
        },
        PackageCookArtifact{
            .asset_id = "shared.material",
            .payload_uri = "generated://cook-cache/material/payload.ktx2",
            .payload_format = "ktx2",
            .source_path = std::filesystem::path("generated/material.ktx2"),
            .content_hash = "sha256:material",
            .bytes = 64U,
            .license_id = "third-party-texture",
            .available = true,
            .required = false
        }
    };
    input.game_profile = PackageGameProfile{
        .id = "windows-x64-release",
        .display_name = "Package Fixture Release",
        .platform = "windows",
        .architecture = "x64",
        .configuration = "release",
        .executable_name = "PackageFixture.exe"
    };
    input.runtime.executable = PackageFileDescriptor{
        .id = "runtime.executable",
        .source_path = std::filesystem::path("build/PackageFixture.exe"),
        .content_hash = "sha256:runtime",
        .bytes = 512U,
        .license_id = "noemancer-runtime"
    };
    input.runtime.requirements={{.id="microsoft-dotnet-runtime",.display_name="Microsoft.NETCore.App Runtime",
        .version="10.x",.architecture="x64",.bundled=false}};
    input.script.assembly = PackageFileDescriptor{
        .id = "project.script",
        .source_path = std::filesystem::path("build/Game.dll"),
        .content_hash = "sha256:assembly",
        .bytes = 256U,
        .license_id = "project-original"
    };
    input.startup_scene_file = PackageFileDescriptor{
        .id = "startup.scene",
        .source_path = input.project.root / input.project.startup_scene,
        .content_hash = "sha256:scene",
        .bytes = 180U,
        .license_id = "project-original"
    };
    input.hud_document_file = PackageFileDescriptor{
        .id = "project.hud",
        .source_path = input.project.root / *input.project.hud_document,
        .content_hash = "sha256:hud",
        .bytes = 96U,
        .license_id = "project-original"
    };
    input.licenses = {
        PackageLicenseDescriptor{
            .id = "project-original",
            .name = "Fixture Project",
            .spdx_id = "LicenseRef-Fixture",
            .notice = "Project-owned fixture content.",
            .third_party = false,
            .redistributable = true
        },
        PackageLicenseDescriptor{
            .id = "third-party-texture",
            .name = "Fixture Texture Library",
            .spdx_id = "CC0-1.0",
            .notice = "Texture library notice.",
            .source_uri = "https://example.invalid/fixture-texture",
            .third_party = true,
            .redistributable = true
        },
        PackageLicenseDescriptor{
            .id = "noemancer-runtime",
            .name = "Noemancer Runtime",
            .spdx_id = "LicenseRef-Noemancer",
            .notice = "Runtime notice.",
            .third_party = false,
            .redistributable = true
        }
    };
    input.staging_root = std::filesystem::path("D:/fixture/package-out");
    input.dry_run = true;
    return input;
}

} // namespace

int main() {
    noemancer::configure_process_diagnostics("test.package-pipeline");
    try {
    using namespace noemancer;

    SceneDocument graph_scene;
    graph_scene.schema = "noemancer.scene/0.1";
    graph_scene.scene_guid = "scene.graph-closure";
    SceneEntityDocument graph_player;
    graph_player.guid = "entity.graph-player";
    graph_player.animation_player = SceneAnimationPlayer{};
    graph_player.animation_player->animation_graph_asset = "graph.player";
    graph_scene.entities.push_back(std::move(graph_player));
    const auto graph_scene_roots = collect_scene_asset_ids(graph_scene);

    auto graph_input = make_input();
    graph_input.project.packaged_assets.clear();
    graph_input.startup_asset_ids.assign(graph_scene_roots.begin(), graph_scene_roots.end());
    graph_input.cook_manifest.outputs = {
        PackageCookArtifact{
            .asset_id = "graph.player",
            .display_name = "Player Graph",
            .kind = "AnimationGraph",
            .payload_uri = "generated://cook-cache/graph/payload.json",
            .payload_format = "json",
            .source_path = std::filesystem::path("generated/graph.json"),
            .content_hash = "sha256:graph",
            .bytes = 96U,
            .license_id = "project-original",
            .redistribution = "project-only",
            .available = true,
            .required = false,
            .dependencies = {"machine.player"}
        },
        PackageCookArtifact{
            .asset_id = "machine.player",
            .display_name = "Player State Machine",
            .kind = "AnimationStateMachine",
            .payload_uri = "generated://cook-cache/machine/payload.json",
            .payload_format = "json",
            .source_path = std::filesystem::path("generated/machine.json"),
            .content_hash = "sha256:machine",
            .bytes = 80U,
            .license_id = "project-original",
            .redistribution = "project-only",
            .available = true,
            .required = false,
            .dependencies = {"clip.idle"}
        },
        PackageCookArtifact{
            .asset_id = "clip.idle",
            .display_name = "Idle Clip",
            .kind = "AnimationClip",
            .payload_uri = "generated://cook-cache/clip/payload.animbin",
            .payload_format = "noemancer/animbin",
            .source_path = std::filesystem::path("generated/clip.animbin"),
            .content_hash = "sha256:clip",
            .bytes = 64U,
            .license_id = "project-original",
            .redistribution = "project-only",
            .available = true,
            .required = false
        }
    };
    const auto graph_plan = plan_package(graph_input);
    const auto* cooked_clip_entry=find_entry(graph_plan,"clip.idle");
    const auto raw_animation_source=std::ranges::find_if(graph_plan.entries,[](const PackageStageEntry& entry) {
        const auto path=entry.source_path.generic_string();
        return path.ends_with(".fbx")||path.ends_with(".glb")||path.ends_with(".animation-clip.json");
    });
    if (graph_scene_roots != std::vector<std::string>{"graph.player"} || !graph_plan.valid ||
        graph_plan.asset_closure != std::vector<std::string>{"clip.idle", "graph.player", "machine.player"}||
        cooked_clip_entry==nullptr||cooked_clip_entry->staging_path.generic_string()!="content/assets/clip.idle.animbin"||
        raw_animation_source!=graph_plan.entries.end()||graph_plan.content_registry_json.find("source.rig")!=std::string::npos) {
        std::cerr << "Animation Graph scene roots did not close through StateMachine to Clip: "
                  << package_plan_json(graph_plan) << '\n';
        return 52;
    }

    const auto input = make_input();
    const auto plan = plan_package(input);
    if (!plan.valid || plan.code != "ok" || plan.asset_closure.size() != 2U ||
        plan.entries.size() != 10U || plan.content_hash.empty() || plan.plan_id.empty() ||
        plan.third_party_license_json.empty() || plan.notice_text.empty()) {
        std::cerr << "Valid package closure was rejected: " << package_plan_json(plan) << '\n';
        return 1;
    }

    const auto* startup_entry = find_entry(plan, "startup.scene");
    const auto* runtime_entry = find_entry(plan, "runtime.executable");
    const auto* profile_entry = find_entry(plan, "game.profile");
    const auto* hud_entry = find_entry(plan, "project.hud");
    if (startup_entry == nullptr || startup_entry->role != "startup-scene" ||
        startup_entry->staging_path.generic_string() != "content/scenes/start.scene.json" ||
        runtime_entry == nullptr || runtime_entry->role != "runtime-executable" ||
        runtime_entry->staging_path.generic_string() != "bin/PackageFixture.exe" ||
        profile_entry == nullptr || profile_entry->role != "game-profile" || hud_entry == nullptr ||
        hud_entry->role != "project-ui" || hud_entry->staging_path.generic_string() != "content/ui/gameplay.ui.json") {
        std::cerr << "Standalone Player package entries were not projected as expected.\n";
        return 2;
    }
    const auto profile_json = nlohmann::json::parse(plan.game_profile_json);
    if (profile_json.at("schema") != "noemancer.game-profile/0.4" ||
        profile_json.at("id") != "windows-x64-release" ||
        profile_json.at("platform") != "windows" || profile_json.at("architecture") != "x64" ||
        profile_json.at("configuration") != "release" ||
        profile_json.at("executable") != "PackageFixture.exe" ||
        profile_json.at("projectId") != "package.fixture" ||
        profile_json.at("targetProfile") != "windows-x64-release" ||
        profile_json.at("runtimeRequirements").size()!=1U ||
        profile_json.at("runtimeRequirements").at(0).at("id")!="microsoft-dotnet-runtime" ||
        profile_json.at("startupScene") != "scenes/start.scene.json" ||
        profile_json.at("startupSceneGuid") != "scene.package.fixture" ||
        profile_json.at("managedAssembly") != "managed/Game.dll" ||
        profile_json.at("managedConfiguration") != "Release" || profile_json.at("inputActions").size() != 3U ||
        profile_json.at("hudDocument") != "ui/gameplay.ui.json" ||
        profile_json.at("assetRegistry") != "content/assets/registry.json" ||
        profile_json.at("packagedAssets") != nlohmann::json::array({"sprite.hero"}) ||
        profile_json.at("inputActions").at(0).at("id") != "gameplay.move.x" ||
        profile_json.at("inputActions").at(0).at("bindings").at(0).at("deadZone") != 0.0F) {
        std::cerr << "Game Profile did not preserve the independent Player launch contract.\n";
        return 3;
    }
    if (profile_json.contains("hybridPixelProfile")) {
        std::cerr << "A project without a Hybrid Pixel profile emitted an optional package field.\n";
        return 33;
    }

    auto hybrid_input = input;
    hybrid_input.project.schema = "noemancer.project/0.2";
    hybrid_input.project.hybrid_pixel_profile = HybridPixelProfile{
        .schema = std::string(hybrid_pixel_profile_schema),
        .profile_id = "lumen-run-hybrid",
        .enabled = true,
        .virtual_width = 320U,
        .virtual_height = 180U,
        .pixels_per_unit = 24.0F,
        .integer_scaling = true,
        .snap_camera = true,
        .snap_sprites = false,
        .presentation_filter = "nearest"
    };
    const auto hybrid_plan = plan_package(hybrid_input);
    if (!hybrid_plan.valid || hybrid_plan.game_profile_json == plan.game_profile_json) {
        std::cerr << "A valid Hybrid Pixel profile did not produce a distinct package profile.\n";
        return 34;
    }
    const auto hybrid_profile_json = nlohmann::json::parse(hybrid_plan.game_profile_json);
    const auto expected_hybrid_profile = nlohmann::json::parse(
        HybridPixelProfileCodec::write_canonical_json(
            *hybrid_input.project.hybrid_pixel_profile));
    if (hybrid_profile_json.at("schema") != "noemancer.game-profile/0.4" ||
        !hybrid_profile_json.contains("hybridPixelProfile") ||
        !hybrid_profile_json.at("hybridPixelProfile").is_object() ||
        hybrid_profile_json.at("hybridPixelProfile") != expected_hybrid_profile ||
        hybrid_profile_json.at("hybridPixelProfile").at("profileId") != "lumen-run-hybrid") {
        std::cerr << "Hybrid Pixel profile was not embedded as the canonical JSON object.\n";
        return 35;
    }
    const auto hybrid_repeat = plan_package(hybrid_input);
    if (hybrid_plan.game_profile_json != hybrid_repeat.game_profile_json ||
        hybrid_plan.content_hash != hybrid_repeat.content_hash ||
        hybrid_plan.plan_id != hybrid_repeat.plan_id ||
        package_plan_json(hybrid_plan) != package_plan_json(hybrid_repeat)) {
        std::cerr << "Hybrid Pixel package profile serialization was not deterministic.\n";
        return 36;
    }
    auto legacy_hybrid_input = hybrid_input;
    legacy_hybrid_input.project.schema = "noemancer.project/0.1";
    const auto legacy_hybrid_plan = plan_package(legacy_hybrid_input);
    if (legacy_hybrid_plan.valid ||
        !has_diagnostic(legacy_hybrid_plan,
            "package.hybrid-pixel-profile-schema")) {
        std::cerr << "A legacy Project schema bypassed the Hybrid Pixel package boundary.\n";
        return 37;
    }
    auto sky_input=input;
    sky_input.project.schema="noemancer.project/0.2";
    sky_input.project.sky_atmosphere=make_sky_atmosphere_settings(SkyAtmosphereQuality::medium);
    sky_input.project.sky_environment=make_sky_environment_settings(SkyAerosolPreset::hazy);
    sky_input.project.sky_environment->solar.time_of_day_hours=17.5F;
    const auto sky_plan=plan_package(sky_input);
    const auto sky_profile=nlohmann::json::parse(sky_plan.game_profile_json);
    if(!sky_plan.valid||!sky_profile.contains("skyAtmosphere")||
       !sky_profile.contains("skyEnvironment")||
       sky_profile.at("skyAtmosphere")!=nlohmann::json::parse(
           SkyAtmosphereSettingsCodec::write_canonical_json(*sky_input.project.sky_atmosphere))||
       sky_profile.at("skyEnvironment")!=nlohmann::json::parse(
           SkyEnvironmentCodec::write_canonical_json(*sky_input.project.sky_environment))) {
        std::cerr<<"Sky Atmosphere/Environment were not embedded as canonical Player contracts.\n";
        return 38;
    }
    auto legacy_sky_input=sky_input;legacy_sky_input.project.schema="noemancer.project/0.1";
    const auto legacy_sky_plan=plan_package(legacy_sky_input);
    if(legacy_sky_plan.valid||!has_diagnostic(legacy_sky_plan,"package.sky-atmosphere-schema")||
       !has_diagnostic(legacy_sky_plan,"package.sky-environment-schema")) {
        std::cerr<<"Legacy Project schema bypassed the Sky package boundary.\n";return 39;
    }
    const auto repeat = plan_package(input);
    if (package_plan_json(plan) != package_plan_json(repeat) ||
        plan.content_hash != repeat.content_hash || plan.plan_id != repeat.plan_id) {
        std::cerr << "Package plan was not deterministic.\n";
        return 4;
    }
    auto changed_profile = input;
    changed_profile.game_profile.display_name = "Package Fixture QA";
    const auto changed_profile_plan = plan_package(changed_profile);
    if (!changed_profile_plan.valid || changed_profile_plan.content_hash == plan.content_hash ||
        changed_profile_plan.plan_id == plan.plan_id) {
        std::cerr << "Game Profile changes did not invalidate the package identity.\n";
        return 5;
    }
    auto changed_input = input;
    changed_input.project.input_actions.front().bindings.front().source = "keyboard.left";
    const auto changed_input_plan = plan_package(changed_input);
    if (!changed_input_plan.valid || changed_input_plan.content_hash == plan.content_hash ||
        changed_input_plan.plan_id == plan.plan_id) {
        std::cerr << "Input mapping changes did not invalidate the package identity.\n";
        return 51;
    }
    const auto json = nlohmann::json::parse(package_plan_json(plan));
    const auto packaged_registry=nlohmann::json::parse(
        json.at("generated").at("assetRegistry").get<std::string>());
    if (json.at("schema") != "noemancer.package-plan/0.1" ||
        json.at("generated").at("thirdPartyLicenses").get<std::string>().find("third-party-texture") == std::string::npos ||
        json.at("entries").size() != 10U || json.at("entries").at(0).at("role").get<std::string>().empty() ||
        packaged_registry.at("assets").size()!=2U||
        packaged_registry.at("assets").at(1).at("path") != "sprite.hero.meshbin" ||
        packaged_registry.at("assets").at(1).at("streamingPolicy")!=nlohmann::json{
            {"mode","stream"},{"importance","critical"},{"priority",900U}}) {
        std::cerr << "Package plan JSON did not expose stable closure metadata.\n";
        return 6;
    }
    const auto* project_ledger = find_license_ledger(plan, "project-original");
    const auto* texture_ledger = find_license_ledger(plan, "third-party-texture");
    if (project_ledger == nullptr || project_ledger->scope != "project-owned" ||
        project_ledger->identifier_kind != "custom" || project_ledger->entry_references.size() != 4U ||
        texture_ledger == nullptr || texture_ledger->scope != "third-party" ||
        texture_ledger->identifier_kind != "spdx" || texture_ledger->entry_references !=
            std::vector<std::string>{"shared.material"} || !texture_ledger->required_roots.empty() ||
        json.at("licenseLedger").size() != 3U ||
        json.at("licenseLedger").at(1).at("entryReferences").is_null()) {
        std::cerr << "Package license ledger did not expose ownership and reference provenance.\n";
        return 29;
    }

    const auto expect_source_geometry_rejected = [](PackageInput candidate,
                                                     const std::string& scenario) {
        const auto rejected = plan_package(candidate);
        if (rejected.valid || rejected.code != "package.source-geometry-forbidden" ||
            !has_diagnostic(rejected, "package.source-geometry-forbidden")) {
            std::cerr << "Package accepted forbidden source geometry (" << scenario << "): "
                      << package_plan_json(rejected) << '\n';
            return false;
        }
        return true;
    };

    int fail_closed_case = 0;

    auto legacy_mesh_format = input;
    legacy_mesh_format.cook_manifest.outputs.front().payload_format = "meshopt/meshbin";
    if (!expect_source_geometry_rejected(std::move(legacy_mesh_format), "legacy meshopt/meshbin"))
        fail_closed_case = 23;

    auto raw_glb_source = input;
    raw_glb_source.cook_manifest.outputs.front().source_path = std::filesystem::path("generated/hero.glb");
    if (!expect_source_geometry_rejected(std::move(raw_glb_source), "raw .glb source path") && fail_closed_case == 0)
        fail_closed_case = 24;

    auto raw_fbx_source = input;
    raw_fbx_source.cook_manifest.outputs.front().source_path = std::filesystem::path("generated/hero.fbx");
    if (!expect_source_geometry_rejected(std::move(raw_fbx_source), "raw .fbx source path") && fail_closed_case == 0)
        fail_closed_case = 25;

    auto raw_glb_payload = input;
    raw_glb_payload.cook_manifest.outputs.front().payload_format = ".glb";
    if (!expect_source_geometry_rejected(std::move(raw_glb_payload), "raw .glb payload format") && fail_closed_case == 0)
        fail_closed_case = 26;

    auto raw_fbx_payload = input;
    raw_fbx_payload.cook_manifest.outputs.front().payload_format = ".fbx";
    if (!expect_source_geometry_rejected(std::move(raw_fbx_payload), "raw .fbx payload format") && fail_closed_case == 0)
        fail_closed_case = 27;

    auto missing_payload_hash = input;
    missing_payload_hash.cook_manifest.outputs.front().content_hash.clear();
    const auto missing_payload_hash_plan = plan_package(missing_payload_hash,
        [](const std::filesystem::path& source_path) -> std::optional<PackageFileObservation> {
            const auto path = source_path.generic_string();
            if (path.ends_with("hero.meshbin")) return PackageFileObservation{120U, "sha256:hero", true};
            if (path.ends_with("material.ktx2")) return PackageFileObservation{64U, "sha256:material", true};
            if (path.ends_with("PackageFixture.exe")) return PackageFileObservation{512U, "sha256:runtime", true};
            if (path.ends_with("Game.dll")) return PackageFileObservation{256U, "sha256:assembly", true};
            if (path.ends_with("start.scene.json")) return PackageFileObservation{180U, "sha256:scene", true};
            if (path.ends_with("gameplay.ui.json")) return PackageFileObservation{96U, "sha256:hud", true};
            return PackageFileObservation{0U, {}, true};
        });
    const auto missing_payload_hash_diagnostic =
        has_diagnostic(missing_payload_hash_plan, "package.payload-hash-missing") ||
        has_diagnostic(missing_payload_hash_plan, "package.content-hash-missing") ||
        has_diagnostic(missing_payload_hash_plan, "package.artifact-hash-missing");
    if (missing_payload_hash_plan.valid || !missing_payload_hash_diagnostic) {
        std::cerr << "Package accepted a Cook output with no declared payload hash: "
                  << package_plan_json(missing_payload_hash_plan) << '\n';
        if (fail_closed_case == 0) fail_closed_case = 28;
    }
    if (fail_closed_case != 0) return fail_closed_case;

    const auto dry_receipt = commit_package(plan);
    if (!dry_receipt.success || !dry_receipt.dry_run || dry_receipt.committed ||
        dry_receipt.code != "package.dry-run") {
        std::cerr << "Dry-run package receipt was incorrect: " << package_receipt_json(dry_receipt) << '\n';
        return 7;
    }
    const auto dry_receipt_json = nlohmann::json::parse(package_receipt_json(dry_receipt));
    if (dry_receipt_json.at("schema") != "noemancer.package-receipt/0.1" ||
        dry_receipt_json.at("planId") != plan.plan_id ||
        dry_receipt_json.at("contentHash") != plan.content_hash ||
        dry_receipt_json.at("entries").size() != plan.entries.size() ||
        dry_receipt_json.at("committed") != false || dry_receipt_json.at("atomic") != false) {
        std::cerr << "Dry-run receipt did not expose the complete Player package identity.\n";
        return 8;
    }

    auto commit_input = input;
    commit_input.dry_run = false;
    const auto commit_plan = plan_package(commit_input);
    if (commit_plan.plan_id != plan.plan_id || commit_plan.content_hash != plan.content_hash) {
        std::cerr << "Dry-run and commit package plans changed package identity.\n";
        return 9;
    }
    bool callback_called = false;
    const auto receipt = commit_package(commit_plan, [&](const PackageCommitRequest& request) {
        callback_called = true;
        if (request.plan_id != commit_plan.plan_id || request.content_hash != commit_plan.content_hash ||
            request.entries.size() != commit_plan.entries.size()) {
            return PackageCommitResult{false, true, "package.test-request-invalid", "Request mismatch", {}};
        }
        return PackageCommitResult{true, true, "ok", "Committed by test atomic callback", "commit.fixture.1"};
    });
    if (!callback_called || !receipt.success || !receipt.committed || !receipt.atomic ||
        receipt.code != "ok" || receipt.commit_id != "commit.fixture.1") {
        std::cerr << "Atomic package commit receipt was incorrect: " << package_receipt_json(receipt) << '\n';
        return 10;
    }
    const auto receipt_json = nlohmann::json::parse(package_receipt_json(receipt));
    if (receipt_json.at("success") != true || receipt_json.at("committed") != true ||
        receipt_json.at("atomic") != true || receipt_json.at("commitId") != "commit.fixture.1" ||
        receipt_json.at("entries").size() != commit_plan.entries.size()) {
        std::cerr << "Atomic receipt did not preserve the committed Player package evidence.\n";
        return 11;
    }

    const auto non_atomic_receipt = commit_package(commit_plan, [](const PackageCommitRequest&) {
        return PackageCommitResult{true, false, "ok", "Test callback did not prove atomicity.", "not-atomic"};
    });
    if (non_atomic_receipt.success || non_atomic_receipt.committed || non_atomic_receipt.atomic ||
        non_atomic_receipt.code != "package.commit-not-atomic") {
        std::cerr << "A non-atomic commit callback was accepted.\n";
        return 12;
    }

    auto escaped = input;
    escaped.runtime.executable.staging_path = std::filesystem::path("../escape/PackageFixture.exe");
    const auto escaped_plan = plan_package(escaped);
    if (escaped_plan.valid || escaped_plan.code != "package.path-escape") {
        std::cerr << "Staging path escape was accepted.\n";
        return 13;
    }

    auto missing_license = input;
    missing_license.cook_manifest.outputs.front().license_id.clear();
    const auto missing_license_plan = plan_package(missing_license);
    if (missing_license_plan.valid || missing_license_plan.code != "package.license-missing") {
        std::cerr << "Missing package license was accepted.\n";
        return 14;
    }

    auto missing_dependency = input;
    missing_dependency.cook_manifest.outputs.front().dependencies = {"asset.missing"};
    const auto missing_dependency_plan = plan_package(missing_dependency);
    if (missing_dependency_plan.valid || missing_dependency_plan.code != "package.artifact-dependency-missing") {
        std::cerr << "Missing Cook dependency was accepted.\n";
        return 15;
    }

    auto invalid_profile = input;
    invalid_profile.game_profile.platform = "linux";
    const auto invalid_profile_plan = plan_package(invalid_profile);
    if (invalid_profile_plan.valid || invalid_profile_plan.code != "package.game-profile-invalid" ||
        !has_diagnostic(invalid_profile_plan, "package.game-profile-invalid")) {
        std::cerr << "An unsupported Game Profile was accepted.\n";
        return 16;
    }

    auto invalid_startup = input;
    invalid_startup.project.startup_scene = std::filesystem::path("../outside.scene.json");
    const auto invalid_startup_plan = plan_package(invalid_startup);
    if (invalid_startup_plan.valid || invalid_startup_plan.code != "package.startup-scene-invalid") {
        std::cerr << "A startup scene outside the project root was accepted.\n";
        return 17;
    }

    auto missing_runtime = input;
    missing_runtime.runtime.executable.source_path.clear();
    missing_runtime.runtime.executable.content_hash.clear();
    const auto missing_runtime_plan = plan_package(missing_runtime);
    if (missing_runtime_plan.valid || missing_runtime_plan.code != "package.runtime-executable-missing") {
        std::cerr << "A package without a standalone Player executable was accepted.\n";
        return 18;
    }

    auto missing_probe_file = input;
    const auto missing_probe_plan = plan_package(missing_probe_file,
        [](const std::filesystem::path&) -> std::optional<PackageFileObservation> { return std::nullopt; });
    if (missing_probe_plan.valid || missing_probe_plan.code != "package.file-missing") {
        std::cerr << "Editor preflight did not reject an unavailable package source.\n";
        return 19;
    }

    auto collision = input;
    collision.runtime.executable.staging_path = std::filesystem::path("content/scenes/start.scene.json");
    const auto collision_plan = plan_package(collision);
    if (collision_plan.valid || collision_plan.code != "package.path-collision") {
        std::cerr << "A package staging path collision was accepted.\n";
        return 20;
    }

    auto provenance_license = input;
    provenance_license.licenses.push_back(PackageLicenseDescriptor{
        .id = "animation-source-license",
        .name = "Animation Source Library",
        .spdx_id = "CC-BY-4.0",
        .notice = "Animation source attribution.",
        .source_uri = "https://example.invalid/animation-source",
        .third_party = true,
        .redistributable = true
    });
    provenance_license.required_license_ids = {"animation-source-license"};
    const auto provenance_plan = plan_package(provenance_license);
    if (!provenance_plan.valid ||
        std::ranges::find(provenance_plan.licenses, "animation-source-license",
            &PackageLicenseDescriptor::id) == provenance_plan.licenses.end() ||
        provenance_plan.notice_text.find("Animation source attribution.") == std::string::npos ||
        find_license_ledger(provenance_plan, "animation-source-license") == nullptr ||
        find_license_ledger(provenance_plan, "animation-source-license")->required_roots !=
            std::vector<std::string>{"animation-source-license"}) {
        std::cerr << "A Cook-only source license was dropped from the Package notice closure.\n";
        return 21;
    }

    auto duplicate_required_root = provenance_license;
    duplicate_required_root.required_license_ids = {"animation-source-license", "animation-source-license"};
    const auto duplicate_root_plan = plan_package(duplicate_required_root);
    if (duplicate_root_plan.valid ||
        !has_diagnostic(duplicate_root_plan, "package.license-required-root-duplicate")) {
        std::cerr << "Duplicate required license roots were accepted.\n";
        return 30;
    }

    auto malformed_license = input;
    malformed_license.licenses.front().spdx_id = "MIT OR Apache-2.0";
    const auto malformed_license_plan = plan_package(malformed_license);
    if (malformed_license_plan.valid ||
        !has_diagnostic(malformed_license_plan, "package.license-spdx-invalid")) {
        std::cerr << "A malformed SPDX/custom license identifier was accepted.\n";
        return 31;
    }

    auto incomplete_third_party = input;
    incomplete_third_party.licenses[1].source_uri.clear();
    incomplete_third_party.licenses[1].notice.clear();
    const auto incomplete_third_party_plan = plan_package(incomplete_third_party);
    if (incomplete_third_party_plan.valid ||
        !has_diagnostic(incomplete_third_party_plan, "package.license-source-uri-missing") ||
        !has_diagnostic(incomplete_third_party_plan, "package.notice-missing")) {
        std::cerr << "An incomplete third-party license record was accepted.\n";
        return 32;
    }

    const auto identity_mismatch_plan = plan_package(input,
        [](const std::filesystem::path&) -> std::optional<PackageFileObservation> {
            return PackageFileObservation{999U, "sha256:mutated", true};
        });
    if (identity_mismatch_plan.valid ||
        !has_diagnostic(identity_mismatch_plan, "package.file-identity-mismatch")) {
        std::cerr << "Package preflight accepted files that no longer match the planned identity.\n";
        return 22;
    }

    std::cout << "package_pipeline_tests: ok\n";
    return 0;
    } catch(const std::exception& exception) {
        std::cerr<<"package_pipeline_tests uncaught exception: "<<exception.what()<<'\n';
        return 99;
    } catch(...) {
        std::cerr<<"package_pipeline_tests uncaught non-standard exception\n";
        return 100;
    }
}
