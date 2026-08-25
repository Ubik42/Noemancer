#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "Unable to read contract source: " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::size_t matching_brace(const std::string& source, const std::size_t opening) {
    require(opening < source.size() && source[opening] == '{',
        "Expected an opening brace in contract source.");
    std::size_t depth = 0U;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = opening; index < source.size(); ++index) {
        const char character = source[index];
        if (in_string) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') in_string = false;
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{') {
            ++depth;
        } else if (character == '}') {
            require(depth > 0U, "Contract source has an unmatched closing brace.");
            --depth;
            if (depth == 0U) return index;
        }
    }
    throw std::runtime_error("Contract source has an unterminated brace block.");
}

std::string block_after(const std::string& source, const std::string_view marker,
                        const std::string_view context) {
    const auto marker_position = source.find(marker);
    require(marker_position != std::string::npos,
        "Missing " + std::string(context) + " marker: " + std::string(marker));
    const auto opening = source.find('{', marker_position + marker.size());
    require(opening != std::string::npos,
        "Missing opening brace for " + std::string(context) + ".");
    const auto closing = matching_brace(source, opening);
    return source.substr(opening, closing - opening + 1U);
}

void require_all(const std::string& source,
                 const std::initializer_list<std::string_view> needles,
                 const std::string_view context) {
    for (const auto needle : needles)
        require(source.find(needle) != std::string::npos,
            std::string(context) + " is missing: " + std::string(needle));
}

const Json& shader_contract(const Json& manifest, const std::string_view stem) {
    require(manifest.contains("shaders") && manifest.at("shaders").is_array(),
        "Shader contract must contain a shaders array.");
    for (const auto& shader : manifest.at("shaders")) {
        if (shader.value("stem", std::string{}) == stem) return shader;
    }
    throw std::runtime_error("Shader contract is missing: " + std::string(stem));
}

void test_renderer_and_graph_versions(const std::filesystem::path& root) {
    const auto renderer = read_text(root / "src" / "runtime" / "scene_renderer.cpp");
    const auto graph = read_text(root / "src" / "engine" / "render_graph.cpp");
    const auto graph_factory = block_after(graph, "CompiledRenderGraph make_forward_render_graph()",
        "forward render graph factory");

    require(renderer.find("noemancer.renderer-status.v29") != std::string::npos,
        "Renderer status contract must remain v29.");
    require(graph_factory.find("render.graph.forward.v17") != std::string::npos,
        "Forward render graph identity must remain v17.");
    require_all(graph_factory, {
        "render.resource.scene-depth-pyramid",
        "render.pass.depth-pyramid-seed",
        "render.pass.depth-pyramid-reduce",
        "render.pass.gpu-visibility"},
        "v17 render graph occlusion closure");
}

void test_occlusion_runtime_status(const std::filesystem::path& root) {
    const auto renderer = read_text(root / "src" / "runtime" / "scene_renderer.cpp");
    const auto status = block_after(renderer, "std::string SceneRenderer::status_json()",
        "renderer status function");

    require(status.find("noemancer.gpu-occlusion-runtime/0.1") != std::string::npos,
        "Renderer status must publish the GPU occlusion runtime v0.1 schema.");
    require_all(status, {
        "requested",
        "available",
        "usedThisFrame",
        "historyValid",
        "fallbackReason",
        "statistics",
        "candidates",
        "frustumCulled",
        "hizTested",
        "hizCulled",
        "uncertainVisible",
        "offscreenVisible",
        "disabledVisible",
        "acceptedVisible"},
        "GPU occlusion runtime status");
}

