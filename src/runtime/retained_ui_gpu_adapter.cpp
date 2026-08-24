#include "runtime/retained_ui_gpu_adapter.hpp"
#include "runtime/shader_artifact_contract.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace noemancer {
namespace {

thread_local std::string shader_artifact_failure;

SDL_GPUShader* load_shader(SDL_GPUDevice* device, const char* stem, const SDL_GPUShaderStage stage,
                           const Uint32 uniforms, const Uint32 samplers) {
    const auto formats = SDL_GetGPUShaderFormats(device);
    const bool dxil = (formats & SDL_GPU_SHADERFORMAT_DXIL) != 0;
    static const ShaderArtifactContract artifacts(
        default_shader_artifact_root()/"shader-artifact-manifest.json");
    const auto artifact=artifacts.load(ShaderArtifactRequest{
        .stem=stem,
        .stage=stage==SDL_GPU_SHADERSTAGE_VERTEX?ShaderArtifactStage::vertex:ShaderArtifactStage::fragment,
        .resources={.uniform_buffers=uniforms,.samplers=samplers}},
        dxil?ShaderArtifactBackend::dxil:ShaderArtifactBackend::spv);
    if(!artifact.success) {
        shader_artifact_failure=artifact.code+": "+artifact.detail;
        return nullptr;
    }
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(artifact.bytes.data());
    info.code_size = artifact.bytes.size();
    info.entrypoint = artifact.entrypoint.c_str();
    info.format = dxil ? SDL_GPU_SHADERFORMAT_DXIL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_uniform_buffers = uniforms;
    info.num_samplers = samplers;
    return SDL_CreateGPUShader(device, &info);
}

} // namespace

RetainedUiGpuAdapter::~RetainedUiGpuAdapter() { release(); }

bool RetainedUiGpuAdapter::initialize(const SDL_GPUTextureFormat format) {
    shader_artifact_failure.clear();
    auto* vertex_shader = load_shader(device_, "retained_ui.vert", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    auto* fragment_shader = load_shader(device_, "retained_ui.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!vertex_shader || !fragment_shader) {
        last_error_ = "Unable to load retained UI shaders";
        if(!shader_artifact_failure.empty())last_error_+="; "+shader_artifact_failure;
        if (vertex_shader) SDL_ReleaseGPUShader(device_, vertex_shader);
        if (fragment_shader) SDL_ReleaseGPUShader(device_, fragment_shader);
        return false;
    }

    SDL_GPUVertexBufferDescription buffer{0, sizeof(RetainedUiVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    std::array<SDL_GPUVertexAttribute, 3> attributes{{
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 8},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12}}};
    SDL_GPUColorTargetDescription color{};
    color.format = format;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.enable_blend = true;
    SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.vertex_shader = vertex_shader;
    pipeline_info.fragment_shader = fragment_shader;
    pipeline_info.vertex_input_state = {&buffer, 1, attributes.data(), 3};
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeline_info.target_info = {&color, 1, SDL_GPU_TEXTUREFORMAT_INVALID, false, 0, 0, 0};
    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline_info);
    SDL_ReleaseGPUShader(device_, vertex_shader);
    SDL_ReleaseGPUShader(device_, fragment_shader);

    SDL_GPUBufferCreateInfo vertex_info{SDL_GPU_BUFFERUSAGE_VERTEX, vertex_capacity, 0};
    SDL_GPUBufferCreateInfo index_info{SDL_GPU_BUFFERUSAGE_INDEX, index_capacity, 0};
    vertices_ = pipeline_ ? SDL_CreateGPUBuffer(device_, &vertex_info) : nullptr;
    indices_ = vertices_ ? SDL_CreateGPUBuffer(device_, &index_info) : nullptr;
    SDL_GPUTransferBufferCreateInfo transfer_info{
        SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vertex_capacity + index_capacity, 0};
    transfer_ = indices_ ? SDL_CreateGPUTransferBuffer(device_, &transfer_info) : nullptr;

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = transfer_ ? SDL_CreateGPUSampler(device_, &sampler_info) : nullptr;
    if (!sampler_) {
        last_error_ = SDL_GetError();
        release();
        return false;
    }

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* pass = command ? SDL_BeginGPUCopyPass(command) : nullptr;
    const RetainedUiTexture white{0, 1, 1, 1, {255, 255, 255, 255}};
    if (!pass || !upload_texture(pass, white)) {
        last_error_ = last_error_.empty() ? SDL_GetError() : last_error_;
        if (pass) SDL_EndGPUCopyPass(pass);
        if (command) SDL_CancelGPUCommandBuffer(command);
        release();
        return false;
    }
    SDL_EndGPUCopyPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(command)) {
        last_error_ = SDL_GetError();
        rollback_uploads();
        release();
        return false;
    }
    commit_uploads();
    white_texture_ = textures_.at(0).handle;
    return true;
}

