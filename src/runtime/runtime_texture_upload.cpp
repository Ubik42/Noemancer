#include "runtime_texture_upload.hpp"

#include "engine/ktx2_cook_adapter.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace noemancer {
namespace {

constexpr std::uint64_t align_up(const std::uint64_t value,const std::uint64_t alignment) {
    return (value+alignment-1U)/alignment*alignment;
}

RuntimeTextureStream failure(std::string code,std::string detail) {
    RuntimeTextureStream result;
    result.code=std::move(code);result.detail=std::move(detail);return result;
}

std::uint64_t resident_bytes_from(const RuntimeTextureStream& stream,const std::uint32_t first) {
    std::uint64_t result{};
    for(std::uint32_t level=first;level<stream.level_count;++level)result+=stream.levels[level].source_bytes;
    return result;
}

SDL_GPUTexture* create_resident_texture(SDL_GPUDevice* device,const RuntimeTextureStream& stream,
    const std::uint32_t resident_mip_start) {
    if(resident_mip_start>=stream.level_count)return nullptr;
    SDL_GPUTextureCreateInfo info{};info.type=SDL_GPU_TEXTURETYPE_2D;info.format=stream.gpu_format;
    info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;info.width=stream.levels[resident_mip_start].width;
    info.height=stream.levels[resident_mip_start].height;info.layer_count_or_depth=1U;
    info.num_levels=stream.level_count-resident_mip_start;info.sample_count=SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device,&info);
}

bool record_range(SDL_GPUCommandBuffer* command,const RuntimeTextureStream& stream,
    SDL_GPUTexture* destination_texture,const std::uint32_t first,const std::uint32_t end,
    const std::uint32_t destination_base_level) {
    if(first>=end)return true;
    auto* pass=SDL_BeginGPUCopyPass(command);if(!pass)return false;
    for(std::uint32_t cursor=end;cursor>first;--cursor) {
        const auto& layout=stream.levels[cursor-1U];
        const SDL_GPUTextureTransferInfo source{stream.transfer,static_cast<Uint32>(layout.offset),
            layout.pixels_per_row,layout.height};
        const SDL_GPUTextureRegion destination{destination_texture,destination_base_level+layout.level-first,
            0,0,0,0,layout.width,layout.height,1U};
        SDL_UploadToGPUTexture(pass,&source,&destination,false);
    }
    SDL_EndGPUCopyPass(pass);return true;
}

} // namespace