void test_occlusion_runtime_policy_and_fallbacks(const std::filesystem::path& root) {
    const auto renderer = read_text(root / "src" / "runtime" / "scene_renderer.cpp");
    const auto frame_setup = block_after(renderer, "void SceneRenderer::render(",
        "renderer frame function");

    // The occlusion path is explicitly opt-in and only becomes history-valid
    // when the previous frame, depth pyramid, pipeline, camera mode and
    // unjittered projection all satisfy the conservative contract.
    require_all(frame_setup, {
        "gpu_occlusion_enabled_&&gpu_occlusion_pipeline_&&",
        "gpu_occlusion_statistics_buffer_&&depth_pyramid_texture_",
        "!camera_orthographic&&!camera_cut",
        "frame_index_>0U&&previous_temporal_frame_+1U==frame_index_",
        "unjittered_projection_delta<=0.000001F",
        "if(!gpu_occlusion_enabled_)gpu_occlusion_fallback_reason_=\"disabled-by-policy\"",
        "pipeline-or-statistics-unavailable",
        "shared-hiz-unavailable",
        "orthographic-conservative-fallback",
        "camera-cut",
        "history-unavailable",
        "camera-motion-conservative-fallback"},
        "GPU occlusion conservative policy");

    const auto pass = block_after(renderer, "pass_id==\"render.pass.gpu-visibility\"",
        "GPU visibility pass");
    require_all(pass, {
        "const bool use_occlusion=gpu_occlusion_history_valid_&&gpu_occlusion_pipeline_&&",
        "gpu_occlusion_statistics_buffer_&&depth_pyramid_texture_",
        "SDL_BeginGPUComputePass(command,nullptr,0,outputs.data(),use_occlusion?3U:2U)",
        "SDL_PushGPUComputeUniformData(command,0,&gpu_occlusion_parameters,sizeof(gpu_occlusion_parameters))",
        "SDL_BindGPUComputePipeline(compute,gpu_occlusion_pipeline_)",
        "SDL_BindGPUComputePipeline(compute,gpu_visibility_pipeline_)"},
        "GPU visibility occlusion opt-in branch");

    const auto statistics_reset = renderer.find(
        "destination={gpu_occlusion_statistics_buffer_,0,gpu_occlusion_statistics_bytes};");
    const auto in_place_upload = renderer.find(
        "SDL_UploadToGPUBuffer(copy,&source,&destination,false);", statistics_reset);
    require(statistics_reset != std::string::npos && in_place_upload != std::string::npos &&
            in_place_upload - statistics_reset < 1024U,
        "GPU occlusion statistics must reset in-place before compute so Vulkan cannot accumulate a cycled backing buffer.");
}

void test_readback_contract(const std::filesystem::path& root) {
    const auto renderer = read_text(root / "src" / "runtime" / "scene_renderer.cpp");
    require(renderer.find("noemancer.gpu-visibility-readback/0.3") != std::string::npos,
        "GPU visibility readback ABI must remain v0.3.");

    const auto enqueue = block_after(renderer, "bool SceneRenderer::enqueue_gpu_visibility_readback(",
        "visibility readback enqueue");
    require_all(enqueue, {
        "gpu_visibility_readback_state_=\"queued\"",
        "gpu_visibility_readback_occlusion_active_=statistics_bytes>0U",
        "SDL_DownloadFromGPUBuffer(copy,&command_source",
        "SDL_DownloadFromGPUBuffer(copy,&index_source",
        "statistics_source"},
        "visibility readback enqueue contract");

    const auto resolve = block_after(renderer, "bool SceneRenderer::resolve_gpu_visibility_readback()",
        "visibility readback resolve");
    require_all(resolve, {
        "gpu_visibility_readback_occlusion_active_",
        "visible<=gpu_visibility_readback_cpu_visible_",
        "gpu_visibility_readback_conservative_subset_match_=",
        "gpu_visibility_readback_unexpected_visible_",
        "gpu_visibility_readback_count_match_&&gpu_visibility_readback_conservative_subset_match_",
        "gpu_visibility_readback_state_=\"complete\"",
        "SDL_ReleaseGPUTransferBuffer(device_,gpu_visibility_readback_transfer_)"},
        "visibility readback conservative-subset contract");
}

