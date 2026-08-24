#include "engine/hybrid_pixel_profile.hpp"
#include "engine/package_pipeline.hpp"
#include "engine/project_document.hpp"
#include "engine/sprite_asset.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;
using noemancer::PackageCookArtifact;
using noemancer::PackageFileDescriptor;
using noemancer::PackagePlan;

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct TemporaryProject final {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "noemancer-hybrid-pixel-production-package";

    TemporaryProject() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "assets" / "sprites");
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "ui");
        std::filesystem::create_directories(root / "scripts");
        std::filesystem::create_directories(root / "build");
    }

    ~TemporaryProject() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write(const std::filesystem::path& relative, const std::string_view text) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "temporary project file could not be opened");
        output << text;
        require(static_cast<bool>(output), "temporary project file could not be written");
    }
};

const noemancer::PackageStageEntry* find_entry(const PackagePlan& plan, const std::string_view id) {
    const auto found = std::ranges::find(plan.entries, id, &noemancer::PackageStageEntry::id);
    return found == plan.entries.end() ? nullptr : &*found;
}

bool has_diagnostic(const PackagePlan& plan, const std::string_view code) {
    return std::ranges::any_of(plan.diagnostics, [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

PackageCookArtifact cooked_artifact(
    const std::string_view id,
    const std::string_view kind,
    const std::string_view format,
    const std::filesystem::path& source,
    const std::string_view hash,
    std::vector<std::string> dependencies = {},
    const bool required = false) {
    PackageCookArtifact artifact;
    artifact.asset_id = id;
    artifact.display_name = std::string(id);
    artifact.kind = kind;
    artifact.payload_uri = "generated://cook-cache/" + std::string(id) + "/payload";
    artifact.payload_format = format;
    artifact.source_path = source;
    artifact.content_hash = hash;
    artifact.bytes = 128U;
    artifact.license_id = "project-original";
    artifact.redistribution = "project-only";
    artifact.streaming_mode = "stream";
    artifact.streaming_importance = "critical";
    artifact.streaming_priority = 800U;
    artifact.available = true;
    artifact.required = required;
    artifact.dependencies = std::move(dependencies);
    return artifact;
}

PackageFileDescriptor file_descriptor(
    const std::string_view id,
    const std::filesystem::path& source,
    const std::string_view hash,
    const std::string_view license) {
    PackageFileDescriptor descriptor;
    descriptor.id = id;
    descriptor.source_path = source;
    descriptor.content_hash = hash;
    descriptor.bytes = 128U;
    descriptor.license_id = license;
    descriptor.available = true;
    descriptor.required = true;
    return descriptor;
}

Json make_hybrid_profile() {
    return Json{
        {"schema", std::string(noemancer::hybrid_pixel_profile_schema)},
        {"profileId", "lumen-run.hybrid"},
        {"enabled", true},
        {"virtualWidth", 320},
        {"virtualHeight", 180},
        {"pixelsPerUnit", 16.0},
        {"integerScaling", true},
        {"snapCamera", true},
        {"snapSprites", true},
        {"presentationFilter", "nearest"}
    };
}

std::string sprite_source() {
    return Json{
        {"schema", "noemancer.sprite-asset/0.2"},
        {"assetId", "sprite.hero"},
        {"textureAsset", "texture.hero"},
        {"textureSize", Json::array({16, 16})},
        {"pixelsPerUnit", 16.0},
        {"sampling", "nearest"},
        {"alphaMode", "cutout"},
        {"material", {
            {"normalTextureAsset", "texture.hero.normal"},
            {"emissiveMaskTextureAsset", "texture.hero.emissive"},
            {"depthTextureAsset", "texture.hero.depth"},
            {"normalStrength", 0.75},
            {"emissiveColor", Json::array({0.2, 0.5, 1.0})},
            {"emissiveIntensity", 2.0},
            {"depthBias", 0.0005},
            {"shadingModel", "lit"},
            {"metallic", 0.35},
            {"roughness", 0.45},
            {"receivesShadows", true},
            {"castsShadows", true}
        }},
        {"frames", Json::array({Json{
            {"id", "idle.0"},
            {"rect", Json::array({0, 0, 16, 16})},
            {"trimOffset", Json::array({0, 0})},
            {"sourceSize", Json::array({16, 16})},
            {"pivot", Json::array({0.5, 0.0})},
            {"collisionProfile", "hero.body"}
        }})},
        {"clips", Json::array({Json{
            {"id", "idle"},
            {"looping", true},
            {"frames", Json::array({Json{
                {"frame", "idle.0"}, {"durationMs", 120}, {"event", ""}
            }})}
        }})},
        {"provenance", {
            {"sourceUri", "sprites/hero.png"},
            {"sourceSha256", "hybrid-production-fixture"},
            {"generator", "hybrid-production-package-test"},
            {"license", "CC0-1.0"}
        }}
    }.dump();
}

void write_project_fixture(const TemporaryProject& fixture) {
    fixture.write("scenes/start.scene.json", R"({
        "schema":"noemancer.scene/0.1",
        "sceneGuid":"scene.hybrid-production",
        "name":"Hybrid Production Start",
        "entities":[{
            "guid":"entity.hero",
            "name":"Hero",
            "parent":null,
            "components":{
                "Transform":{"position":[0,0,0],"scale":[1,1,1],"rotationEulerDegrees":[0,0,0]},
                "SpriteRenderer":{
                    "spriteAsset":"sprite.hero",
                    "clip":"idle",
                    "playbackSpeed":1.0,
                    "playing":true,
                    "flipX":false,
                    "flipY":false,
                    "sortingLayer":"default",
                    "sortingOrder":0,
                    "visible":true
                }
            }
        }]
    })");
    fixture.write("ui/hud.ui.json", R"({
        "schemaVersion":"noemancer.ui-document/0.1",
        "documentId":"hud.hybrid-production",
        "nodes":[{"id":"hud.root","parentId":null,"role":"hud","label":"Hybrid"}]
    })");
    // This is deliberately a managed-only project.  The package contract is
    // expected to carry its assembly without discovering or staging C++.
    fixture.write("scripts/Game.csproj", R"(<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup></Project>)");
    fixture.write("build/HybridProduction.exe", "managed runtime placeholder\n");
    fixture.write("build/Game.dll", "managed assembly placeholder\n");
    fixture.write("assets/sprites/hero.sprite.json", sprite_source());
    for (const auto* texture : {"hero.png", "hero-normal.png", "hero-emissive.png", "hero-depth.png"})
        fixture.write(std::filesystem::path("assets/sprites") / texture, "texture placeholder\n");

    const Json manifest = {
        {"schema", "noemancer.project/0.2"},
        {"projectId", "hybrid.pixel.production"},
        {"name", "Hybrid Pixel Production Fixture"},
        {"startupScene", "scenes/start.scene.json"},
        {"assetRoots", Json::array({"assets"})},
        {"packagedAssets", Json::array({"sprite.hero", "vfx.billboard.spark"})},
        {"scriptProject", "scripts/Game.csproj"},
        {"hudDocument", "ui/hud.ui.json"},
        {"inputActions", Json::array({
            Json{
                {"id", "gameplay.move.x"}, {"kind", "axis1d"},
                {"bindings", Json::array({Json{{"source", "keyboard.a"}, {"scale", -1.0}, {"deadZone", 0.10}}})}
            },
            Json{
                {"id", "gameplay.jump"}, {"kind", "button"},
                {"bindings", Json::array({Json{{"source", "keyboard.space"}}})}
            }
        })},
        {"hybridPixelProfile", make_hybrid_profile()}
    };
    fixture.write("noemancer.project.json", manifest.dump(2));
}

noemancer::PackageInput make_package_input(
    const TemporaryProject& fixture,
    const noemancer::ProjectLoadResult& loaded,
    const std::vector<std::string>& sprite_dependencies) {
    using namespace noemancer;
    PackageInput input;
    input.project = *loaded.project;
    input.startup_scene = *loaded.startup_scene;
    input.target_profile = cook_platform_profile("windows-x64-release");
    input.cook_manifest.schema = "noemancer.cook-manifest/0.1";
    input.cook_manifest.content_hash = "sha256:hybrid-production-cook";
    input.cook_manifest.target_profile = input.target_profile.id;
    input.cook_manifest.outputs.push_back(cooked_artifact(
        "sprite.hero", "Sprite", "noemancer/spritebin/0.2",
        fixture.root / "generated" / "sprite.hero.spritebin", "sha256:sprite.hero",
        sprite_dependencies));
    input.cook_manifest.outputs.push_back(cooked_artifact(
        "texture.hero", "Texture", "ktx2", fixture.root / "generated" / "texture.hero.ktx2",
        "sha256:texture.hero"));
    input.cook_manifest.outputs.push_back(cooked_artifact(
        "texture.hero.depth", "Texture", "ktx2", fixture.root / "generated" / "texture.hero.depth.ktx2",
        "sha256:texture.hero.depth"));
    input.cook_manifest.outputs.push_back(cooked_artifact(
        "texture.hero.emissive", "Texture", "ktx2", fixture.root / "generated" / "texture.hero.emissive.ktx2",
        "sha256:texture.hero.emissive"));
    input.cook_manifest.outputs.push_back(cooked_artifact(
        "texture.hero.normal", "Texture", "ktx2", fixture.root / "generated" / "texture.hero.normal.ktx2",
        "sha256:texture.hero.normal"));
    input.cook_manifest.outputs.push_back(cooked_artifact(
        "vfx.billboard.spark", "VfxBillboard", "json", fixture.root / "generated" / "vfx.billboard.spark.json",
        "sha256:vfx.billboard.spark", {"vfx.billboard.material"}));
    input.cook_manifest.outputs.push_back(cooked_artifact(
        "vfx.billboard.material", "Material", "json", fixture.root / "generated" / "vfx.billboard.material.json",
        "sha256:vfx.billboard.material"));

    input.game_profile = PackageGameProfile{
        .id = "windows-x64-release",
        .display_name = "Hybrid Pixel Production Release",
        .platform = "windows",
        .architecture = "x64",
        .configuration = "release",
        .executable_name = "HybridProduction.exe"
    };
    input.runtime.executable = file_descriptor(
        "runtime.executable", fixture.root / "build" / "HybridProduction.exe",
        "sha256:runtime", "noemancer-runtime");
    input.runtime.requirements = {{
        .id = "microsoft-dotnet-runtime",
        .display_name = "Microsoft.NETCore.App Runtime",
        .version = "8.x",
        .architecture = "x64",
        .bundled = false
    }};
    input.script.assembly = file_descriptor(
        "project.script", fixture.root / "build" / "Game.dll", "sha256:managed-assembly", "project-original");
    input.startup_scene_file = file_descriptor(
        "startup.scene", fixture.root / input.project.startup_scene, "sha256:startup-scene", "project-original");
    input.hud_document_file = file_descriptor(
        "project.hud", fixture.root / *input.project.hud_document, "sha256:hud-document", "project-original");

    input.startup_asset_ids = collect_scene_asset_ids(input.startup_scene);
    // SceneDocument 0.1 has no VFX renderer component.  A dynamic billboard
    // is therefore an explicit project/package root, while the sprite root
    // above comes from the parsed startup scene.  Build that explicit root
    // from the ProjectDocument rather than repeating a second asset list.
    for (const auto& packaged_asset : input.project.packaged_assets) {
        if (std::ranges::find(input.startup_asset_ids, packaged_asset) == input.startup_asset_ids.end())
            input.startup_asset_ids.push_back(packaged_asset);
    }
    input.licenses = {
        PackageLicenseDescriptor{
            .id = "project-original",
            .name = "Hybrid Production Fixture",
            .spdx_id = "LicenseRef-HybridProduction",
            .notice = "Project-owned production fixture content.",
            .third_party = false,
            .redistributable = true
        },
        PackageLicenseDescriptor{
            .id = "noemancer-runtime",
            .name = "Noemancer Runtime",
            .spdx_id = "LicenseRef-NoemancerRuntime",
            .notice = "Noemancer runtime notice.",
            .third_party = false,
            .redistributable = true
        }
    };
    input.staging_root = fixture.root / "generated" / "package";
    input.dry_run = true;
    return input;
}

void assert_authoritative_profile_and_inputs(
    const noemancer::PackageInput& input,
    const PackagePlan& plan) {
    require(plan.valid && plan.code == "ok", "hybrid production package plan was rejected");
    const auto profile = Json::parse(plan.game_profile_json);
    require(profile.at("schema") == "noemancer.game-profile/0.4", "Game Profile schema drifted");
    require(profile.at("projectId") == "hybrid.pixel.production", "projectId was lost from Game Profile");
    require(profile.at("startupScene") == "scenes/start.scene.json", "startup scene drifted");
    require(profile.at("startupSceneGuid") == "scene.hybrid-production", "startup scene GUID drifted");
    require(profile.at("managedAssembly") == "managed/Game.dll", "managed script assembly drifted");
    require(profile.at("hudDocument") == "ui/hud.ui.json", "HUD document drifted");
    require(profile.at("packagedAssets") == Json::array({"sprite.hero", "vfx.billboard.spark"}),
            "packaged asset roots drifted");
    require(profile.at("inputActions").size() == input.project.input_actions.size(),
            "input action count was not preserved");
    require(profile.at("inputActions").at(0).at("id") == "gameplay.move.x" &&
                profile.at("inputActions").at(0).at("bindings").at(0).at("source") == "keyboard.a" &&
                profile.at("inputActions").at(1).at("id") == "gameplay.jump",
            "input action values drifted");
    require(profile.at("hybridPixelProfile") ==
                Json::parse(noemancer::HybridPixelProfileCodec::write_canonical_json(
                    *input.project.hybrid_pixel_profile)),
            "Hybrid Pixel profile was not emitted canonically");
    require(!profile.contains("nativeCpp") && !profile.contains("nativeProject"),
            "Game Profile invented a native C++ dependency");

    const auto* script = find_entry(plan, "project.script");
    const auto* hud = find_entry(plan, "project.hud");
    require(script != nullptr && script->role == "script-assembly", "scriptProject was not packaged");
    require(hud != nullptr && hud->role == "project-ui", "hudDocument was not packaged");
    require(std::ranges::none_of(plan.entries, [](const auto& entry) {
        const auto path = entry.source_path.generic_string();
        return path.ends_with(".cpp") || path.ends_with(".vcxproj") || path.find("native") != std::string::npos;
    }), "a Native C++ source entered the managed-only package");
}

void assert_asset_closure(const PackagePlan& plan, const std::vector<std::string>& sprite_dependencies) {
    const std::vector<std::string> expected{
        "sprite.hero", "texture.hero", "texture.hero.depth", "texture.hero.emissive",
        "texture.hero.normal", "vfx.billboard.material", "vfx.billboard.spark"
    };
    require(plan.asset_closure == expected, "Sprite/VFX asset closure lost or reordered an authoritative root");
    const auto* sprite = find_entry(plan, "sprite.hero");
    const auto* vfx = find_entry(plan, "vfx.billboard.spark");
    require(sprite != nullptr && sprite->role == "cook-artifact" && sprite->dependencies == sprite_dependencies,
            "Sprite 0.2 material dependencies were not retained");
    require(vfx != nullptr && vfx->dependencies == std::vector<std::string>{"vfx.billboard.material"},
            "VFX root dependency closure was not retained");

    const auto registry = Json::parse(plan.content_registry_json);
    require(registry.at("schema") == "noemancer.assets/0.1" && registry.at("assets").size() == expected.size(),
            "asset registry does not match package closure");
    const auto sprite_projection = std::ranges::find_if(registry.at("assets"), [](const auto& asset) {
        return asset.at("id") == "sprite.hero";
    });
    require(sprite_projection != registry.at("assets").end() &&
                sprite_projection->at("dependencies") == sprite_dependencies,
            "asset registry dropped Sprite normal/emissive/depth dependencies");
}

} // namespace