RuntimeTextureStream create_ktx2_texture_stream(SDL_GPUDevice* device,
    const std::span<const std::byte> payload,const bool srgb,const std::uint32_t initial_tail_levels) {
    if(!device)return failure("render.texture-device-missing","A GPU device is required for KTX2 upload.");

    const auto bc7_format=srgb?SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB:SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    const bool bc7_supported=SDL_GPUTextureSupportsFormat(
        device,bc7_format,SDL_GPU_TEXTURETYPE_2D,SDL_GPU_TEXTUREUSAGE_SAMPLER);
    auto chain=decode_ktx2_mip_chain(payload,bc7_supported
        ?RuntimeTextureFormat::bc7_rgba:RuntimeTextureFormat::rgba8);
    if(!chain.valid&&bc7_supported)chain=decode_ktx2_mip_chain(payload,RuntimeTextureFormat::rgba8);
    if(!chain.valid)return failure(chain.code,chain.detail);
    if(chain.levels.empty())return failure("render.texture-mips-empty","KTX2 Runtime decode produced no mip levels.");

    const bool compressed=chain.format==RuntimeTextureFormat::bc7_rgba;
    const auto gpu_format=compressed?bc7_format:
        (srgb?SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    if(!SDL_GPUTextureSupportsFormat(device,gpu_format,SDL_GPU_TEXTURETYPE_2D,SDL_GPU_TEXTUREUSAGE_SAMPLER))
        return failure("render.texture-format-unsupported","The selected Runtime texture format is not supported for sampling.");

    RuntimeTextureStream stream;
    stream.native_compressed=compressed;stream.format=runtime_texture_format_name(chain.format);
    stream.width=chain.width;stream.height=chain.height;stream.level_count=static_cast<std::uint32_t>(chain.levels.size());
    stream.source_bytes=chain.source_bytes;stream.resident_bytes=chain.upload_bytes;
    stream.levels.reserve(chain.levels.size());
    for(const auto& mip:chain.levels) {
        stream.staging_bytes=align_up(stream.staging_bytes,512U);
        const auto pixels_per_row=static_cast<std::uint32_t>(align_up(mip.width,64U));
        const auto row_count=compressed?(mip.height+3U)/4U:mip.height;
        const auto row_bytes=compressed?static_cast<std::uint64_t>(pixels_per_row/4U)*16U:
            static_cast<std::uint64_t>(pixels_per_row)*4U;
        stream.levels.push_back({mip.level,mip.width,mip.height,pixels_per_row,stream.staging_bytes,
            row_bytes,row_count,mip.bytes.size()});
        stream.staging_bytes+=row_bytes*row_count;
    }
    if(stream.staging_bytes==0U||stream.staging_bytes>std::numeric_limits<Uint32>::max())
        return failure("render.texture-staging-too-large","KTX2 upload exceeds the bounded SDL_GPU transfer size.");

    stream.gpu_format=gpu_format;

    const SDL_GPUTransferBufferCreateInfo transfer_info{
        SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,static_cast<Uint32>(stream.staging_bytes),0};
    stream.transfer=SDL_CreateGPUTransferBuffer(device,&transfer_info);
    if(!stream.transfer){const auto detail=std::string(SDL_GetError());release_texture_stream(device,stream);
        return failure("render.texture-transfer-create-failed",detail);}
    auto* mapped=static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device,stream.transfer,false));
    if(!mapped){const auto detail=std::string(SDL_GetError());release_texture_stream(device,stream);
        return failure("render.texture-transfer-map-failed",detail);}
    std::memset(mapped,0,static_cast<std::size_t>(stream.staging_bytes));
    for(std::size_t index=0;index<chain.levels.size();++index) {
        const auto& mip=chain.levels[index];const auto& layout=stream.levels[index];
        const auto source_row_bytes=compressed?static_cast<std::uint64_t>((mip.width+3U)/4U)*16U:
            static_cast<std::uint64_t>(mip.width)*4U;
        for(std::uint64_t row=0;row<layout.row_count;++row)
            std::memcpy(mapped+layout.offset+row*layout.row_bytes,
                mip.bytes.data()+row*source_row_bytes,static_cast<std::size_t>(source_row_bytes));
    }
    SDL_UnmapGPUTransferBuffer(device,stream.transfer);

    stream.tail_level_count=std::clamp(initial_tail_levels,1U,stream.level_count);
    stream.resident_mip_start=stream.level_count-stream.tail_level_count;
    stream.target_mip_start=0U;stream.maximum_mip_start=stream.resident_mip_start;
    stream.minimum_resident_level=stream.resident_mip_start;
    stream.next_detail_level=static_cast<std::int32_t>(stream.minimum_resident_level)-1;
    stream.full_chain_bytes=chain.upload_bytes;stream.resident_bytes=resident_bytes_from(stream,stream.resident_mip_start);
    stream.texture=create_resident_texture(device,stream,stream.resident_mip_start);
    if(!stream.texture){const auto detail=std::string(SDL_GetError());release_texture_stream(device,stream);
        return failure("render.texture-create-failed",detail);}
    auto* command=SDL_AcquireGPUCommandBuffer(device);
    if(!command){const auto detail=std::string(SDL_GetError());release_texture_stream(device,stream);
        return failure("render.texture-command-failed",detail);}
    if(!record_range(command,stream,stream.texture,stream.resident_mip_start,stream.level_count,0U)) {
        const auto detail=std::string(SDL_GetError());SDL_CancelGPUCommandBuffer(command);release_texture_stream(device,stream);
        return failure("render.texture-copy-pass-failed",detail);
    }
    if(!SDL_SubmitGPUCommandBuffer(command)) {const auto detail=std::string(SDL_GetError());release_texture_stream(device,stream);
        return failure("render.texture-submit-failed",detail);}

    stream.uploaded_level_count=stream.tail_level_count;
    for(std::uint32_t level=stream.minimum_resident_level;level<stream.level_count;++level) {
        stream.tail_bytes+=stream.levels[level].source_bytes;
        stream.uploaded_source_bytes+=stream.levels[level].source_bytes;
        stream.uploaded_copy_bytes+=stream.levels[level].copy_bytes();
    }
    stream.valid=true;stream.code="ok";
    stream.detail="KTX2 mip tail is visible; authored detail mips are staged for budgeted frame streaming.";
    return stream;
}

