#include "engine/ktx2_cook_adapter.hpp"

#if __has_include(<ktx.h>)
#include <ktx.h>
#define NOEMANCER_HAS_KTX2 1
#else
#define NOEMANCER_HAS_KTX2 0
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace noemancer {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::uint8_t, 12> kKtx2Identifier{
    0xABU, 0x4BU, 0x54U, 0x58U, 0x20U, 0x32U,
    0x30U, 0xBBU, 0x0DU, 0x0AU, 0x1AU, 0x0AU
};

std::uint64_t fnv1a(const std::span<const std::byte> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex_u64(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

void populate_product(TextureCookProduct& result, const CookSource& source,
                      const CookPlatformProfile& profile, const TextureCookSettings& settings,
                      const TextureCookCompression compression) {
    result.source_asset_id = source.asset_id;
    result.source_hash = source.source_hash;
    result.target_profile = profile.id;
    result.semantic = texture_semantic_name(settings.semantic);
    result.alpha_mode = texture_alpha_mode_name(settings.alpha_mode);
    result.color_space = settings.srgb ? "srgb" : "linear";
    result.compression = texture_cook_compression_name(compression);
}

bool valid_level_dimensions(const std::uint32_t width, const std::uint32_t height,
                            const std::vector<std::byte>& pixels) {
    if (width == 0U || height == 0U) return false;
    const auto expected = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ULL;
    return expected <= std::numeric_limits<std::size_t>::max() && pixels.size() == expected;
}

TextureCookMip downsample_rgba8(const TextureCookMip& source) {
    TextureCookMip result;
    result.width = std::max(1U, source.width / 2U);
    result.height = std::max(1U, source.height / 2U);
    result.rgba8.resize(static_cast<std::size_t>(result.width) * result.height * 4U);
    for (std::uint32_t y = 0; y < result.height; ++y) {
        for (std::uint32_t x = 0; x < result.width; ++x) {
            const auto dst = (static_cast<std::size_t>(y) * result.width + x) * 4U;
            std::uint32_t samples = 0U;
            std::array<std::uint32_t, 4> sums{};
            for (std::uint32_t dy = 0; dy < 2U; ++dy) {
                const auto source_y = std::min(source.height - 1U, y * 2U + dy);
                for (std::uint32_t dx = 0; dx < 2U; ++dx) {
                    const auto source_x = std::min(source.width - 1U, x * 2U + dx);
                    const auto src = (static_cast<std::size_t>(source_y) * source.width + source_x) * 4U;
                    for (std::size_t channel = 0; channel < 4U; ++channel)
                        sums[channel] += std::to_integer<std::uint8_t>(source.rgba8[src + channel]);
                    ++samples;
                }
            }
            for (std::size_t channel = 0; channel < 4U; ++channel)
                result.rgba8[dst + channel] = static_cast<std::byte>((sums[channel] + samples / 2U) / samples);
        }
    }
    return result;
}

bool build_mip_chain(const TextureCookInput& input, const TextureCookSettings& settings,
                     std::vector<TextureCookMip>& levels, std::string& code, std::string& detail) {
    if (input.width == 0U || input.height == 0U || input.width > 32768U || input.height > 32768U) {
        code = "asset.ktx2-dimensions-invalid";
        detail = "Texture dimensions must be within 1..32768 for the KTX2 Cook adapter.";
        return false;
    }
    if (!valid_level_dimensions(input.width, input.height, input.rgba8)) {
        code = "asset.ktx2-base-level-invalid";
        detail = "Texture base level must contain exactly width*height*4 RGBA8 bytes.";
        return false;
    }
    levels.push_back(TextureCookMip{input.width, input.height, input.rgba8});
    std::uint32_t previous_width = input.width;
    std::uint32_t previous_height = input.height;
    for (const auto& supplied : input.mip_levels) {
        const auto expected_width = std::max(1U, previous_width / 2U);
        const auto expected_height = std::max(1U, previous_height / 2U);
        if (supplied.width != expected_width || supplied.height != expected_height ||
            !valid_level_dimensions(supplied.width, supplied.height, supplied.rgba8)) {
            code = "asset.ktx2-mip-invalid";
            detail = "Supplied mip levels must be contiguous half-resolution RGBA8 levels.";
            return false;
        }
        levels.push_back(supplied);
        previous_width = supplied.width;
        previous_height = supplied.height;
        if (levels.size() > 32U) {
            code = "asset.ktx2-mip-count-invalid";
            detail = "KTX2 Cook input contains too many mip levels.";
            return false;
        }
    }
    if (settings.generate_mipmaps) {
        while (previous_width > 1U || previous_height > 1U) {
            levels.push_back(downsample_rgba8(levels.back()));
            previous_width = levels.back().width;
            previous_height = levels.back().height;
            if (levels.size() > 32U) {
                code = "asset.ktx2-mip-count-invalid";
                detail = "Generated KTX2 mip chain exceeded the supported level count.";
                return false;
            }
        }
    }
    return true;
}

bool has_ktx2_identifier(const std::vector<std::byte>& payload) {
    if (payload.size() < kKtx2Identifier.size()) return false;
    for (std::size_t index = 0; index < kKtx2Identifier.size(); ++index) {
        if (std::to_integer<std::uint8_t>(payload[index]) != kKtx2Identifier[index]) return false;
    }
    return true;
}

#if NOEMANCER_HAS_KTX2

std::string supercompression_name(const ktxSupercmpScheme scheme) {
    switch (scheme) {
    case KTX_SS_NONE: return "none";
    case KTX_SS_BASIS_LZ: return "basis-lz";
    case KTX_SS_ZSTD: return "zstd";
    case KTX_SS_ZLIB: return "zlib";
    default: return "vendor-or-unknown";
    }
}

void set_ktx_error(TextureCookProduct& result, const std::string_view prefix,
                   const KTX_error_code error) {
    result.code = "asset.ktx2-operation-failed";
    result.detail = std::string(prefix) + ": " + ktxErrorString(error);
    result.diagnostics.push_back(result.detail);
}

#endif

} // namespace

std::string texture_cook_compression_name(const TextureCookCompression compression) {
    switch (compression) {
    case TextureCookCompression::basis_lz: return "basis-lz";
    case TextureCookCompression::uastc: return "uastc";
    }
    return "unknown";
}

std::string runtime_texture_format_name(const RuntimeTextureFormat format) {
    switch (format) {
    case RuntimeTextureFormat::rgba8: return "rgba8";
    case RuntimeTextureFormat::bc7_rgba: return "bc7-rgba";
    }
    return "unknown";
}

bool ktx2_available() noexcept {
#if NOEMANCER_HAS_KTX2
    return true;
#else
    return false;
#endif
}

TextureCookProduct execute_texture_cook(const CookSource& source, const TextureCookInput& input,
                                        const CookPlatformProfile& profile,
                                        const TextureCookSettings& settings,
                                        const TextureCookCompression compression) {
    TextureCookProduct result;
    result.ktx_available = ktx2_available();
    populate_product(result, source, profile, settings, compression);

    const auto plan = plan_texture_cook(source, profile, settings);
    if (!plan.valid) {
        result.code = plan.code;
        result.detail = plan.detail;
        result.diagnostics = plan.diagnostics;
        return result;
    }
    if (!result.ktx_available) {
        result.code = "asset.ktx2-unavailable";
        result.detail = "ktx.h was not available to the Texture Cook target.";
        result.diagnostics.push_back(result.detail);
        return result;
    }

    if (settings.max_dimension != 0U &&
        (input.width > settings.max_dimension || input.height > settings.max_dimension)) {
        result.code = "asset.ktx2-dimensions-exceed-max";
        result.detail = "Texture dimensions exceed the max_dimension Cook setting.";
        result.diagnostics.push_back(result.detail);
        return result;
    }

    // The platform profile is part of the Cook contract. A target may disable
    // generated mips even when an authoring setting requests them; supplied
    // source mips remain valid and are never discarded.
    TextureCookSettings execution_settings = settings;
    execution_settings.generate_mipmaps = settings.generate_mipmaps && profile.generate_mipmaps;
    std::vector<TextureCookMip> levels;
    if (!build_mip_chain(input, execution_settings, levels, result.code, result.detail)) {
        result.diagnostics.push_back(result.detail);
        return result;
    }

#if NOEMANCER_HAS_KTX2
    // Vulkan's core enum values are stable API values and are used here only
    // in this private adapter.  37/43 are R8G8B8A8_UNORM/SRGB respectively;
    // no Vulkan header or type enters the engine contract.
    constexpr ktx_uint32_t vk_format_rgba8_unorm = 37U;
    constexpr ktx_uint32_t vk_format_rgba8_srgb = 43U;
    ktxTextureCreateInfo create_info{};
    create_info.vkFormat = settings.srgb ? vk_format_rgba8_srgb : vk_format_rgba8_unorm;
    create_info.baseWidth = input.width;
    create_info.baseHeight = input.height;
    create_info.baseDepth = 1U;
    create_info.numDimensions = 2U;
    create_info.numLevels = static_cast<ktx_uint32_t>(levels.size());
    create_info.numLayers = 1U;
    create_info.numFaces = 1U;
    create_info.isArray = KTX_FALSE;
    create_info.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    auto error = ktxTexture2_Create(&create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
    if (error != KTX_SUCCESS || texture == nullptr) {
        set_ktx_error(result, "ktxTexture2_Create", error);
        return result;
    }
    const auto destroy_texture = [&]() { if (texture != nullptr) ktxTexture2_Destroy(texture); };
    if (ktxTexture2_SetTransferFunction(texture,
        settings.srgb ? KHR_DF_TRANSFER_SRGB : KHR_DF_TRANSFER_LINEAR) != KTX_SUCCESS) {
        result.code = "asset.ktx2-transfer-function-failed";
        result.detail = "libktx rejected the requested texture transfer function.";
        result.diagnostics.push_back(result.detail);
        destroy_texture();
        return result;
    }
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const auto& mip = levels[level];
        error = ktxTexture_SetImageFromMemory(
            ktxTexture(texture), static_cast<ktx_uint32_t>(level), 0U, 0U,
            reinterpret_cast<const ktx_uint8_t*>(mip.rgba8.data()),
            static_cast<ktx_size_t>(mip.rgba8.size()));
        if (error != KTX_SUCCESS) {
            set_ktx_error(result, "ktxTexture_SetImageFromMemory", error);
            destroy_texture();
            return result;
        }
    }

    ktxBasisParams basis_params{};
    basis_params.structSize = sizeof(basis_params);
    basis_params.uastc = compression == TextureCookCompression::uastc ? KTX_TRUE : KTX_FALSE;
    basis_params.threadCount = 1U;
    basis_params.compressionLevel = std::min(6U, settings.quality);
    basis_params.qualityLevel = std::clamp(settings.quality * 51U + 1U, 1U, 255U);
    basis_params.normalMap = settings.semantic == TextureSemantic::normal ? KTX_TRUE : KTX_FALSE;
    basis_params.uastcFlags = std::min(settings.quality, 4U);
    basis_params.uastcRDONoMultithreading = KTX_TRUE;
    error = ktxTexture2_CompressBasisEx(texture, &basis_params);
    if (error != KTX_SUCCESS) {
        set_ktx_error(result, "ktxTexture2_CompressBasisEx", error);
        destroy_texture();
        return result;
    }

    ktx_uint8_t* encoded = nullptr;
    ktx_size_t encoded_size = 0U;
    error = ktxTexture2_WriteToMemory(texture, &encoded, &encoded_size);
    if (error != KTX_SUCCESS || encoded == nullptr || encoded_size == 0U) {
        set_ktx_error(result, "ktxTexture2_WriteToMemory", error);
        destroy_texture();
        if (encoded != nullptr) std::free(encoded);
        return result;
    }
    result.payload.assign(reinterpret_cast<const std::byte*>(encoded),
                          reinterpret_cast<const std::byte*>(encoded) + encoded_size);
    std::free(encoded);
    destroy_texture();

    if (!has_ktx2_identifier(result.payload)) {
        result.code = "asset.ktx2-identifier-invalid";
        result.detail = "libktx emitted a payload without the KTX2 identifier.";
        result.diagnostics.push_back(result.detail);
        result.payload.clear();
        return result;
    }

    ktxTexture2* verification = nullptr;
    error = ktxTexture2_CreateFromMemory(
        reinterpret_cast<const ktx_uint8_t*>(result.payload.data()),
        static_cast<ktx_size_t>(result.payload.size()), KTX_TEXTURE_CREATE_NO_FLAGS, &verification);
    if (error != KTX_SUCCESS || verification == nullptr) {
        set_ktx_error(result, "ktxTexture2_CreateFromMemory", error);
        if (verification != nullptr) ktxTexture2_Destroy(verification);
        result.payload.clear();
        return result;
    }
    const auto expected_supercompression = compression == TextureCookCompression::basis_lz
        ? "basis-lz"
        : "none";
    const bool metadata_matches = verification->baseWidth == input.width &&
        verification->baseHeight == input.height && verification->numLevels == levels.size();
    result.supercompression = supercompression_name(verification->supercompressionScheme);
    ktxTexture2_Destroy(verification);
    if (!metadata_matches || result.supercompression != expected_supercompression) {
        result.code = "asset.ktx2-metadata-invalid";
        result.detail = "KTX2 verification did not preserve dimensions, mip levels or the requested Basis encoding metadata.";
        result.diagnostics.push_back(result.detail);
        result.payload.clear();
        return result;
    }

    result.width = input.width;
    result.height = input.height;
    result.level_count = static_cast<std::uint32_t>(levels.size());
    result.payload_fingerprint = "fnv1a64:" + hex_u64(fnv1a(
        std::span<const std::byte>(result.payload.data(), result.payload.size())));
    result.valid = true;
    result.code = "ok";
    result.detail = "Texture encoded as deterministic KTX2 " + result.compression +
        " payload with verified mip and supercompression metadata.";
    result.diagnostics.push_back("executor:libktx/4.4.2");
    result.diagnostics.push_back("mipPolicy:" + std::string(input.mip_levels.empty() ?
        (execution_settings.generate_mipmaps ? "generated-box2x2" : "base-only") :
        (execution_settings.generate_mipmaps ? "provided-plus-generated" : "provided-only")));
    result.diagnostics.push_back("levels:" + std::to_string(result.level_count));
    result.diagnostics.push_back("target:" + plan.payload_target);
    return result;
#else
    static_cast<void>(input);
    static_cast<void>(compression);
    return result;
#endif
}

std::string texture_cook_product_json(const TextureCookProduct& product) {
    return Json{
        {"schema", product.schema},
        {"valid", product.valid},
        {"ktxAvailable", product.ktx_available},
        {"code", product.code},
        {"detail", product.detail},
        {"source", {
            {"assetId", product.source_asset_id},
            {"hash", product.source_hash}
        }},
        {"targetProfile", product.target_profile},
        {"semantic", product.semantic},
        {"alphaMode", product.alpha_mode},
        {"colorSpace", product.color_space},
        {"compression", product.compression},
        {"supercompression", product.supercompression},
        {"dimensions", {
            {"width", product.width},
            {"height", product.height},
            {"levels", product.level_count}
        }},
        {"payload", {
            {"format", product.payload_format},
            {"bytes", product.payload.size()},
            {"fingerprint", product.payload_fingerprint}
        }},
        {"diagnostics", product.diagnostics}
    }.dump();
}

DecodedKtx2MipChain decode_ktx2_mip_chain(const std::span<const std::byte> payload,
                                          const RuntimeTextureFormat format) {
    DecodedKtx2MipChain result;
    result.format=format;
    result.source_bytes=payload.size();
#if NOEMANCER_HAS_KTX2
    if(payload.empty()){result.code="asset.ktx2-empty";result.detail="KTX2 payload is empty.";return result;}
    ktxTexture2* texture=nullptr;
    auto error=ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(payload.data()),
        static_cast<ktx_size_t>(payload.size()),KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,&texture);
    if(error!=KTX_SUCCESS||texture==nullptr){result.code="asset.ktx2-runtime-open-failed";
        result.detail="libktx could not open the Runtime texture payload: "+std::string(ktxErrorString(error));return result;}
    const auto destroy=[&]{ktxTexture2_Destroy(texture);};
    result.width=texture->baseWidth;result.height=texture->baseHeight;
    result.srgb=ktxTexture2_GetTransferFunction_e(texture)==KHR_DF_TRANSFER_SRGB;
    if(texture->numDimensions!=2U||texture->numLayers!=1U||texture->numFaces!=1U||result.width==0U||result.height==0U) {
        result.code="asset.ktx2-runtime-shape-unsupported";result.detail="Runtime mip decoder accepts one non-array 2D texture.";destroy();return result;}
    const bool needs_transcoding=ktxTexture2_NeedsTranscoding(texture);
    if(needs_transcoding) {
        const auto target=format==RuntimeTextureFormat::bc7_rgba?KTX_TTF_BC7_RGBA:KTX_TTF_RGBA32;
        error=ktxTexture2_TranscodeBasis(texture,target,0U);
        if(error!=KTX_SUCCESS){result.code="asset.ktx2-runtime-transcode-failed";
            result.detail="Basis payload could not transcode to "+runtime_texture_format_name(format)+": "+
                std::string(ktxErrorString(error));destroy();return result;}
    } else if(format==RuntimeTextureFormat::bc7_rgba) {
        result.code="asset.ktx2-runtime-format-unavailable";
        result.detail="An uncompressed KTX2 payload cannot be re-encoded to BC7 at Runtime.";
        destroy();return result;
    } else if(texture->vkFormat!=37U&&texture->vkFormat!=43U) {
        result.code="asset.ktx2-runtime-format-unsupported";
        result.detail="Runtime RGBA8 extraction only accepts R8G8B8A8_UNORM or R8G8B8A8_SRGB payloads.";
        destroy();return result;
    }
    result.levels.reserve(texture->numLevels);
    std::uint32_t width=result.width,height=result.height;
    for(std::uint32_t level=0;level<texture->numLevels;++level) {
        ktx_size_t offset{};error=ktxTexture2_GetImageOffset(texture,level,0U,0U,&offset);
        const auto bytes=format==RuntimeTextureFormat::bc7_rgba
            ? static_cast<std::uint64_t>((width+3U)/4U)*((height+3U)/4U)*16ULL
            : static_cast<std::uint64_t>(width)*height*4ULL;
        if(error!=KTX_SUCCESS||texture->pData==nullptr||bytes>texture->dataSize||offset>texture->dataSize-bytes||
            bytes>std::numeric_limits<std::size_t>::max()) {
            result.code="asset.ktx2-runtime-data-invalid";
            result.detail="Transcoded KTX2 mip level is missing or truncated.";destroy();return result;
        }
        RuntimeTextureMip mip{level,width,height,{}};
        mip.bytes.assign(reinterpret_cast<const std::byte*>(texture->pData+offset),
            reinterpret_cast<const std::byte*>(texture->pData+offset+bytes));
        result.upload_bytes+=bytes;result.levels.push_back(std::move(mip));
        width=std::max(1U,width/2U);height=std::max(1U,height/2U);
    }
    destroy();result.valid=true;result.code="ok";
    result.detail="KTX2 full mip chain validated and transcoded to "+runtime_texture_format_name(format)+".";
    return result;
#else
    static_cast<void>(payload);static_cast<void>(format);result.code="asset.ktx2-unavailable";
    result.detail="This build has no libktx Runtime adapter.";return result;
#endif
}

DecodedKtx2Texture decode_ktx2_rgba8(const std::span<const std::byte> payload) {
    DecodedKtx2Texture result;
    const auto chain=decode_ktx2_mip_chain(payload,RuntimeTextureFormat::rgba8);
    result.valid=chain.valid;result.code=chain.code;result.detail=chain.detail;
    result.width=chain.width;result.height=chain.height;result.level_count=static_cast<std::uint32_t>(chain.levels.size());
    result.srgb=chain.srgb;
    if(chain.valid&&!chain.levels.empty())result.rgba8=chain.levels.front().bytes;
    if(result.valid)result.detail="KTX2 base level validated and transcoded to portable RGBA8.";
    return result;
}

} // namespace noemancer
