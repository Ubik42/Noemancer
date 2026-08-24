#include "engine/asset_registry.hpp"

#include <nlohmann/json.hpp>
#include "engine/gltf_mesh.hpp"
#include "engine/image_decoder.hpp"
#include "engine/ktx2_cook_adapter.hpp"
#include "engine/mesh_runtime_artifact.hpp"
#include "engine/process_diagnostics.hpp"

#include <algorithm>
#include <iostream>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

int main() {
    noemancer::configure_process_diagnostics("test.asset-registry");
    noemancer::AssetRegistry registry;
    constexpr std::array<std::uint8_t,16> pixels{{255U,0U,0U,255U,0U,255U,0U,255U,0U,0U,255U,255U,255U,255U,255U,255U}};
    const auto encoded_png=noemancer::encode_png_rgba8(2U,2U,pixels);
    const auto decoded_png=noemancer::decode_png_rgba8(std::as_bytes(std::span(encoded_png.bytes)));
    if (!encoded_png.valid || !decoded_png.valid || decoded_png.width!=2U || decoded_png.height!=2U || decoded_png.rgba8!=std::vector<std::uint8_t>(pixels.begin(),pixels.end())) {
        std::cerr << "Bounded PNG decoder did not produce a validated RGBA8 payload: "
                  << decoded_png.code << " - " << decoded_png.detail << '\n';
        return 11;
    }
    if (registry.records().size() != 43 || !registry.errors().empty()) {
        std::cerr << "Canonical asset registry did not load all project assets\n";
        return 1;
    }
    const auto streaming_policy_root = std::filesystem::temp_directory_path() /
        "noemancer-asset-streaming-policy-test";
    std::filesystem::remove_all(streaming_policy_root);
    std::filesystem::create_directories(streaming_policy_root);
    {
        std::ofstream manifest(streaming_policy_root / "registry.json", std::ios::binary);
        manifest << R"({"schema":"noemancer.assets/0.1","assets":[
          {"id":"asset.policy.default","displayName":"Default Texture","kind":"Texture","uri":"builtin://texture/default"},
          {"id":"asset.policy.explicit","displayName":"Explicit Texture","kind":"Texture","uri":"builtin://texture/explicit",
           "streamingPolicy":{"mode":"resident","importance":"critical","priority":1000}}
        ]})";
    }
    noemancer::AssetRegistry streaming_policy_registry(streaming_policy_root);
    const auto* default_policy_asset = streaming_policy_registry.find("asset.policy.default");
    const auto* explicit_policy_asset = streaming_policy_registry.find("asset.policy.explicit");
    if (!streaming_policy_registry.errors().empty() || default_policy_asset == nullptr ||
        explicit_policy_asset == nullptr || default_policy_asset->streaming_mode != "stream" ||
        default_policy_asset->streaming_importance != "normal" ||
        default_policy_asset->streaming_priority != 500U ||
        explicit_policy_asset->streaming_mode != "resident" ||
        explicit_policy_asset->streaming_importance != "critical" ||
        explicit_policy_asset->streaming_priority != 1000U) {
        std::cerr << "Asset registry did not preserve authored streaming policy defaults and overrides\n";
        return 23;
    }
    const auto expected_streaming_policy = nlohmann::json{
        {"mode", "resident"}, {"importance", "critical"}, {"priority", 1000}
    };
    const auto registry_projection = nlohmann::json::parse(streaming_policy_registry.registry_json());
    const auto query_projection = nlohmann::json::parse(streaming_policy_registry.query_json(
        {.text = "explicit", .limit = 4}));
    const auto inspect_projection = nlohmann::json::parse(
        streaming_policy_registry.inspect_json("asset.policy.explicit"));
    const auto find_asset_projection = [](const nlohmann::json& document, const std::string_view id) {
        for (const auto& asset : document.at("assets")) {
            if (asset.at("id").get<std::string>() == id) return asset;
        }
        return nlohmann::json{};
    };
    if (find_asset_projection(registry_projection, "asset.policy.explicit").at("streamingPolicy") !=
            expected_streaming_policy ||
        find_asset_projection(query_projection, "asset.policy.explicit").at("streamingPolicy") !=
            expected_streaming_policy ||
        inspect_projection.at("asset").at("streamingPolicy") != expected_streaming_policy ||
        find_asset_projection(registry_projection, "asset.policy.default").at("streamingPolicy") !=
            nlohmann::json{{"mode", "stream"}, {"importance", "normal"}, {"priority", 500}}) {
        std::cerr << "Asset registry, query and inspect projections diverged for streaming policy\n";
        return 24;
    }
    const std::array<std::string_view, 7> invalid_streaming_policies{
        R"("streamingPolicy":"stream")",
        R"("streamingPolicy":{"mode":"cache","importance":"normal","priority":500})",
        R"("streamingPolicy":{"mode":"stream","importance":"urgent","priority":500})",
        R"("streamingPolicy":{"mode":"stream","importance":"normal","priority":1001})",
        R"("streamingPolicy":{"mode":"stream","importance":"normal","priority":-1})",
        R"("streamingPolicy":{"mode":"stream","importance":"normal"})",
        R"("streamingPolicy":{"mode":"stream","importance":"normal","priority":500,"extra":true})"
    };
    for (std::size_t index = 0; index < invalid_streaming_policies.size(); ++index) {
        const auto invalid_root = streaming_policy_root / ("invalid-" + std::to_string(index));
        std::filesystem::create_directories(invalid_root);
        std::ofstream manifest(invalid_root / "registry.json", std::ios::binary);
        manifest << R"({"schema":"noemancer.assets/0.1","assets":[{"id":"asset.invalid","displayName":"Invalid","kind":"Texture","uri":"builtin://texture/invalid",)"
                 << invalid_streaming_policies[index] << R"(}]})";
        manifest.close();
        noemancer::AssetRegistry invalid_registry(invalid_root);
        if (invalid_registry.errors().empty()) {
            std::cerr << "Asset registry accepted invalid streaming policy case " << index << '\n';
            return 25;
        }
    }
    std::filesystem::remove_all(streaming_policy_root);
    const auto* scene = registry.find("asset.scene.bootstrap");
    const auto* model = registry.find("asset.test.kenney.alien");
    const auto* material_reference = registry.find("asset.test.material-reference");
    const auto* shader = registry.find("asset.shader.scene-lit-fragment");
    const auto* tone_shader = registry.find("asset.shader.tone-map-fragment");
    const auto* sprite_shader = registry.find("asset.shader.sprite-fragment");
    const auto* bloom_downsample_shader = registry.find("asset.shader.bloom-downsample-fragment");
    const auto* bloom_upsample_shader = registry.find("asset.shader.bloom-upsample-fragment");
    const auto* ao_denoise_shader = registry.find("asset.shader.ao-denoise-fragment");
    const auto* fxaa_shader = registry.find("asset.shader.fxaa-fragment");
    const auto* taa_shader = registry.find("asset.shader.taa-fragment");
    const auto* vfx_shader = registry.find("asset.shader.vfx-sim-compute");
    const auto* vfx_spawn_shader = registry.find("asset.shader.vfx-spawn-compute");
    const auto* vfx_group_shader = registry.find("asset.shader.vfx-group-compute");
    const auto* vfx_sort_shader = registry.find("asset.shader.vfx-sort-alpha-compute");
    const auto* ui_shader = registry.find("asset.shader.retained-ui-fragment");
    const auto* environment = registry.find("asset.environment.procedural-sky");
    if (scene == nullptr || model == nullptr || material_reference == nullptr ||
        !scene->available || !model->available || !material_reference->available ||
        shader == nullptr || !shader->available || tone_shader == nullptr || !tone_shader->available ||
        sprite_shader == nullptr || !sprite_shader->available ||
        bloom_downsample_shader == nullptr || !bloom_downsample_shader->available ||
        bloom_upsample_shader == nullptr || !bloom_upsample_shader->available ||
        ao_denoise_shader == nullptr || !ao_denoise_shader->available ||
        fxaa_shader == nullptr || !fxaa_shader->available || taa_shader == nullptr || !taa_shader->available ||
        vfx_shader == nullptr || !vfx_shader->available || vfx_spawn_shader == nullptr || !vfx_spawn_shader->available ||
        vfx_group_shader == nullptr || !vfx_group_shader->available || vfx_sort_shader == nullptr || !vfx_sort_shader->available ||
        ui_shader == nullptr || !ui_shader->available ||
        environment == nullptr || !environment->available ||
        !scene->content_hash.starts_with("sha256:") ||
        !model->content_hash.starts_with("sha256:") || !shader->content_hash.starts_with("sha256:") ||
        !vfx_shader->content_hash.starts_with("sha256:")) {
        std::cerr << "Required source assets do not have verified content identities\n";
        return 2;
    }
    if (model->content_hash != "sha256:fc660db2b2efa44b9968c119959c9ff99ba3d32aff21e728938b6f3e064d587d") {
        std::cerr << "Asset SHA-256 implementation does not match the tracked fixture\n";
        return 6;
    }
    const auto revision = registry.revision();
    const auto hash = model->content_hash;
    if (!registry.refresh() || registry.revision() != revision + 1 ||
        registry.find("asset.test.kenney.alien")->content_hash != hash) {
        std::cerr << "Asset discovery is not deterministic across refresh\n";
        return 3;
    }
    noemancer::AssetQuery query{
        .tags = {"gltf"},
        .limit = 2
    };
    const auto first_page = registry.query_json(query);
    if (first_page.find(R"("total":4)") == std::string::npos ||
        first_page.find("nextCursor") == std::string::npos) {
        std::cerr << "Asset query did not filter and paginate semantic tags\n";
        return 4;
    }
    const auto inspection = registry.inspect_json("asset.test.kenney.alien");
    if (inspection.find(R"("valid":true)") == std::string::npos ||
        inspection.find(R"("format":"glb")") == std::string::npos ||
        inspection.find(R"("meshes":)") == std::string::npos ||
        inspection.find(R"("primitiveCount":18)") == std::string::npos ||
        inspection.find("positionBounds") == std::string::npos || inspection.find("primitiveBounds") == std::string::npos) {
        std::cerr << "GLB semantic inspection did not expose topology and bounds\n";
        return 7;
    }
    const auto decoded = noemancer::decode_glb_mesh(registry.asset_root() / model->relative_path);
    if (!decoded.valid || decoded.vertices.size() != 637U || decoded.indices.size() != 876U ||
        decoded.primitives.size() != 18U || decoded.primitives.front().index_count == 0U || decoded.primitives.front().bounds_radius<=0.0F) {
        std::cerr << "GLB payload decoder did not produce renderable geometry and material ranges\n";
        return 10;
    }
    const auto material_decoded = noemancer::decode_glb_mesh(registry.asset_root() / material_reference->relative_path);
    if (!material_decoded.valid || material_decoded.primitives.size()!=3U || material_decoded.images.size()!=5U ||
        std::ranges::count_if(material_decoded.primitives,[](const auto& primitive){return primitive.normal_image>=0;})!=3 ||
        std::ranges::count_if(material_decoded.primitives,[](const auto& primitive){return primitive.metallic_roughness_image>=0;})!=3 ||
        std::ranges::count_if(material_decoded.primitives,[](const auto& primitive){return primitive.occlusion_image>=0;})!=3 ||
        std::ranges::count_if(material_decoded.primitives,[](const auto& primitive){return primitive.emissive_image>=0;})!=3 ||
        std::ranges::count_if(material_decoded.primitives,[](const auto& primitive){return primitive.alpha_mode=="MASK";})!=1 ||
        std::ranges::count_if(material_decoded.primitives,[](const auto& primitive){return primitive.alpha_mode=="BLEND";})!=1) {
        std::cerr << "Complete glTF material reference did not decode every production material channel\n";
        return 26;
    }
    const auto cook_plan = registry.cook_plan_json(
        {"asset.test.kenney.alien"},
        "windows-x64-debug");
    if (cook_plan.find(R"("valid":true)") == std::string::npos ||
        cook_plan.find("gltf.binary/0.1") == std::string::npos ||
        cook_plan.find("contentHash") == std::string::npos ||
        cook_plan.find("cache://sha256/") == std::string::npos) {
        std::cerr << "Cook plan is missing importer or content-addressed cache evidence\n";
        return 5;
    }
    const auto generated_root = registry.asset_root().parent_path() / "generated";
    const auto cook_plan_json = nlohmann::json::parse(cook_plan);
    const auto cache_uri = cook_plan_json.at("inputs").front().at("cacheUri").get<std::string>();
    const auto model_hash = cache_uri.substr(cache_uri.find_last_of('/') + 1U);
    const auto model_cache = generated_root / "cook-cache" / model_hash;
    const auto model_manifest = generated_root / "cook-manifests" /
        (cook_plan_json.at("planId").get<std::string>() + ".json");
    std::filesystem::remove_all(model_cache);
    std::filesystem::remove(model_manifest);
    const auto fbx_plan = registry.cook_plan_json(
        {"asset.local.mixamo.rumba-01"},
        "windows-x64-debug");
    if (fbx_plan.find(R"("valid":true)") == std::string::npos ||
        fbx_plan.find("ufbx.scene/0.23.0") == std::string::npos) {
        std::cerr << "FBX cook plan did not select the pinned scene importer\n";
        return 8;
    }
    const auto dry_run = registry.apply_cook_plan_json(cook_plan, true);
    const auto applied = registry.apply_cook_plan_json(cook_plan, false);
    const auto cache_hit = registry.apply_cook_plan_json(cook_plan, false);
    if (dry_run.find(R"("success":true)") == std::string::npos ||
        applied.find("generated://cook-cache/") == std::string::npos ||
        cache_hit.find(R"("cacheHits":1)") == std::string::npos) {
        std::cerr << "Cook plan did not dry-run, commit and reuse its immutable cache\n";
        return 9;
    }
    const auto applied_json = nlohmann::json::parse(applied);
    const auto& applied_artifacts = applied_json.at("artifacts");
    const auto mesh_payload_uri = "generated://cook-cache/" + model_hash + "/payload.meshbin";
    if (!std::filesystem::is_regular_file(model_cache / "payload.meshbin") ||
        !std::filesystem::is_regular_file(model_cache / "asset.json") ||
        !std::ranges::any_of(applied_artifacts, [&](const auto& artifact) {
            return artifact.get<std::string>() == mesh_payload_uri;
        })) {
        std::cerr << "GLB Cook did not commit a content-addressed meshbin payload.\n";
        return 18;
    }
    {
        std::ifstream payload(model_cache / "payload.meshbin", std::ios::binary);
        std::array<char, 8> magic{};
        payload.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        const std::string magic_string(magic.data(), magic.size());
        if (magic_string != "NMMESH02" || !payload ||
            std::filesystem::file_size(model_cache / "payload.meshbin") <= magic.size()) {
            std::cerr << "GLB Cook payload does not have the deterministic NMMESH02 header.\n";
            return 19;
        }
    }
    nlohmann::json cooked_metadata;
    {
        std::ifstream metadata_stream(model_cache / "asset.json", std::ios::binary);
        cooked_metadata = nlohmann::json::parse(metadata_stream);
    }
    if (cooked_metadata.at("payloadUri") != mesh_payload_uri ||
        cooked_metadata.at("payloadFormat") != "noemancer/meshbin/0.2" ||
        cooked_metadata.at("recipeHash") != cook_plan_json.at("inputs").front().at("recipeHash") ||
        cooked_metadata.at("importedMetadata").at("format").get<std::string>() !=
            std::string(noemancer::mesh_runtime_artifact_schema) ||
        cooked_metadata.at("importedMetadata").at("runtimeContract").at("sourceDecodeAtRuntime") != false ||
        cooked_metadata.at("importedMetadata").at("runtimeContract").at("primitiveCount") != 18U) {
        std::cerr << "GLB Cook metadata did not expose the self-contained runtime mesh contract.\n";
        return 20;
    }
    nlohmann::json manifest_json;
    {
        std::ifstream manifest_stream(model_manifest, std::ios::binary);
        manifest_json = nlohmann::json::parse(manifest_stream);
    }
    if (manifest_json.at("outputs").size() != 1U ||
        manifest_json.at("outputs").front().at("payloadUri") != mesh_payload_uri ||
        manifest_json.at("outputs").front().at("payloadFormat") != "noemancer/meshbin/0.2" ||
        manifest_json.at("outputs").front().at("payloadHash").get<std::string>().empty()) {
        std::cerr << "Cook manifest did not preserve the actual meshbin payload URI.\n";
        return 21;
    }
    const auto stale_plan = registry.cook_plan_json({"asset.test.kenney.alien"}, "windows-x64-debug");
    if (!registry.refresh() || registry.apply_cook_plan_json(stale_plan, false).find(
        R"("code":"asset.registry-revision-conflict")") == std::string::npos) {
        std::cerr << "Cook apply did not reject a plan created against an older registry revision.\n";
        return 22;
    }
    const auto overlay=std::filesystem::temp_directory_path()/"noemancer-asset-overlay-test";
    std::filesystem::remove_all(overlay);std::filesystem::create_directories(overlay/"textures");
    {std::ofstream stream(overlay/"textures"/"project-test.png",std::ios::binary);stream.write(
        reinterpret_cast<const char*>(encoded_png.bytes.data()),static_cast<std::streamsize>(encoded_png.bytes.size()));}
    if(!registry.add_root(overlay)||registry.asset_roots().size()!=2) return 12;
    const auto project_asset=std::ranges::find_if(registry.records(),[&](const noemancer::AssetRecord& asset){
        return asset.source_root==std::filesystem::absolute(overlay).lexically_normal().generic_string()&&asset.relative_path=="textures/project-test.png";});
    if(project_asset==registry.records().end()||project_asset->import_state!="unregistered"||
       registry.source_path(*project_asset)!=std::filesystem::absolute(overlay/"textures"/"project-test.png").lexically_normal()) {
        std::cerr<<"Registry-free project asset overlay was not discovered with its owning source root\n";return 13;
    }
    const auto overlay_plan=registry.cook_plan_json({project_asset->id},"windows-x64-debug");
    if(overlay_plan.find(R"("valid":true)")==std::string::npos||
       registry.apply_cook_plan_json(overlay_plan,true).find(R"("success":true)")==std::string::npos) return 14;
    std::filesystem::remove_all(overlay);

    const auto sprite_test_root=std::filesystem::temp_directory_path()/"noemancer-sprite-cook-test";
    const auto sprite_overlay=sprite_test_root/"assets";
    std::filesystem::remove_all(sprite_test_root);std::filesystem::create_directories(sprite_overlay/"sprites");
    {std::ofstream stream(sprite_overlay/"sprites"/"courier.png",std::ios::binary);stream.write(
        reinterpret_cast<const char*>(encoded_png.bytes.data()),static_cast<std::streamsize>(encoded_png.bytes.size()));}
    for(const auto* channel:{"courier-normal.png","courier-emissive.png","courier-depth.png"}) {
        std::ofstream stream(sprite_overlay/"sprites"/channel,std::ios::binary);stream.write(
            reinterpret_cast<const char*>(encoded_png.bytes.data()),static_cast<std::streamsize>(encoded_png.bytes.size()));
    }
    {std::ofstream stream(sprite_overlay/"sprites"/"courier.sprite.json",std::ios::binary);stream<<R"({
      "schema":"noemancer.sprite-asset/0.2","assetId":"sprite.courier","textureAsset":"texture.courier","textureSize":[2,2],
      "pixelsPerUnit":16,"sampling":"nearest","alphaMode":"cutout",
      "material":{"normalTextureAsset":"texture.courier.normal","emissiveMaskTextureAsset":"texture.courier.emissive","depthTextureAsset":"texture.courier.depth","normalStrength":0.75,"emissiveColor":[0.2,0.5,1.0],"emissiveIntensity":2.0,"depthBias":0.0005,"shadingModel":"unlit","metallic":0.35,"roughness":0.45,"receivesShadows":false,"castsShadows":true},
      "frames":[{"id":"idle.0","rect":[0,0,2,2],"trimOffset":[0,0],"sourceSize":[2,2],"pivot":[0.5,0.0],"collisionProfile":"courier.body"}],
      "clips":[{"id":"idle","looping":true,"frames":[{"frame":"idle.0","durationMs":120,"event":""}]}],
      "provenance":{"sourceUri":"sprites/courier.png","sourceSha256":"fixture","generator":"test","license":"CC0-1.0"}
    })";}
    {
        std::ifstream input(sprite_overlay/"sprites"/"courier.sprite.json",std::ios::binary);
        std::string copy{std::istreambuf_iterator<char>(input),{}};
        constexpr std::string_view source_id="sprite.courier";
        constexpr std::string_view copy_id="sprite.courier.copy";
        copy.replace(copy.find(source_id),source_id.size(),copy_id);
        std::ofstream output(sprite_overlay/"sprites"/"courier-copy.sprite.json",std::ios::binary);
        output<<copy;
    }
    {std::ofstream stream(sprite_overlay/"registry.json",std::ios::binary);stream<<R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"texture.courier","displayName":"Courier Atlas","kind":"Texture","uri":"asset://sprites/courier.png","path":"sprites/courier.png","license":"CC0-1.0","redistribution":"public"},
      {"id":"texture.courier.normal","displayName":"Courier Normal","kind":"Texture","uri":"asset://sprites/courier-normal.png","path":"sprites/courier-normal.png","license":"CC0-1.0","redistribution":"public"},
      {"id":"texture.courier.emissive","displayName":"Courier Emissive","kind":"Texture","uri":"asset://sprites/courier-emissive.png","path":"sprites/courier-emissive.png","license":"CC0-1.0","redistribution":"public"},
      {"id":"texture.courier.depth","displayName":"Courier Depth","kind":"Texture","uri":"asset://sprites/courier-depth.png","path":"sprites/courier-depth.png","license":"CC0-1.0","redistribution":"public"},
      {"id":"sprite.courier","displayName":"Courier Sprite","kind":"Sprite","uri":"asset://sprites/courier.sprite.json","path":"sprites/courier.sprite.json","license":"CC0-1.0","redistribution":"public","dependencies":["texture.courier"]},
      {"id":"sprite.courier.copy","displayName":"Courier Sprite Copy","kind":"Sprite","uri":"asset://sprites/courier-copy.sprite.json","path":"sprites/courier-copy.sprite.json","license":"CC0-1.0","redistribution":"public","dependencies":["texture.courier"]}
    ]})";}
    noemancer::AssetRegistry sprite_registry(sprite_overlay);
    const auto sprite_inspection=sprite_registry.inspect_json("sprite.courier");
    const auto sprite_plan=sprite_registry.cook_plan_json({"sprite.courier"},"windows-x64-debug");
    const auto sprite_receipt=sprite_registry.apply_cook_plan_json(sprite_plan,false);
    const auto sprite_cache_hit=sprite_registry.apply_cook_plan_json(sprite_plan,false);
    const auto sprite_dependencies=std::vector<std::string>{"texture.courier","texture.courier.depth","texture.courier.emissive","texture.courier.normal"};
    const auto sprite_asset=sprite_registry.find("sprite.courier");
    const auto sprite_inspection_json=nlohmann::json::parse(sprite_inspection);
    const auto sprite_plan_json=nlohmann::json::parse(sprite_plan);
    const auto sprite_registry_json=nlohmann::json::parse(sprite_registry.registry_json());
    const auto sprite_projection=find_asset_projection(sprite_registry_json,"sprite.courier");
    const auto sprite_plan_input=[&] {
        for(const auto& input:sprite_plan_json.at("inputs"))if(input.at("assetId")=="sprite.courier")return input;
        return nlohmann::json{};
    }();
    if(sprite_asset==nullptr||sprite_asset->dependencies!=sprite_dependencies||sprite_projection.at("dependencies")!=sprite_dependencies||
       sprite_inspection.find(R"("frameCount":1)")==std::string::npos||
       sprite_inspection_json.at("importedMetadata").at("dependencies")!=sprite_dependencies||
       sprite_inspection_json.at("renderPayload").at("dependencies")!=sprite_dependencies||
       sprite_inspection_json.at("renderPayload").at("material").at("shadingModel")!="unlit"||
       sprite_inspection_json.at("renderPayload").at("material").at("metallic")!=0.35F||
       sprite_inspection_json.at("renderPayload").at("material").at("roughness")!=0.45F||
       sprite_inspection_json.at("renderPayload").at("material").at("receivesShadows")!=false||
       sprite_inspection_json.at("renderPayload").at("material").at("castsShadows")!=true||
       sprite_plan.find("noemancer.sprite-asset/0.2")==std::string::npos||
       sprite_plan.find("texture.courier.normal")==std::string::npos||
       sprite_plan.find("texture.courier.emissive")==std::string::npos||
       sprite_plan.find("texture.courier.depth")==std::string::npos||
       !sprite_plan_input.contains("dependencies")||sprite_plan_input.at("dependencies")!=sprite_dependencies||
       sprite_receipt.find(R"("success":true)")==std::string::npos||
       sprite_receipt.find(R"("cacheMisses":5)")==std::string::npos||
       sprite_receipt.find(R"("cacheHits":0)")==std::string::npos||
       sprite_cache_hit.find(R"("cacheHits":5)")==std::string::npos) {
        std::cerr<<"Sprite descriptor and material textures did not form one content-addressed cook closure\n"
                 <<sprite_inspection<<'\n'<<sprite_plan<<'\n'<<sprite_receipt<<'\n'<<sprite_cache_hit<<'\n';return 15;
    }
    const auto sprite_manifest_path=sprite_test_root/"generated"/"cook-manifests"/
        (nlohmann::json::parse(sprite_plan).at("planId").get<std::string>()+".json");
    nlohmann::json sprite_manifest;{std::ifstream stream(sprite_manifest_path);stream>>sprite_manifest;}
    const auto sprite_cache_hash=sprite_plan_json.at("inputs").front().at("cacheUri").get<std::string>().substr(
        sprite_plan_json.at("inputs").front().at("cacheUri").get<std::string>().find_last_of('/')+1U);
    nlohmann::json sprite_metadata;
    {std::ifstream stream(sprite_test_root/"generated"/"cook-cache"/sprite_cache_hash/"asset.json");stream>>sprite_metadata;}
    if(sprite_metadata.at("dependencies")!=sprite_dependencies||
       sprite_metadata.at("importedMetadata").at("dependencies")!=sprite_dependencies||
       sprite_metadata.at("importedMetadata").at("runtimeContract").at("dependencies")!=sprite_dependencies||
       sprite_metadata.at("importedMetadata").at("runtimeContract").at("material").at("shadingModel")!="unlit"||
       sprite_metadata.at("importedMetadata").at("runtimeContract").at("material").at("metallic")!=0.35F||
       sprite_metadata.at("importedMetadata").at("runtimeContract").at("material").at("roughness")!=0.45F||
       sprite_metadata.at("importedMetadata").at("runtimeContract").at("material").at("receivesShadows")!=false||
       sprite_metadata.at("importedMetadata").at("runtimeContract").at("material").at("castsShadows")!=true) {
        std::cerr<<"Sprite Cook metadata did not preserve material contract and dependency closure\n"<<sprite_metadata.dump()<<'\n';return 153;
    }
    const auto shared_plan=nlohmann::json::parse(
        sprite_registry.cook_plan_json({"sprite.courier","sprite.courier.copy"},"windows-x64-debug"));
    const auto shared_plan_repeat=nlohmann::json::parse(
        sprite_registry.cook_plan_json({"sprite.courier","sprite.courier.copy"},"windows-x64-debug"));
    std::size_t shared_sprite_inputs{};
    std::vector<std::string> shared_texture_inputs;
    bool shared_sprite_dependencies_valid=true;
    for(const auto& input:shared_plan.at("inputs")) {
        const auto id=input.at("assetId").get<std::string>();
        if(id=="sprite.courier"||id=="sprite.courier.copy") {
            ++shared_sprite_inputs;
            shared_sprite_dependencies_valid=shared_sprite_dependencies_valid&&input.value("dependencies",nlohmann::json::array())==sprite_dependencies;
        } else if(id.starts_with("texture.courier")) shared_texture_inputs.push_back(id);
    }
    bool shared_textures_unique=shared_texture_inputs.size()==sprite_dependencies.size();
    for(const auto& dependency:sprite_dependencies) {
        shared_textures_unique=shared_textures_unique&&std::count(shared_texture_inputs.begin(),shared_texture_inputs.end(),dependency)==1;
    }
    const auto shared_receipt=nlohmann::json::parse(sprite_registry.apply_cook_plan_json(shared_plan.dump(),false));
    const auto shared_manifest_path=sprite_test_root/"generated"/"cook-manifests"/
        (shared_plan.at("planId").get<std::string>()+".json");
    nlohmann::json shared_manifest;{std::ifstream stream(shared_manifest_path);stream>>shared_manifest;}
    bool shared_outputs_unique=true;
    for(std::size_t first=0;first<shared_manifest.at("outputs").size();++first) {
        for(std::size_t second=first+1;second<shared_manifest.at("outputs").size();++second) {
            const auto& left=shared_manifest.at("outputs").at(first);const auto& right=shared_manifest.at("outputs").at(second);
            shared_outputs_unique=shared_outputs_unique&&left.at("assetId")!=right.at("assetId")&&left.at("payloadUri")!=right.at("payloadUri");
        }
    }
    const auto find_output=[&](const nlohmann::json& manifest,const std::string& asset_id) {
        for(const auto& output:manifest.at("outputs"))if(output.at("assetId")==asset_id)return output;
        return nlohmann::json{};
    };
    bool shared_dependency_outputs_stable=true;
    for(const auto& dependency:sprite_dependencies) {
        const auto original=find_output(sprite_manifest,dependency);const auto shared=find_output(shared_manifest,dependency);
        shared_dependency_outputs_stable=shared_dependency_outputs_stable&&!original.empty()&&!shared.empty()&&
            original.at("payloadUri")==shared.at("payloadUri")&&original.at("payloadHash")==shared.at("payloadHash");
    }
    if(!shared_plan.at("valid").get<bool>()||shared_plan!=shared_plan_repeat||shared_plan.at("inputs").size()!=6U||
       shared_sprite_inputs!=2U||!shared_sprite_dependencies_valid||!shared_textures_unique||
       !shared_receipt.at("success").get<bool>()||shared_receipt.at("cacheHits")!=5U||shared_receipt.at("cacheMisses")!=1U||
       shared_manifest.at("outputs").size()!=6U||!shared_outputs_unique||!shared_dependency_outputs_stable) {
        std::cerr<<"Shared Sprite material dependencies did not produce one stable Cook closure without duplicate outputs\n"
                 <<shared_plan.dump()<<'\n'<<shared_receipt.dump()<<'\n';return 154;
    }
    std::size_t verified_ktx{};
    for(const auto& output:sprite_manifest.at("outputs")) {
        if(output.at("payloadFormat")!="ktx2")continue;
        auto relative=output.at("payloadUri").get<std::string>().substr(std::string("generated://").size());
        std::ifstream stream(sprite_test_root/"generated"/relative,std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),{}};
        const auto decoded=noemancer::decode_ktx2_rgba8(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
        if(!decoded.valid||decoded.width!=2U||decoded.height!=2U||decoded.rgba8.size()!=16U)return 151;
        ++verified_ktx;
    }
    if(verified_ktx!=4U)return 152;
    std::filesystem::remove_all(sprite_test_root);
    const auto tile_test_root=std::filesystem::temp_directory_path()/"noemancer-tile-cook-test";
    const auto tile_assets=tile_test_root/"assets";std::filesystem::remove_all(tile_test_root);std::filesystem::create_directories(tile_assets/"tiles");
    {std::ofstream stream(tile_assets/"tiles"/"terrain.sprite.json");stream<<R"({"schema":"noemancer.sprite-asset/0.1","assetId":"sprite.terrain","textureAsset":"texture.terrain","textureSize":[2,2],"pixelsPerUnit":2,"sampling":"nearest","alphaMode":"cutout","frames":[{"id":"ground","rect":[0,0,2,2],"trimOffset":[0,0],"sourceSize":[2,2],"pivot":[0.5,0.5],"collisionProfile":""}],"clips":[],"provenance":{"sourceUri":"tiles/terrain.png","sourceSha256":"fixture","generator":"test","license":"CC0-1.0"}})";}
    {std::ofstream stream(tile_assets/"tiles"/"terrain.png",std::ios::binary);stream.write(reinterpret_cast<const char*>(encoded_png.bytes.data()),static_cast<std::streamsize>(encoded_png.bytes.size()));}
    {std::ofstream stream(tile_assets/"tiles"/"terrain.tile-palette.json");stream<<R"({"schema":"noemancer.tile-palette/0.2","assetId":"palette.terrain","spriteAsset":"sprite.terrain","tiles":[{"id":"ground","frame":"ground","collision":"solid","tags":["terrain"],"autotile":{"group":"terrain","variants":[{"mask":2,"frame":"ground"},{"mask":8,"frame":"ground"}]}}]})";}
    {std::ofstream stream(tile_assets/"tiles"/"level.tilemap.json");stream<<R"({"schema":"noemancer.tilemap/0.1","assetId":"tilemap.level","paletteAsset":"palette.terrain","cellSize":[1,1],"chunkSize":[8,8],"layers":[{"id":"ground","sortingLayer":"terrain","sortingOrder":0,"collisionEnabled":true,"chunks":[{"position":[0,0],"cells":[[0,0,"ground"]]}]}]})";}
    {std::ofstream stream(tile_assets/"registry.json");stream<<R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"texture.terrain","displayName":"Terrain Texture","kind":"Texture","uri":"asset://tiles/terrain.png","path":"tiles/terrain.png","license":"CC0-1.0","redistribution":"public"},
      {"id":"sprite.terrain","displayName":"Terrain Sprite","kind":"Sprite","uri":"asset://tiles/terrain.sprite.json","path":"tiles/terrain.sprite.json","license":"CC0-1.0","redistribution":"public"},
      {"id":"palette.terrain","displayName":"Terrain Palette","kind":"TilePalette","uri":"asset://tiles/terrain.tile-palette.json","path":"tiles/terrain.tile-palette.json","license":"CC0-1.0","redistribution":"public"},
      {"id":"tilemap.level","displayName":"Level Tilemap","kind":"Tilemap","uri":"asset://tiles/level.tilemap.json","path":"tiles/level.tilemap.json","license":"CC0-1.0","redistribution":"public"}]})";}
    noemancer::AssetRegistry tile_registry(tile_assets);const auto tile_inspection=tile_registry.inspect_json("tilemap.level");
    const auto tile_plan=tile_registry.cook_plan_json({"tilemap.level"},"windows-x64-debug");
    const auto tile_plan_json=nlohmann::json::parse(tile_plan);const auto tile_receipt=tile_registry.apply_cook_plan_json(tile_plan,false);
    if(tile_inspection.find(R"("cellCount":1)")==std::string::npos||!tile_plan_json.at("valid").get<bool>()||
       tile_plan_json.at("inputs").size()!=4||tile_plan.find("noemancer.tile-palette/0.2")==std::string::npos||
       tile_plan.find("sprite.terrain")==std::string::npos||tile_plan.find("texture.terrain")==std::string::npos||
       tile_receipt.find(R"("success":true)")==std::string::npos){std::cerr<<"Tilemap cook dependency closure is incomplete\n"<<tile_plan<<'\n'<<tile_receipt<<'\n';return 16;}
    const auto tile_source=tile_assets/"tiles"/"level.tilemap.json";
    const auto read_tile_source=[&] {std::ifstream input(tile_source,std::ios::binary);return std::string{std::istreambuf_iterator<char>(input),{}};};
    const auto before_edit=read_tile_source();const auto after_edit=before_edit+"\n";
    const auto compare_conflict=tile_registry.commit_text_source("tilemap.level",after_edit,"test.asset-history","stale source");
    const auto source_commit=tile_registry.commit_text_source("tilemap.level",after_edit,"test.asset-history",before_edit);
    const auto source_undo=tile_registry.undo_text_source("test.asset-history.undo");const auto after_undo=read_tile_source();
    const auto source_redo=tile_registry.redo_text_source("test.asset-history.redo");const auto after_redo=read_tile_source();
    {std::ofstream output(tile_source,std::ios::binary|std::ios::app);output<<" ";}
    const auto source_conflict=tile_registry.undo_text_source("test.asset-history.conflict");
    if(compare_conflict.success||compare_conflict.code!="asset.source-conflict"||!source_commit.success||
       source_commit.transaction_id==0U||!source_undo.success||source_undo.transaction_id!=source_commit.transaction_id||
       after_undo!=before_edit||!source_redo.success||source_redo.transaction_id!=source_commit.transaction_id||after_redo!=after_edit||
       source_conflict.success||source_conflict.code!="asset.history-conflict"||!tile_registry.can_undo_text_source()) {
        std::cerr<<"Text asset history did not commit, restore, replay and protect external edits\n";return 17;
    }
    std::filesystem::remove_all(tile_test_root);

    const auto transaction_test_root=std::filesystem::temp_directory_path()/"noemancer-animation-source-transaction-test";
    const auto transaction_assets=transaction_test_root/"assets";
    std::filesystem::remove_all(transaction_test_root);
    std::filesystem::create_directories(transaction_assets/"text");
    const auto transaction_source=transaction_assets/"text"/"authoring.txt";
    const std::string transaction_before="E0\n";
    {std::ofstream stream(transaction_source,std::ios::binary);stream<<transaction_before;}
    {std::ofstream stream(transaction_assets/"registry.json");stream<<R"({"schema":"noemancer.assets/0.1","assets":[
      {"id":"text.authoring","displayName":"Authoring Text","kind":"Text","uri":"asset://text/authoring.txt","path":"text/authoring.txt","license":"Noemancer project","redistribution":"public"}]})";}
    noemancer::AssetRegistry transaction_registry(transaction_assets);
    const auto read_transaction_source=[&] {std::ifstream input(transaction_source,std::ios::binary);return std::string{std::istreambuf_iterator<char>(input),{}};};
    const auto e1=transaction_registry.commit_text_source("text.authoring",transaction_before+"E1\n","test.transaction.e1");
    const auto a1=transaction_registry.commit_text_source("text.authoring",transaction_before+"E1\nA1\n","test.transaction.a1");
    const auto order_conflict=transaction_registry.rollback_text_source(e1.transaction_id,"test.transaction.rollback-order");
    const auto after_order_conflict=read_transaction_source();
    const auto rollback_a1=transaction_registry.rollback_text_source(a1.transaction_id,"test.transaction.rollback-a1");
    const auto after_a1_rollback=read_transaction_source();
    const auto rollback_e1=transaction_registry.rollback_text_source(e1.transaction_id,"test.transaction.rollback-e1");
    const auto after_e1_rollback=read_transaction_source();
    const auto e2=transaction_registry.commit_text_source("text.authoring",transaction_before+"E2\n","test.transaction.e2");
    const auto a2=transaction_registry.commit_text_source("text.authoring",transaction_before+"E2\nA2\n","test.transaction.a2");
    const auto undo_a2=transaction_registry.undo_text_source("test.transaction.undo-a2");
    const auto rollback_e2=transaction_registry.rollback_text_source(e2.transaction_id,"test.transaction.rollback-e2");
    const auto after_e2_rollback=read_transaction_source();
    const auto redo_preserved_after_rollback=transaction_registry.can_redo_text_source();
    const auto redo_after_rollback=transaction_registry.redo_text_source("test.transaction.redo-after-rollback");
    const auto after_redo_after_rollback=read_transaction_source();
    const auto redo_preserved_after_failed_redo=transaction_registry.can_redo_text_source();
    const auto e1_again=transaction_registry.commit_text_source("text.authoring",transaction_before+"E1-again\n","test.transaction.e1-again");
    {std::ofstream stream(transaction_source,std::ios::binary|std::ios::app);stream<<"external\n";}
    const auto source_conflict_rollback=transaction_registry.rollback_text_source(e1_again.transaction_id,"test.transaction.rollback-source");
    const auto after_source_conflict=read_transaction_source();
    if(!e1.success||e1.transaction_id==0U||!a1.success||a1.transaction_id<=e1.transaction_id||
       order_conflict.success||order_conflict.code!="asset.rollback-conflict"||after_order_conflict!=transaction_before+"E1\nA1\n"||
       !rollback_a1.success||rollback_a1.transaction_id!=a1.transaction_id||transaction_registry.can_redo_text_source()||
       after_a1_rollback!=transaction_before+"E1\n"||
       !rollback_e1.success||rollback_e1.transaction_id!=e1.transaction_id||after_e1_rollback!=transaction_before||
       !e2.success||!a2.success||a2.transaction_id<=e2.transaction_id||!undo_a2.success||
       undo_a2.transaction_id!=a2.transaction_id||!rollback_e2.success||rollback_e2.transaction_id!=e2.transaction_id||
       after_e2_rollback!=transaction_before||!redo_preserved_after_rollback||redo_after_rollback.success||
       redo_after_rollback.code!="asset.history-conflict"||redo_after_rollback.transaction_id!=a2.transaction_id||
       after_redo_after_rollback!=transaction_before||!redo_preserved_after_failed_redo||
       !e1_again.success||e1_again.transaction_id<=a2.transaction_id||source_conflict_rollback.success||
       source_conflict_rollback.code!="asset.rollback-conflict"||source_conflict_rollback.transaction_id!=e1_again.transaction_id||
       after_source_conflict!=transaction_before+"E1-again\nexternal\n") {
        std::cerr<<"Source transaction rollback did not enforce stack order, IDs and exact source matching\n";
        return 43;
    }
    std::filesystem::remove_all(transaction_test_root);

    const auto graph_test_root=std::filesystem::temp_directory_path()/"noemancer-animation-graph-registry-test";
    const auto graph_assets=graph_test_root/"assets";
    std::filesystem::remove_all(graph_test_root);
    std::filesystem::create_directories(graph_assets/"graphs");
    {
        std::ofstream stream(graph_assets/"graphs"/"locomotion.animation-graph.json");
        stream<<R"({
          "schemaVersion":"noemancer.animation-graph/0.1","assetId":"graph.locomotion",
          "parameters":[{"id":"speed","type":"float","default":0}],
          "nodes":[
            {"id":"idle","kind":"clip","clipAsset":"clip.idle","looping":true},
            {"id":"run","kind":"clip","clipAsset":"clip.idle","looping":true},
            {"id":"combat","kind":"state-machine","stateMachineAsset":"machine.combat"},
            {"id":"locomotion","kind":"blend-1d","parameter":"speed","children":[
              {"nodeId":"idle","threshold":0},{"nodeId":"run","threshold":1}]}
          ],
          "layers":[{"id":"base","rootNode":"locomotion","mode":"override","weight":1},
                     {"id":"combat","rootNode":"combat","mode":"additive","weight":1}],
          "masks":[],"syncGroups":[],"editor":{"nodes":[],"zoom":1,"pan":[0,0]}
        })";
    }
    {
        std::ofstream stream(graph_assets/"registry.json");
        stream<<R"({"schema":"noemancer.assets/0.1","assets":[
          {"id":"graph.locomotion","displayName":"Locomotion Graph","kind":"AnimationGraph",
           "uri":"asset://graphs/locomotion.animation-graph.json","path":"graphs/locomotion.animation-graph.json",
           "contentHash":"sha256:manifest-placeholder","dependencies":["stale.manifest-dependency"],"license":"Noemancer project","redistribution":"public"},
          {"id":"clip.idle","displayName":"Idle Clip","kind":"Animation","uri":"builtin://animation/test-idle"},
          {"id":"machine.combat","displayName":"Combat Machine","kind":"AnimationStateMachine","uri":"builtin://animation/combat"}
        ]})";
    }
    noemancer::AssetRegistry graph_registry(graph_assets);
    const std::vector<std::string> graph_dependencies{"clip.idle","machine.combat"};
    const auto* graph_asset=graph_registry.find("graph.locomotion");
    if(!graph_registry.errors().empty()||graph_asset==nullptr||graph_asset->hash_provenance!="computed"||
       graph_asset->dependencies!=graph_dependencies||
       graph_registry.find("graph.locomotion")->relative_path!="graphs/locomotion.animation-graph.json") {
        std::cerr<<"Animation Graph source was not recognized or codec dependencies did not replace manifest guesses\n";
        std::filesystem::remove_all(graph_test_root);return 27;
    }
    const auto graph_registry_projection=nlohmann::json::parse(graph_registry.registry_json());
    const auto graph_projection=find_asset_projection(graph_registry_projection,"graph.locomotion");
    if(graph_projection.at("dependencies")!=graph_dependencies) {
        std::cerr<<"Animation Graph direct dependencies were not published by Asset Registry\n";
        std::filesystem::remove_all(graph_test_root);return 28;
    }
    const auto graph_inspection=nlohmann::json::parse(graph_registry.inspect_json("graph.locomotion"));
    if(!graph_inspection.at("valid").get<bool>()||graph_inspection.at("importedMetadata").at("format")!=
       "noemancer.animation-graph/0.1"||graph_inspection.at("importedMetadata").at("dependencies")!=graph_dependencies||
       graph_inspection.at("renderPayload").at("nodeCount")!=4U||graph_inspection.at("renderPayload").at("dependencyCount")!=2U) {
        std::cerr<<"Animation Graph inspection did not expose the validated document and dependencies\n";
        std::filesystem::remove_all(graph_test_root);return 29;
    }
    const auto graph_plan=nlohmann::json::parse(graph_registry.cook_plan_json({"graph.locomotion"},"windows-x64-debug"));
    const auto graph_input=[&] {
        for(const auto& input:graph_plan.at("inputs"))if(input.at("assetId")=="graph.locomotion")return input;
        return nlohmann::json{};
    }();
    if(!graph_plan.at("valid").get<bool>()||graph_plan.at("inputs").size()!=3U||
       graph_input.at("importer")!="noemancer.animation-graph/0.1"||graph_input.at("dependencies")!=graph_dependencies) {
        std::cerr<<"Animation Graph Cook plan did not form a codec-derived dependency closure\n"<<graph_plan.dump()<<'\n';
        std::filesystem::remove_all(graph_test_root);return 30;
    }
    const auto graph_receipt=nlohmann::json::parse(graph_registry.apply_cook_plan_json(graph_plan.dump(),false));
    if(!graph_receipt.at("success").get<bool>()||graph_receipt.at("cacheMisses")!=3U) {
        std::cerr<<"Animation Graph Cook did not commit its dependency closure\n"<<graph_receipt.dump()<<'\n';
        std::filesystem::remove_all(graph_test_root);return 31;
    }
    const auto graph_hash=graph_asset->content_hash.substr(graph_asset->content_hash.find(':')+1);
    const auto graph_metadata_path=graph_test_root/"generated"/"cook-cache"/graph_hash/"asset.json";
    nlohmann::json graph_metadata;
    {std::ifstream stream(graph_metadata_path);stream>>graph_metadata;}
    if(graph_metadata.at("dependencies")!=graph_dependencies||
       graph_metadata.at("importedMetadata").at("dependencies")!=graph_dependencies||
       graph_metadata.at("importedMetadata").at("runtimeContract").at("dependencies")!=graph_dependencies||
       graph_metadata.at("importedMetadata").at("document").at("schemaVersion")!="noemancer.animation-graph/0.1") {
        std::cerr<<"Animation Graph Cook metadata did not retain codec-derived dependencies\n"<<graph_metadata.dump()<<'\n';
        std::filesystem::remove_all(graph_test_root);return 32;
    }
    const auto graph_cache_hit=nlohmann::json::parse(graph_registry.apply_cook_plan_json(graph_plan.dump(),false));
    if(!graph_cache_hit.at("success").get<bool>()||graph_cache_hit.at("cacheHits")!=3U) {
        std::cerr<<"Animation Graph Cook did not reuse its dependency closure cache\n"<<graph_cache_hit.dump()<<'\n';
        std::filesystem::remove_all(graph_test_root);return 33;
    }

    const auto graph_source_path=graph_assets/"graphs"/"locomotion.animation-graph.json";
    const auto graph_manifest_path=graph_assets/"registry.json";
    const auto read_graph_source=[&] {
        std::ifstream stream(graph_source_path,std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(stream),{}};
    };
    const auto write_graph_source=[&](const std::string& source) {
        std::ofstream stream(graph_source_path,std::ios::binary|std::ios::trunc);stream<<source;
    };
    const auto read_graph_manifest=[&] {
        std::ifstream stream(graph_manifest_path,std::ios::binary);
        return nlohmann::json::parse(stream);
    };
    const auto write_graph_manifest=[&](const nlohmann::json& manifest) {
        std::ofstream stream(graph_manifest_path,std::ios::binary|std::ios::trunc);stream<<manifest.dump(2);
    };
    const auto contains_registry_error=[&](const std::string_view code) {
        for(const auto& error:graph_registry.errors())if(error.find(code)!=std::string::npos)return true;
        return false;
    };
    const auto original_graph_source=read_graph_source();
    const auto original_graph_manifest=read_graph_manifest();

    auto mismatched_graph=nlohmann::json::parse(original_graph_source);
    mismatched_graph["assetId"]="graph.document-with-different-id";
    write_graph_source(mismatched_graph.dump());
    const auto mismatch_refresh=graph_registry.refresh();
    const auto mismatch_inspection=nlohmann::json::parse(graph_registry.inspect_json("graph.locomotion"));
    const auto mismatch_plan=nlohmann::json::parse(graph_registry.cook_plan_json({"graph.locomotion"},"windows-x64-debug"));
    if(mismatch_refresh||!contains_registry_error("animation.graph-identity-mismatch")||
       mismatch_inspection.at("valid").get<bool>()||mismatch_inspection.at("code")!="animation.graph-identity-mismatch"||
       mismatch_plan.at("valid").get<bool>()||mismatch_plan.dump().find("animation.graph-identity-mismatch")==std::string::npos) {
        std::cerr<<"Animation Graph Registry/document identity mismatch was not rejected consistently\n";
        std::filesystem::remove_all(graph_test_root);return 34;
    }
    write_graph_source(original_graph_source);
    if(!graph_registry.refresh()||!graph_registry.errors().empty()) {
        std::cerr<<"Animation Graph Registry did not recover after restoring a valid source\n";
        std::filesystem::remove_all(graph_test_root);return 35;
    }

    auto conflicting_manifest=original_graph_manifest;
    for(auto& asset:conflicting_manifest.at("assets"))
        if(asset.at("id")=="graph.locomotion")asset["kind"]="AnimationStateMachine";
    write_graph_manifest(conflicting_manifest);
    const auto conflict_refresh=graph_registry.refresh();
    const auto conflict_inspection=nlohmann::json::parse(graph_registry.inspect_json("graph.locomotion"));
    const auto conflict_plan=nlohmann::json::parse(graph_registry.cook_plan_json({"graph.locomotion"},"windows-x64-debug"));
    if(conflict_refresh||!contains_registry_error("asset.animation-format-conflict")||
       conflict_inspection.at("valid").get<bool>()||conflict_inspection.at("code")!="asset.animation-format-conflict"||
       conflict_plan.at("valid").get<bool>()||conflict_plan.dump().find("asset.animation-format-conflict")==std::string::npos) {
        std::cerr<<"Animation Graph kind/suffix conflicts were not rejected consistently\n";
        std::filesystem::remove_all(graph_test_root);return 36;
    }
    write_graph_manifest(original_graph_manifest);
    if(!graph_registry.refresh()||!graph_registry.errors().empty()) {
        std::cerr<<"Animation Graph Registry did not recover after restoring a valid manifest\n";
        std::filesystem::remove_all(graph_test_root);return 37;
    }

    auto wrong_dependency_graph=nlohmann::json::parse(original_graph_source);
    for(auto& node:wrong_dependency_graph.at("nodes"))
        if(node.at("kind")=="state-machine")node["stateMachineAsset"]="clip.idle";
    write_graph_source(wrong_dependency_graph.dump());
    const auto wrong_dependency_refresh=graph_registry.refresh();
    const auto wrong_dependency_inspection=nlohmann::json::parse(graph_registry.inspect_json("graph.locomotion"));
    const auto wrong_dependency_plan=nlohmann::json::parse(graph_registry.cook_plan_json({"graph.locomotion"},"windows-x64-debug"));
    if(wrong_dependency_refresh||!contains_registry_error("animation.graph-state-machine-dependency-kind-invalid")||
       wrong_dependency_inspection.at("valid").get<bool>()||wrong_dependency_inspection.at("code")!=
       "animation.graph-state-machine-dependency-kind-invalid"||wrong_dependency_plan.at("valid").get<bool>()||
       wrong_dependency_plan.dump().find("animation.graph-state-machine-dependency-kind-invalid")==std::string::npos) {
        std::cerr<<"Animation Graph stateMachineAsset type errors were not propagated through Registry\n";
        std::filesystem::remove_all(graph_test_root);return 38;
    }
    write_graph_source(original_graph_source);
    if(!graph_registry.refresh()||!graph_registry.errors().empty()) {
        std::cerr<<"Animation Graph Registry did not recover after restoring dependency references\n";
        std::filesystem::remove_all(graph_test_root);return 39;
    }

    auto oversized_string_graph=nlohmann::json::parse(original_graph_source);
    oversized_string_graph.at("nodes").at(0)["clipAsset"]=std::string(4097,'x');
    write_graph_source(oversized_string_graph.dump());
    const auto oversized_string_refresh=graph_registry.refresh();
    const auto oversized_string_inspection=nlohmann::json::parse(graph_registry.inspect_json("graph.locomotion"));
    const auto oversized_string_plan=nlohmann::json::parse(graph_registry.cook_plan_json({"graph.locomotion"},"windows-x64-debug"));
    if(oversized_string_refresh||!contains_registry_error("animation.graph-string-too-large")||
       oversized_string_inspection.at("valid").get<bool>()||oversized_string_inspection.at("code")!="animation.graph-string-too-large"||
       oversized_string_plan.at("valid").get<bool>()||oversized_string_plan.dump().find("animation.graph-string-too-large")==std::string::npos) {
        std::cerr<<"Animation Graph key-string budget was not enforced through Registry\n";
        std::filesystem::remove_all(graph_test_root);return 40;
    }
    write_graph_source(original_graph_source);
    if(!graph_registry.refresh()||!graph_registry.errors().empty()) {
        std::cerr<<"Animation Graph Registry did not recover after restoring bounded strings\n";
        std::filesystem::remove_all(graph_test_root);return 41;
    }

    const auto source_before_commit=read_graph_source();
    auto edited_graph=nlohmann::json::parse(source_before_commit);
    edited_graph["editor"]["pan"][0]=37.0F;
    const auto source_commit_after_manifest_hash=graph_registry.commit_text_source(
        "graph.locomotion",edited_graph.dump(),"test.graph-source","source that is no longer current");
    const auto graph_source_commit=graph_registry.commit_text_source(
        "graph.locomotion",edited_graph.dump(),"test.graph-source",source_before_commit);
    const auto* graph_after_commit=graph_registry.find("graph.locomotion");
    const auto graph_plan_after_commit=nlohmann::json::parse(
        graph_registry.cook_plan_json({"graph.locomotion"},"windows-x64-debug"));
    const auto graph_receipt_after_commit=nlohmann::json::parse(
        graph_registry.apply_cook_plan_json(graph_plan_after_commit.dump(),false));
    if(graph_after_commit==nullptr) {
        std::cerr<<"Animation Graph source commit removed its Registry record\n";
        std::filesystem::remove_all(graph_test_root);return 42;
    }
    const auto graph_hash_after_commit=graph_after_commit->content_hash.substr(
        graph_after_commit->content_hash.find(':')+1);
    nlohmann::json graph_metadata_after_commit;
    {std::ifstream stream(graph_test_root/"generated"/"cook-cache"/graph_hash_after_commit/"asset.json");stream>>graph_metadata_after_commit;}
    if(source_commit_after_manifest_hash.success||source_commit_after_manifest_hash.code!="asset.source-conflict"||
       !graph_source_commit.success||graph_after_commit==nullptr||graph_after_commit->hash_provenance!="computed"||
       graph_hash_after_commit==graph_hash||!graph_receipt_after_commit.at("success").get<bool>()||
       graph_receipt_after_commit.at("cacheMisses")!=1U||
       graph_metadata_after_commit.at("importedMetadata").at("document").at("editor").at("pan").at(0)!=37.0F) {
        std::cerr<<"Animation Graph source commit did not invalidate an explicit manifest hash cache\n"
                 <<graph_receipt_after_commit.dump()<<'\n';
        std::filesystem::remove_all(graph_test_root);return 42;
    }
    std::filesystem::remove_all(graph_test_root);
    return 0;
}
