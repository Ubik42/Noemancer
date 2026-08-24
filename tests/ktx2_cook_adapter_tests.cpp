#include "engine/ktx2_cook_adapter.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace {

bool has_ktx2_identifier(const std::vector<std::byte>& payload) {
    constexpr std::array<std::uint8_t, 12> identifier{
        0xABU, 0x4BU, 0x54U, 0x58U, 0x20U, 0x32U,
        0x30U, 0xBBU, 0x0DU, 0x0AU, 0x1AU, 0x0AU
    };
    if (payload.size() < identifier.size()) return false;
    for (std::size_t index = 0; index < identifier.size(); ++index)
        if (std::to_integer<std::uint8_t>(payload[index]) != identifier[index]) return false;
    return true;
}

} // namespace

int main(const int argc,char** argv) {
    using namespace noemancer;
    if(argc==2) {
        std::ifstream stream(argv[1],std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(stream),{}};
        const auto decoded=decode_ktx2_rgba8(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()));
        std::cout<<nlohmann::json{{"valid",decoded.valid},{"code",decoded.code},{"detail",decoded.detail},
            {"width",decoded.width},{"height",decoded.height},{"levels",decoded.level_count},
            {"srgb",decoded.srgb},{"bytes",decoded.rgba8.size()}}.dump()<<'\n';
        return decoded.valid?0:2;
    }
    const auto profile = cook_platform_profile("windows-x64-debug");
    const CookSource source{
        .asset_id = "texture.test.ktx2",
        .source_uri = "asset://test/fixture.rgba8",
        .source_hash = "sha256:fixture-ktx2",
        .source_bytes = 16U,
        .importer = "image/rgba8"
    };
    const TextureCookInput input{
        .width = 2U,
        .height = 2U,
        .rgba8 = {
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
            std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
            std::byte{0x00}, std::byte{0xff}, std::byte{0x00}, std::byte{0xff},
            std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff}
        }
    };
    TextureCookSettings settings;
    settings.semantic = TextureSemantic::base_color;
    settings.alpha_mode = TextureAlphaMode::blend;
    settings.srgb = true;
    settings.generate_mipmaps = true;
    settings.quality = 2U;

    const auto basis = execute_texture_cook(source, input, profile, settings, TextureCookCompression::basis_lz);
    if (!ktx2_available()) {
        if (basis.valid || basis.code != "asset.ktx2-unavailable" || basis.ktx_available) {
            std::cerr << "Missing KTX dependency did not produce an explicit unavailable result.\n";
            return 1;
        }
        std::cout << "ktx2_cook_adapter_tests: KTX unavailable; executor skipped\n";
        return 0;
    }
    if (!basis.valid || basis.code != "ok" || !basis.ktx_available ||
        basis.width != 2U || basis.height != 2U || basis.level_count != 2U ||
        basis.compression != "basis-lz" || basis.supercompression != "basis-lz" ||
        basis.payload_format != "ktx2" || basis.payload.empty() ||
        !has_ktx2_identifier(basis.payload) || basis.payload_fingerprint.empty()) {
        std::cerr << "BasisLZ KTX2 output did not preserve verified dimensions and metadata.\n";
        return 2;
    }
    const auto rgba_chain=decode_ktx2_mip_chain(basis.payload,RuntimeTextureFormat::rgba8);
    if(!rgba_chain.valid||rgba_chain.format!=RuntimeTextureFormat::rgba8||rgba_chain.width!=2U||
       rgba_chain.height!=2U||rgba_chain.levels.size()!=2U||rgba_chain.levels[0].bytes.size()!=16U||
       rgba_chain.levels[1].width!=1U||rgba_chain.levels[1].height!=1U||
       rgba_chain.upload_bytes!=20U||rgba_chain.source_bytes!=basis.payload.size()) {
        std::cerr << "Runtime RGBA8 mip-chain decode did not preserve every authored level.\n";
        return 11;
    }
    const auto bc7_chain=decode_ktx2_mip_chain(basis.payload,RuntimeTextureFormat::bc7_rgba);
    if(!bc7_chain.valid||bc7_chain.format!=RuntimeTextureFormat::bc7_rgba||
       bc7_chain.levels.size()!=2U||bc7_chain.levels[0].bytes.size()!=16U||
       bc7_chain.levels[1].bytes.size()!=16U||bc7_chain.upload_bytes!=32U||!bc7_chain.srgb||
       runtime_texture_format_name(bc7_chain.format)!="bc7-rgba") {
        std::cerr << "Runtime BC7 mip-chain transcode did not preserve the full KTX2 chain.\n";
        return 12;
    }
    const auto basis_repeat = execute_texture_cook(source, input, profile, settings, TextureCookCompression::basis_lz);
    if (!basis_repeat.valid || basis.payload != basis_repeat.payload ||
        basis.payload_fingerprint != basis_repeat.payload_fingerprint ||
        basis.level_count != basis_repeat.level_count) {
        std::cerr << "BasisLZ KTX2 output was not deterministic.\n";
        return 3;
    }
    const auto basis_json = nlohmann::json::parse(texture_cook_product_json(basis));
    if (basis_json.at("schema") != "noemancer.texture-artifact/0.1" ||
        basis_json.at("payload").at("format") != "ktx2" ||
        basis_json.at("dimensions").at("levels") != 2U ||
        basis_json.at("compression") != "basis-lz" ||
        basis_json.at("source").at("hash") != source.source_hash) {
        std::cerr << "KTX2 product JSON did not expose stable engine-owned metadata.\n";
        return 4;
    }

    const auto uastc = execute_texture_cook(source, input, profile, settings, TextureCookCompression::uastc);
    if (!uastc.valid || uastc.code != "ok" || uastc.compression != "uastc" ||
        uastc.supercompression != "none" || !has_ktx2_identifier(uastc.payload) ||
        uastc.payload.empty()) {
        std::cerr << "UASTC KTX2 output was not produced with verified UASTC metadata.\n";
        return 5;
    }

    TextureCookSettings explicit_settings = settings;
    explicit_settings.generate_mipmaps = false;
    const TextureCookInput explicit_input{
        .width = 4U,
        .height = 4U,
        .rgba8 = std::vector<std::byte>(4U * 4U * 4U, std::byte{0x7f}),
        .mip_levels = {TextureCookMip{
            .width = 2U,
            .height = 2U,
            .rgba8 = std::vector<std::byte>(2U * 2U * 4U, std::byte{0x3f})
        }}
    };
    const auto explicit_mips = execute_texture_cook(source, explicit_input, profile,
        explicit_settings, TextureCookCompression::basis_lz);
    if (!explicit_mips.valid || explicit_mips.level_count != 2U ||
        explicit_mips.diagnostics.empty()) {
        std::cerr << "Supplied KTX2 mip levels were not preserved.\n";
        return 6;
    }

    TextureCookSettings capped_settings = settings;
    capped_settings.max_dimension = 1U;
    const auto capped = execute_texture_cook(source, input, profile, capped_settings);
    if (capped.valid || capped.code != "asset.ktx2-dimensions-exceed-max") {
        std::cerr << "KTX2 max_dimension contract was not enforced.\n";
        return 7;
    }
    auto no_generated_mips_profile = profile;
    no_generated_mips_profile.generate_mipmaps = false;
    const auto source_only = execute_texture_cook(source, input, no_generated_mips_profile, settings);
    if (!source_only.valid || source_only.level_count != 1U || source_only.diagnostics.empty() ||
        source_only.diagnostics[1] != "mipPolicy:base-only") {
        std::cerr << "KTX2 profile mip policy was not enforced.\n";
        return 8;
    }

    auto invalid = input;
    invalid.rgba8.pop_back();
    const auto invalid_result = execute_texture_cook(source, invalid, profile, settings);
    if (invalid_result.valid || invalid_result.code != "asset.ktx2-base-level-invalid") {
        std::cerr << "Malformed KTX2 base level was accepted.\n";
        return 9;
    }
    TextureCookSettings invalid_normal = settings;
    invalid_normal.semantic = TextureSemantic::normal;
    const auto invalid_normal_result = execute_texture_cook(source, input, profile, invalid_normal);
    if (invalid_normal_result.valid || invalid_normal_result.code != "asset.texture-color-space-invalid") {
        std::cerr << "sRGB normal-map KTX2 contract was accepted.\n";
        return 10;
    }
    std::cout << "ktx2_cook_adapter_tests: ok\n";
    return 0;
}