bool RetainedUiGpuAdapter::upload_texture(SDL_GPUCopyPass* pass, const RetainedUiTexture& source) {
    const auto byte_count = static_cast<std::size_t>(source.width) * source.height * 4U;
    if (!pass || source.width == 0 || source.height == 0 || source.rgba8.size() != byte_count) {
        last_error_ = "Retained UI texture payload is not tightly packed RGBA8";
        return false;
    }
    auto found = textures_.find(source.id);
    if (found != textures_.end() && found->second.width == source.width &&
        found->second.height == source.height && found->second.revision == source.revision) return true;
    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.width = source.width;
    texture_info.height = source.height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    auto* texture = SDL_CreateGPUTexture(device_, &texture_info);
    SDL_GPUTransferBufferCreateInfo transfer_info{
        SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, static_cast<Uint32>(byte_count), 0};
    auto* texture_transfer = texture ? SDL_CreateGPUTransferBuffer(device_, &transfer_info) : nullptr;
    auto* mapped = texture_transfer ? SDL_MapGPUTransferBuffer(device_, texture_transfer, false) : nullptr;
    if (!mapped) {
        last_error_ = SDL_GetError();
        if (texture_transfer) SDL_ReleaseGPUTransferBuffer(device_, texture_transfer);
        if (texture) SDL_ReleaseGPUTexture(device_, texture);
        return false;
    }
    std::memcpy(mapped, source.rgba8.data(), byte_count);
    SDL_UnmapGPUTransferBuffer(device_, texture_transfer);
    SDL_GPUTextureTransferInfo source_info{};
    source_info.transfer_buffer = texture_transfer;
    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.w = source.width;
    destination.h = source.height;
    destination.d = 1;
    SDL_UploadToGPUTexture(pass, &source_info, &destination, false);
    SDL_ReleaseGPUTransferBuffer(device_, texture_transfer);
    const TextureResourceMetadata metadata{source.width,source.height,1U,0U,byte_count};
    TextureResourceHandle handle;
    bool created=false;
    if(found==textures_.end()) {
        handle=texture_resources_.acquire({.stable_id=owner_id_+"/texture/"+std::to_string(source.id),
            .semantic="ui-rgba",.owner=owner_id_,.source="retained-ui-generated",.residency="resident",
            .metadata=metadata},texture);
        created=true;
    } else {
        handle=found->second.handle;
        if(!texture_resources_.stage_replacement(handle,texture,metadata))handle={};
    }
    if(!handle.valid()) {
        SDL_ReleaseGPUTexture(device_,texture);
        last_error_="Unable to stage retained UI texture resource";
        return false;
    }
    if(created)textures_.emplace(source.id,TextureResource{handle,source.width,source.height,source.revision});
    pending_updates_.push_back({source.id,handle,texture,source.width,source.height,source.revision,created});
    ++texture_uploads_;
    return true;
}

bool RetainedUiGpuAdapter::upload(SDL_GPUCommandBuffer* command, const RetainedUiRenderPacket& packet) {
    const auto vertex_bytes = packet.vertices.size() * sizeof(RetainedUiVertex);
    const auto index_bytes = packet.indices.size() * sizeof(std::uint32_t);
    if (!command || vertex_bytes > vertex_capacity || index_bytes > index_capacity) {
        last_error_ = "Retained UI packet exceeds bounded GPU upload capacity";
        return false;
    }
    auto* mapped = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
    if (!mapped) {
        last_error_ = SDL_GetError();
        return false;
    }
    if (vertex_bytes) std::memcpy(mapped, packet.vertices.data(), vertex_bytes);
    if (index_bytes) std::memcpy(mapped + vertex_capacity, packet.indices.data(), index_bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer_);
    auto* pass = SDL_BeginGPUCopyPass(command);
    if (!pass) {
        last_error_ = SDL_GetError();
        return false;
    }
    if (vertex_bytes) {
        SDL_GPUTransferBufferLocation source{transfer_, 0};
        SDL_GPUBufferRegion destination{vertices_, 0, static_cast<Uint32>(vertex_bytes)};
        SDL_UploadToGPUBuffer(pass, &source, &destination, true);
    }
    if (index_bytes) {
        SDL_GPUTransferBufferLocation source{transfer_, vertex_capacity};
        SDL_GPUBufferRegion destination{indices_, 0, static_cast<Uint32>(index_bytes)};
        SDL_UploadToGPUBuffer(pass, &source, &destination, true);
    }
    for (const auto& texture : packet.textures) {
        if (!upload_texture(pass, texture)) {
            SDL_EndGPUCopyPass(pass);
            return false;
        }
    }
    SDL_EndGPUCopyPass(pass);

    std::unordered_set<std::uint64_t> live_ids{0};
    for (const auto& texture : packet.textures) live_ids.insert(texture.id);
    pending_removals_.clear();
    for (const auto& [id,resource] : textures_)
        if(!live_ids.contains(id)&&id!=0U)pending_removals_.push_back(id);
    packet_ = packet;
    ++uploads_;
    last_error_.clear();
    return true;
}