RuntimeTextureStreamStep record_texture_stream_detail(SDL_GPUDevice* device,SDL_GPUCommandBuffer* command,
    RuntimeTextureStream& stream) {
    return record_texture_stream_rebase(device,command,stream,
        stream.resident_mip_start>0U?stream.resident_mip_start-1U:0U);
}

RuntimeTextureStreamStep record_texture_stream_rebase(SDL_GPUDevice* device,SDL_GPUCommandBuffer* command,
    RuntimeTextureStream& stream,const std::uint32_t target_mip_start) {
    RuntimeTextureStreamStep result;result.resident_bytes_before=stream.resident_bytes;
    if(!command||!stream.valid||!stream.texture||!stream.transfer||target_mip_start>=stream.level_count) {
        result.code="render.texture-stream-invalid";result.detail="A valid command, stream, and mip target are required.";return result;
    }
    if(stream.transition_pending) {
        result.valid=false;result.code="render.texture-stream-transition-pending";
        result.detail="Commit or roll back the previous physical residency transition before recording another one.";
        return result;
    }
    result.valid=true;result.code="ok";result.texture=stream.texture;
    if(target_mip_start==stream.resident_mip_start){result.resident_bytes_after=stream.resident_bytes;return result;}
    if(target_mip_start+1U<stream.resident_mip_start||target_mip_start>stream.resident_mip_start+1U) {
        result.valid=false;result.code="render.texture-stream-step-unbounded";
        result.detail="Physical texture residency changes one mip tier per scheduler step.";return result;
    }
    if(!device){result.valid=false;result.code="render.texture-device-missing";
        result.detail="A GPU device is required to change physical texture residency.";return result;}
    auto* replacement=create_resident_texture(device,stream,target_mip_start);
    if(!replacement){result.valid=false;result.code="render.texture-rebase-create-failed";result.detail=SDL_GetError();return result;}
    // Populate the complete replacement tier from the persistent CPU staging
    // payload. SDL_GPU's D3D12 path rejects texture-to-texture copies between
    // differently based mip chains on some drivers; a bounded full-tier
    // upload is portable and also makes the replacement self-contained.
    if(!record_range(command,stream,replacement,target_mip_start,stream.level_count,0U)) {
        result.valid=false;result.code="render.texture-rebase-copy-pass-failed";result.detail=SDL_GetError();
        SDL_ReleaseGPUTexture(device,replacement);return result;
    }
    result.uploaded=true;result.level=target_mip_start;
    for(std::uint32_t level=target_mip_start;level<stream.level_count;++level) {
        result.source_bytes+=stream.levels[level].source_bytes;
        result.copy_bytes+=stream.levels[level].copy_bytes();
    }
    result.previous_texture=stream.texture;result.texture=replacement;

    // Recording is deliberately transactional. The replacement is now the
    // effective texture for commands recorded later in this frame, but the
    // previous texture remains owned by the stream until submission succeeds.
    stream.transition_pending=true;
    stream.transition_previous_texture=stream.texture;
    stream.transition_previous_resident_mip_start=stream.resident_mip_start;
    stream.transition_previous_minimum_resident_level=stream.minimum_resident_level;
    stream.transition_previous_next_detail_level=stream.next_detail_level;
    stream.transition_previous_uploaded_level_count=stream.uploaded_level_count;
    stream.transition_previous_resident_bytes=stream.resident_bytes;
    stream.transition_previous_uploaded_source_bytes=stream.uploaded_source_bytes;
    stream.transition_previous_uploaded_copy_bytes=stream.uploaded_copy_bytes;
    stream.texture=replacement;stream.resident_mip_start=target_mip_start;
    stream.minimum_resident_level=target_mip_start;
    stream.next_detail_level=target_mip_start>0U?static_cast<std::int32_t>(target_mip_start)-1:-1;
    stream.uploaded_level_count=stream.level_count-target_mip_start;
    stream.resident_bytes=resident_bytes_from(stream,target_mip_start);
    result.resident_bytes_after=stream.resident_bytes;
    if(result.uploaded){stream.uploaded_source_bytes+=result.source_bytes;stream.uploaded_copy_bytes+=result.copy_bytes;}
    return result;
}