int main() {
    try {
        TemporaryProject fixture;
        write_project_fixture(fixture);

        std::ifstream sprite_file(fixture.root / "assets" / "sprites" / "hero.sprite.json", std::ios::binary);
        const std::string sprite_text{std::istreambuf_iterator<char>(sprite_file), std::istreambuf_iterator<char>()};
        const auto sprite = noemancer::SpriteAssetCodec::parse_json(sprite_text);
        require(static_cast<bool>(sprite), "Sprite 0.2 fixture could not be parsed");
        require(sprite.document->schema == "noemancer.sprite-asset/0.2" && sprite.document->material &&
                    sprite.document->material->shading_model == "lit" &&
                    sprite.document->material->normal_strength == 0.75F &&
                    sprite.document->material->metallic == 0.35F &&
                    sprite.document->material->roughness == 0.45F,
                "Sprite 0.2 material authority was not authored as expected");
        const std::vector<std::string> sprite_dependencies =
            noemancer::SpriteAssetCodec::asset_dependencies(*sprite.document);
        require(sprite_dependencies == std::vector<std::string>{
                    "texture.hero", "texture.hero.depth", "texture.hero.emissive", "texture.hero.normal"},
                "Sprite 0.2 dependency extraction drifted");

        const auto loaded = noemancer::load_project(fixture.root);
        require(static_cast<bool>(loaded), "temporary Project 0.2 fixture could not be loaded");
        require(loaded.project->schema == "noemancer.project/0.2" && loaded.project->hybrid_pixel_profile &&
                    loaded.project->script_project && loaded.project->hud_document &&
                    loaded.project->input_actions.size() == 2U,
                "Project 0.2 authorities were not loaded together");
        require(noemancer::collect_scene_asset_ids(*loaded.startup_scene) ==
                    std::vector<std::string>{"sprite.hero"},
                "startup scene did not expose the Sprite root");

        auto input = make_package_input(fixture, loaded, sprite_dependencies);
        const auto plan = noemancer::plan_package(input);
        assert_authoritative_profile_and_inputs(input, plan);
        assert_asset_closure(plan, sprite_dependencies);

        const auto repeat = noemancer::plan_package(input);
        require(noemancer::package_plan_json(plan) == noemancer::package_plan_json(repeat) &&
                    plan.content_hash == repeat.content_hash && plan.plan_id == repeat.plan_id,
                "Hybrid production package planning was not deterministic");

        auto changed_profile = input;
        changed_profile.project.hybrid_pixel_profile->profile_id = "lumen-run.hybrid.changed";
        const auto changed_profile_plan = noemancer::plan_package(changed_profile);
        require(changed_profile_plan.valid && changed_profile_plan.content_hash != plan.content_hash &&
                    changed_profile_plan.game_profile_json != plan.game_profile_json,
                "Hybrid Pixel profile drift did not change package identity");

        auto changed_input = input;
        changed_input.project.input_actions.front().bindings.front().source = "keyboard.d";
        const auto changed_input_plan = noemancer::plan_package(changed_input);
        require(changed_input_plan.valid && changed_input_plan.content_hash != plan.content_hash,
                "Input action drift did not change package identity");

        auto missing_dependency = input;
        missing_dependency.cook_manifest.outputs.front().dependencies.push_back("texture.hero.missing");
        const auto missing_dependency_plan = noemancer::plan_package(missing_dependency);
        require(!missing_dependency_plan.valid &&
                    missing_dependency_plan.code == "package.artifact-dependency-missing" &&
                    has_diagnostic(missing_dependency_plan, "package.artifact-dependency-missing"),
                "Package planner accepted a missing Sprite dependency");

        std::cout << "hybrid_pixel_production_package_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hybrid_pixel_production_package_tests: " << error.what() << '\n';
        return 1;
    }
}