void RetainedUiGpuAdapter::commit_uploads() {
    for(const auto& update:pending_updates_) {
        if(!update.created) {
            if(auto* previous=texture_resources_.commit_replacement(update.handle))
                SDL_ReleaseGPUTexture(device_,previous);
            if(auto found=textures_.find(update.id);found!=textures_.end()) {
                found->second.width=update.width;found->second.height=update.height;found->second.revision=update.revision;
            }
        }
    }
    for(const auto id:pending_removals_)if(auto found=textures_.find(id);found!=textures_.end()) {
        if(auto* texture=texture_resources_.remove(found->second.handle))SDL_ReleaseGPUTexture(device_,texture);
        textures_.erase(found);
    }
    pending_updates_.clear();pending_removals_.clear();
}

void RetainedUiGpuAdapter::rollback_uploads() {
    for(auto cursor=pending_updates_.rbegin();cursor!=pending_updates_.rend();++cursor) {
        if(cursor->created) {
            if(auto found=textures_.find(cursor->id);found!=textures_.end())textures_.erase(found);
            if(auto* texture=texture_resources_.remove(cursor->handle))SDL_ReleaseGPUTexture(device_,texture);
        } else {
            if(auto* rejected=texture_resources_.rollback_replacement(cursor->handle))SDL_ReleaseGPUTexture(device_,rejected);
        }
    }
    pending_updates_.clear();pending_removals_.clear();
}

void RetainedUiGpuAdapter::render(SDL_GPUCommandBuffer* command, SDL_GPUTexture* target,
                                  const std::uint32_t width, const std::uint32_t height,const bool clear_target) {
    if (!command || !target || packet_.draws.empty()) return;
    const std::array<float, 4> viewport{static_cast<float>(width), static_cast<float>(height), 0, 0};
    SDL_PushGPUVertexUniformData(command, 0, viewport.data(), sizeof(viewport));
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color=SDL_FColor{0.035F,0.043F,0.058F,1.0F};
    color.load_op = clear_target?SDL_GPU_LOADOP_CLEAR:SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    auto* pass = SDL_BeginGPURenderPass(command, &color, 1, nullptr);
    if (!pass) {
        last_error_ = SDL_GetError();
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUBufferBinding vertex_binding{vertices_, 0};
    SDL_GPUBufferBinding index_binding{indices_, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    for (const auto& draw : packet_.draws) {
        const auto found = textures_.find(draw.texture_id);
        auto* texture=found == textures_.end()?texture_resources_.resolve(white_texture_):texture_resources_.resolve(found->second.handle);
        SDL_GPUTextureSamplerBinding texture_binding{
            texture ? texture : texture_resources_.resolve(white_texture_), sampler_};
        SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
        if (draw.scissor_enabled) {
            SDL_Rect rect{std::max(0, draw.scissor[0]), std::max(0, draw.scissor[1]),
                          std::max(0, draw.scissor[2]), std::max(0, draw.scissor[3])};
            SDL_SetGPUScissor(pass, &rect);
        } else {
            SDL_Rect rect{0, 0, static_cast<int>(width), static_cast<int>(height)};
            SDL_SetGPUScissor(pass, &rect);
        }
        SDL_DrawGPUIndexedPrimitives(pass, draw.index_count, 1, draw.first_index, 0, 0);
        ++draws_;
    }
    SDL_EndGPURenderPass(pass);
}

std::string RetainedUiGpuAdapter::status_json() const {
    return nlohmann::json{{"schemaVersion", "noemancer.retained-ui-gpu/0.3"},
        {"pipelineCreated", pipeline_ != nullptr}, {"uploads", uploads_},
        {"textureUploads", texture_uploads_}, {"residentTextureCount", textures_.size()},
        {"draws", draws_}, {"vertexCount", packet_.vertices.size()},
        {"indexCount", packet_.indices.size()}, {"drawCount", packet_.draws.size()},
        {"scissorSupported", true}, {"alphaBlend", "straight-alpha"},
        {"textureSampling", true},{"textureResources",nlohmann::json::parse(texture_resources_.observe_json(owner_id_))}}.dump();
}

void RetainedUiGpuAdapter::release() {
    rollback_uploads();
    for (const auto& [id, resource] : textures_) {
        static_cast<void>(id);
        if(auto* texture=texture_resources_.remove(resource.handle))SDL_ReleaseGPUTexture(device_,texture);
    }
    textures_.clear();
    white_texture_ = {};
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    if (vertices_) SDL_ReleaseGPUBuffer(device_, vertices_);
    if (indices_) SDL_ReleaseGPUBuffer(device_, indices_);
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    sampler_ = nullptr;
    transfer_ = nullptr;
    vertices_ = nullptr;
    indices_ = nullptr;
    pipeline_ = nullptr;
}

} // namespace noemancer