void commit_texture_stream_transition(SDL_GPUDevice* device,RuntimeTextureStream& stream) {
    if(!device||!stream.transition_pending)return;
    const auto previous=stream.transition_previous_texture;
    const auto replacement=stream.texture;
    if(device&&previous!=nullptr&&previous!=replacement)SDL_ReleaseGPUTexture(device,previous);
    stream.transition_pending=false;
    stream.transition_previous_texture=nullptr;
    stream.transition_previous_resident_mip_start=0U;
    stream.transition_previous_minimum_resident_level=0U;
    stream.transition_previous_next_detail_level=-1;
    stream.transition_previous_uploaded_level_count=0U;
    stream.transition_previous_resident_bytes=0U;
    stream.transition_previous_uploaded_source_bytes=0U;
    stream.transition_previous_uploaded_copy_bytes=0U;
}

void rollback_texture_stream_transition(SDL_GPUDevice* device,RuntimeTextureStream& stream) {
    if(!device||!stream.transition_pending)return;
    const auto previous=stream.transition_previous_texture;
    const auto replacement=stream.texture;
    if(device&&replacement!=nullptr&&replacement!=previous)SDL_ReleaseGPUTexture(device,replacement);
    stream.texture=previous;
    stream.resident_mip_start=stream.transition_previous_resident_mip_start;
    stream.minimum_resident_level=stream.transition_previous_minimum_resident_level;
    stream.next_detail_level=stream.transition_previous_next_detail_level;
    stream.uploaded_level_count=stream.transition_previous_uploaded_level_count;
    stream.resident_bytes=stream.transition_previous_resident_bytes;
    stream.uploaded_source_bytes=stream.transition_previous_uploaded_source_bytes;
    stream.uploaded_copy_bytes=stream.transition_previous_uploaded_copy_bytes;
    stream.transition_pending=false;
    stream.transition_previous_texture=nullptr;
    stream.transition_previous_resident_mip_start=0U;
    stream.transition_previous_minimum_resident_level=0U;
    stream.transition_previous_next_detail_level=-1;
    stream.transition_previous_uploaded_level_count=0U;
    stream.transition_previous_resident_bytes=0U;
    stream.transition_previous_uploaded_source_bytes=0U;
    stream.transition_previous_uploaded_copy_bytes=0U;
}

void release_texture_stream(SDL_GPUDevice* device,RuntimeTextureStream& stream) {
    if(device&&stream.transfer)SDL_ReleaseGPUTransferBuffer(device,stream.transfer);
    // A pending transition owns both resources. Shutdown follows a GPU-idle
    // point, so releasing both here is safe and avoids leaking the old texture
    // if the owner exits before resolving the frame transaction.
    if(device&&stream.transition_pending&&stream.transition_previous_texture&&
       stream.transition_previous_texture!=stream.texture)
        SDL_ReleaseGPUTexture(device,stream.transition_previous_texture);
    if(device&&stream.texture)SDL_ReleaseGPUTexture(device,stream.texture);
    stream.transfer=nullptr;stream.texture=nullptr;stream.valid=false;
    stream.transition_pending=false;stream.transition_previous_texture=nullptr;
}

} // namespace noemancer
