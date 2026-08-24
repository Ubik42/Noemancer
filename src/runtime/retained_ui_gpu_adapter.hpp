#pragma once
#include "engine/retained_ui_runtime.hpp"
#include "runtime/texture_resource_table.hpp"
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
namespace noemancer {
class RetainedUiGpuAdapter final {
public:
    RetainedUiGpuAdapter(SDL_GPUDevice* device,TextureResourceTable& texture_resources,std::string owner_id)
        :device_(device),texture_resources_(texture_resources),owner_id_(std::move(owner_id)){}
    ~RetainedUiGpuAdapter();
    [[nodiscard]] bool initialize(SDL_GPUTextureFormat target_format);
    void shutdown(){release();}
    [[nodiscard]] bool upload(SDL_GPUCommandBuffer* command,const RetainedUiRenderPacket& packet);
    void commit_uploads();
    void rollback_uploads();
    void render(SDL_GPUCommandBuffer* command,SDL_GPUTexture* target,std::uint32_t width,std::uint32_t height,
                bool clear_target=false);
    [[nodiscard]] std::string status_json() const;
    [[nodiscard]] const std::string& last_error() const noexcept{return last_error_;}
private:
    struct TextureResource final {
        TextureResourceHandle handle;
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint64_t revision{};
    };
    struct PendingTextureUpdate final {
        std::uint64_t id{};
        TextureResourceHandle handle;
        SDL_GPUTexture* texture{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint64_t revision{};
        bool created{};
    };
    void release();
    [[nodiscard]] bool upload_texture(SDL_GPUCopyPass* pass, const RetainedUiTexture& source);
    SDL_GPUDevice* device_{};TextureResourceTable& texture_resources_;std::string owner_id_;
    SDL_GPUGraphicsPipeline* pipeline_{}; SDL_GPUBuffer* vertices_{}; SDL_GPUBuffer* indices_{};
    SDL_GPUTransferBuffer* transfer_{}; SDL_GPUSampler* sampler_{}; TextureResourceHandle white_texture_{};
    std::unordered_map<std::uint64_t,TextureResource> textures_;
    std::vector<PendingTextureUpdate> pending_updates_;
    std::vector<std::uint64_t> pending_removals_;
    RetainedUiRenderPacket packet_; std::string last_error_; std::uint64_t uploads_{}; std::uint64_t texture_uploads_{}; std::uint64_t draws_{};
    static constexpr std::uint32_t vertex_capacity=1U<<20U,index_capacity=1U<<19U;
};
}
