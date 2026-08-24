#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "Unable to read shader source: " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::size_t matching_brace(const std::string& source, const std::size_t opening) {
    require(opening < source.size() && source[opening] == '{',
            "Expected an opening brace in shader source.");
    std::size_t depth = 0U;
    for (std::size_t index = opening; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            require(depth > 0U, "Shader source has an unmatched closing brace.");
            --depth;
            if (depth == 0U) {
                return index;
            }
        }
    }
    throw std::runtime_error("Shader source has an unterminated brace block.");
}

std::string block_after(const std::string& source,
                        const std::string& marker,
                        const std::string& context) {
    const auto marker_position = source.find(marker);
    require(marker_position != std::string::npos,
            "Missing " + context + " marker: " + marker);
    const auto opening = source.find('{', marker_position + marker.size());
    require(opening != std::string::npos, "Missing opening brace for " + context + ".");
    const auto closing = matching_brace(source, opening);
    return source.substr(opening, closing - opening + 1U);
}

std::size_t count_occurrences(const std::string& source, const std::string& needle) {
    std::size_t count = 0U;
    std::size_t offset = 0U;
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void test_metadata_semantics(const std::string& source) {
    const auto cbuffer = block_after(source, "cbuffer GpuDrivenDrawData", "draw metadata cbuffer");
    require(cbuffer.find("uint4 drawMetadata") != std::string::npos,
            "draw metadata must remain a uint4 cbuffer field.");
    require(cbuffer.find("drawMetadata.x = visible-index offset") != std::string::npos,
            "drawMetadata.x must document the visible-index offset semantic.");
    require(cbuffer.find("drawMetadata.y = candidate count belonging to this batch") != std::string::npos,
            "drawMetadata.y must document the batch candidate-count semantic.");
    require(cbuffer.find("drawMetadata.z = global candidate count") != std::string::npos,
            "drawMetadata.z must document the global candidate-count semantic.");
    require(source.find("drawMetadata.w") == std::string::npos,
            "drawMetadata.w must not acquire an undocumented storage-read semantic.");
    require(source.find("gpuDrivenInstanceCapacity = 16384u") != std::string::npos,
            "The shader must carry the fixed GPU instance/visible-index capacity contract.");
}

void test_storage_read_guards(const std::string& source) {
    const auto main_body = block_after(source, "VertexOutput main", "GPU-driven vertex main");
    const auto metadata_guard = main_body.find("drawMetadata.x > gpuDrivenInstanceCapacity");
    const auto range_guard = main_body.find("drawMetadata.x > drawMetadata.z");
    const auto lane_guard = main_body.find("instanceId >= drawMetadata.y");
    const auto visible_read = main_body.find("visibleIndices[visibleIndexOffset]");
    const auto candidate_guard = main_body.find("candidateIndex >= drawMetadata.z");
    const auto instance_read = main_body.find("instances[candidateIndex]");
    require(metadata_guard != std::string::npos && range_guard != std::string::npos,
            "Metadata capacity and offset-vs-global range guards are required.");
    require(lane_guard != std::string::npos, "Batch candidate-count lane guard is required.");
    require(visible_read != std::string::npos, "The guarded visible-index storage read is missing.");
    require(candidate_guard != std::string::npos, "Candidate-index range guard is required.");
    require(instance_read != std::string::npos, "The guarded instance storage read is missing.");
    require(metadata_guard < visible_read && range_guard < visible_read && lane_guard < visible_read,
            "All metadata and lane guards must precede the visibleIndices storage read.");
    require(visible_read < candidate_guard && candidate_guard < instance_read,
            "The candidate-index guard must sit between visibleIndices and instances reads.");
    require(main_body.find("instances[visibleIndices") == std::string::npos,
            "The shader must not combine unguarded visibleIndices and instances reads.");
    require(count_occurrences(main_body, "return clipped_vertex_output();") >= 3U,
            "Every invalid metadata/lane/candidate path must return the deterministic clip output.");
}

void test_deterministic_clip_output(const std::string& source) {
    const auto clip_body = block_after(source, "VertexOutput clipped_vertex_output", "deterministic clip helper");
    require(clip_body.find("output.position = float4(0.0f, 0.0f, -2.0f, 1.0f);") != std::string::npos,
            "Invalid lanes must use the deterministic outside-clip position.");
    require(clip_body.find("output.objectId = 0u;") != std::string::npos,
            "Invalid lanes must initialize objectId deterministically.");
}

void test_sprite_mixed_lighting_contract(const std::filesystem::path& repository_root) {
    const auto vertex = read_text(repository_root / "assets" / "shaders" / "sprite.vert.hlsl");
    const auto fragment = read_text(repository_root / "assets" / "shaders" / "sprite.frag.hlsl");
    const auto scene_lit = read_text(repository_root / "assets" / "shaders" / "scene_lit.frag.hlsl");
    const auto manifest = read_text(repository_root / "assets" / "shaders" / "shader-artifact-contract.json");

    require(vertex.find("float3 worldPosition : TEXCOORD0;") != std::string::npos,
            "Sprite vertex must export world position for clustered lighting.");
    require(vertex.find("float4 worldTangent : TEXCOORD2;") != std::string::npos &&
                vertex.find("const float3 modelNormal = mul((float3x3)instance.model") != std::string::npos,
            "Sprite vertex must export a model-derived tangent basis for the local XY plane.");
    require(vertex.find("float4 surfaceParameters;") != std::string::npos &&
                vertex.find("output.surfaceParameters = instance.surfaceParameters;") != std::string::npos,
            "Sprite instance surface parameters must cross the vertex/fragment boundary.");

    for (const auto& binding : {
             "Texture2D<float4> spriteTexture : register(t0, space2);",
             "Texture2D<float4> normalTexture : register(t1, space2);",
             "Texture2D<float4> emissiveMaskTexture : register(t2, space2);",
             "Texture2D<float4> depthTexture : register(t3, space2);",
             "Texture2DArray<float> shadowMap : register(t4, space2);",
             "Texture2DArray<float> localShadowMap : register(t5, space2);",
             "SamplerState shadowSampler : register(s4, space2);",
             "SamplerState localShadowSampler : register(s5, space2);",
             "StructuredBuffer<LocalLightData> localLights : register(t6, space2);",
             "StructuredBuffer<uint2> lightClusters : register(t7, space2);",
             "StructuredBuffer<uint> lightClusterIndices : register(t8, space2);",
             "cbuffer LightingData : register(b0, space3)"}) {
        require(fragment.find(binding) != std::string::npos,
                std::string("Sprite mixed-lighting resource binding is missing: ") + binding);
    }
    require(fragment.find("if (input.surfaceParameters.z <= 0.5f)") != std::string::npos &&
                fragment.find("const float metallic = saturate(input.surfaceParameters.x);") != std::string::npos &&
                fragment.find("const float roughness = clamp(input.surfaceParameters.y") != std::string::npos,
            "Sprite lit/unlit, metallic, and roughness surface controls are missing.");
    require(fragment.find("shadow_visibility(input.worldPosition, normal)") != std::string::npos &&
                fragment.find("local_shadow_visibility(light, input.worldPosition, normal, lightToSurface)") != std::string::npos,
            "Sprite direct lighting must use both directional and local shadow visibility.");
    require(block_after(fragment, "struct LocalLightData", "sprite local-light ABI") ==
                block_after(scene_lit, "struct LocalLightData", "scene_lit local-light ABI") &&
                block_after(fragment, "cbuffer LightingData", "sprite lighting cbuffer") ==
                block_after(scene_lit, "cbuffer LightingData", "scene_lit lighting cbuffer"),
            "Sprite light data and LightingData blocks must remain byte-compatible with scene_lit.");
    require(fragment.find("float4 indirectLighting : SV_Target5;") != std::string::npos &&
                fragment.find("output.indirectLighting = float4(indirect, 1.0f);") != std::string::npos &&
                fragment.find("float depth : SV_Depth;") != std::string::npos,
            "Sprite must preserve the indirect-light and per-pixel depth outputs.");

    const auto sprite_fragment = manifest.find("\"stem\": \"sprite.frag\"");
    require(sprite_fragment != std::string::npos, "Shader artifact source contract is missing sprite.frag.");
    const auto sprite_fragment_end = manifest.find("\"stem\":", sprite_fragment + 1U);
    const auto sprite_fragment_entry = manifest.substr(sprite_fragment,
        sprite_fragment_end == std::string::npos ? std::string::npos : sprite_fragment_end - sprite_fragment);
    require(sprite_fragment_entry.find("\"samplers\": 6") != std::string::npos &&
                sprite_fragment_entry.find("\"uniformBuffers\": 1") != std::string::npos &&
                sprite_fragment_entry.find("\"storageBuffers\": 3") != std::string::npos,
            "sprite.frag source contract must declare six samplers, one cbuffer, and three storage buffers.");
    require(manifest.find("\"stem\": \"sprite_shadow.vert\"") != std::string::npos,
            "Shader artifact source contract is missing sprite_shadow.vert.");
}

void test_vfx_billboard_hybrid_pixel_contract(const std::filesystem::path& repository_root) {
    const auto source = read_text(repository_root / "assets" / "shaders" / "vfx_billboard.vert.hlsl");
    const auto fragment = read_text(repository_root / "assets" / "shaders" / "vfx_billboard.frag.hlsl");
    const auto group = read_text(repository_root / "assets" / "shaders" / "vfx_group.comp.hlsl");
    const auto camera = block_after(source, "cbuffer VfxCamera", "VFX camera cbuffer");
    require(camera.find("float4x4 viewProjection;") != std::string::npos &&
                camera.find("float4 cameraRight;") != std::string::npos &&
                camera.find("float4 cameraUp;") != std::string::npos &&
                camera.find("float deltaSeconds;") != std::string::npos &&
                camera.find("float3 padding;") != std::string::npos,
            "VFX camera cbuffer must preserve its existing fields.");
    const auto render_size = camera.find("float2 renderSize;");
    const auto world_units = camera.find("float worldUnitsPerPixel;");
    const auto flags = camera.find("uint hybridPixelFlags;");
    require(render_size != std::string::npos && world_units != std::string::npos &&
                flags != std::string::npos && render_size < world_units && world_units < flags,
            "VFX camera cbuffer must append the 16-byte Hybrid Pixel payload in order.");
    require(source.find("StructuredBuffer<ParticleState> particles : register(t0, space0);") != std::string::npos &&
                source.find("StructuredBuffer<uint> compactAliveIndices : register(t1, space0);") != std::string::npos,
            "VFX billboard storage-buffer ABI must remain unchanged.");

    const auto snap_body = block_after(source, "float4 snap_center_to_virtual_pixel", "VFX center snap helper");
    require(snap_body.find("finite_render_size()") != std::string::npos &&
                snap_body.find("floor(virtualPixel) + 0.5f") != std::string::npos &&
                snap_body.find("centerClip.xy = snappedNdc * centerClip.w") != std::string::npos,
            "Hybrid Pixel center snapping must use finite-safe NDC pixel-center quantization.");
    const auto size_body = block_after(source, "float quantize_billboard_size", "VFX size quantization helper");
    require(size_body.find("worldUnitsPerPixel") != std::string::npos &&
                size_body.find("floor(pixelCount + 0.5f)") != std::string::npos,
            "Hybrid Pixel billboard size must quantize against worldUnitsPerPixel.");

    const auto main_body = block_after(source, "VertexOutput main", "VFX billboard main");
    require(main_body.find("(hybridPixelFlags & 1u) != 0u") != std::string::npos &&
                main_body.find("(hybridPixelFlags & 2u) != 0u") != std::string::npos &&
                main_body.find("(particle.renderMetadata.x & 2u) != 0u") != std::string::npos &&
                main_body.find("(particle.renderMetadata.x & 4u) != 0u") != std::string::npos &&
                main_body.find("if (!snapCenter && !quantizeSize)") != std::string::npos,
            "VFX billboard must combine profile and graph policy flags while preserving the ordinary path.");
    require(main_body.find("snap_center_to_virtual_pixel(currentCenterClip)") != std::string::npos &&
                main_body.find("snap_center_to_virtual_pixel(previousCenterClip)") != std::string::npos &&
                main_body.find("const float4 offsetClip = mul(viewProjection, float4(offset, 0.0f));") != std::string::npos,
            "Current and previous billboard centers must share the clip-space Hybrid Pixel alignment.");
    require(main_body.find("const float2 currentUv = currentClip.xy") != std::string::npos &&
                main_body.find("const float2 previousUv = previousClip.xy") != std::string::npos &&
                main_body.find("output.motion = currentUv - previousUv;") != std::string::npos,
            "VFX motion vectors must be derived from the aligned current and previous clips.");
    require(main_body.find("(hybridPixelFlags & 4u) != 0u") != std::string::npos &&
                main_body.find("(particle.renderMetadata.x & 8u) != 0u") != std::string::npos &&
                fragment.find("input.profileSampling != 0u") != std::string::npos,
            "VFX profile sampling must cross the billboard ABI into deterministic coverage.");
    require(group.find("(particles[particleIndex].renderMetadata.x & 1u) != 0u") != std::string::npos,
            "VFX blend grouping must mask its bit after render-policy metadata is appended.");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path repository_root = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::current_path();
        const auto shader_path = repository_root / "assets" / "shaders" / "scene_gpu_driven.vert.hlsl";
        const auto source = read_text(shader_path);
        test_metadata_semantics(source);
        test_storage_read_guards(source);
        test_deterministic_clip_output(source);
        test_sprite_mixed_lighting_contract(repository_root);
        test_vfx_billboard_hybrid_pixel_contract(repository_root);
    } catch (const std::exception& error) {
        std::cerr << "gpu_driven_shader_contract_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gpu_driven_shader_contract_tests: ok\n";
    return 0;
}