void test_shader_manifest_and_source_abi(const std::filesystem::path& root) {
    const auto manifest = Json::parse(read_text(root / "assets" / "shaders" /
        "shader-artifact-contract.json"));
    const auto& shader = shader_contract(manifest, "gpu_occlusion.comp");
    require(shader.value("stage", std::string{}) == "compute" &&
                shader.value("profile", std::string{}) == "cs_6_0" &&
                shader.value("entrypoint", std::string{}) == "main",
        "GPU occlusion shader stage/profile/entrypoint contract drifted.");
    const auto& resources = shader.at("resources");
    require(resources.value("samplers", 0U) == 1U &&
                resources.value("uniformBuffers", 0U) == 1U &&
                resources.value("storageBuffers", 0U) == 0U &&
                resources.value("readonlyStorageBuffers", 0U) == 2U &&
                resources.value("readwriteStorageBuffers", 0U) == 3U,
        "GPU occlusion shader resource ABI must be 1 sampler, 1 UBO, 2 RO and 3 RW buffers.");
    require(shader.at("threadGroup") == Json::array({64, 1, 1}),
        "GPU occlusion shader thread group must remain 64x1x1.");

    const auto source = read_text(root / "assets" / "shaders" / "gpu_occlusion.comp.hlsl");
    require_all(source, {
        "Texture2D<float2> depthPyramid : register(t0, space0);",
        "SamplerState depthPyramidSampler : register(s0, space0);",
        "StructuredBuffer<GpuDrivenInstance> instances : register(t1, space0);",
        "StructuredBuffer<GpuDrivenBatch> batches : register(t2, space0);",
        "RWStructuredBuffer<uint> visibilityStats : register(u2, space1);",
        "cbuffer GpuOcclusionParameters : register(b0, space2)",
        "float4 frustumPlanes[6];",
        "float4x4 viewProjection;",
        "float4 viewport;",
        "float4 depthParameters;",
        "float4 occlusionParameters;",
        "uint4 dispatchParameters;",
        "[numthreads(64, 1, 1)]",
        "depthPyramid.Load(int3(coordinate, mip))",
        "maximumOccluderDepth = max(maximumOccluderDepth, sampleMaximum)",
        "sphere.nearestDepth > maximumOccluderDepth + bias",
        "PROJECTED_OFFSCREEN",
        "PROJECTED_UNCERTAIN",
        "STAT_HIZ_TESTED",
        "STAT_HIZ_CULLED"},
        "GPU occlusion shader ABI and safety invariants");
}

void test_resource_release_contract(const std::filesystem::path& root) {
    const auto renderer = read_text(root / "src" / "runtime" / "scene_renderer.cpp");
    const auto release = block_after(renderer, "void SceneRenderer::release()",
        "renderer resource release");
    require_all(release, {
        "SDL_ReleaseGPUComputePipeline(device_,gpu_occlusion_pipeline_)",
        "SDL_ReleaseGPUBuffer(device_,gpu_occlusion_statistics_buffer_)",
        "SDL_ReleaseGPUTransferBuffer(device_,gpu_visibility_readback_transfer_)",
        "SDL_ReleaseGPUFence(device_,gpu_visibility_readback_fence_)",
        "gpu_occlusion_pipeline_=nullptr",
        "gpu_occlusion_statistics_buffer_=nullptr",
        "gpu_visibility_readback_transfer_=nullptr",
        "gpu_visibility_readback_fence_=nullptr"},
        "GPU occlusion/readback resource release");

    const auto creation = block_after(renderer, "bool SceneRenderer::create_gpu_driven_resources()",
        "GPU-driven resource creation");
    require_all(creation, {
        "gpu_occlusion_pipeline_=load_occlusion_compute_pipeline(device_)",
        "statistics-buffer-unavailable",
        "SDL_ReleaseGPUComputePipeline(device_,gpu_occlusion_pipeline_)",
        "SDL_ReleaseGPUBuffer(device_,gpu_occlusion_statistics_buffer_)"},
        "GPU occlusion allocation fallback");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path repository_root = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::current_path();
        test_renderer_and_graph_versions(repository_root);
        test_occlusion_runtime_status(repository_root);
        test_occlusion_runtime_policy_and_fallbacks(repository_root);
        test_readback_contract(repository_root);
        test_shader_manifest_and_source_abi(repository_root);
        test_resource_release_contract(repository_root);
    } catch (const std::exception& error) {
        std::cerr << "gpu_occlusion_runtime_contract_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gpu_occlusion_runtime_contract_tests: ok\n";
    return 0;
}
