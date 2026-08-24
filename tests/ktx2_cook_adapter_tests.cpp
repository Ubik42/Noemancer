#include "engine/ktx2_cook_adapter.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

bool has_diagnostic(const noemancer::TextureCookProduct& product, const std::string_view value) {
    return std::find(product.diagnostics.begin(), product.diagnostics.end(), value) !=
        product.diagnostics.end();
}

bool has_stage(const noemancer::TextureCookProduct& product, const std::string_view value) {
    return std::find_if(product.stage_timings.begin(), product.stage_timings.end(),
                        [&](const auto& stage) { return stage.name == value; }) !=
        product.stage_timings.end();
}

bool parse_pressure_worker_selector(
    const std::string_view selector, noemancer::TextureCookExecutionOptions& options) {
    if (selector == "auto") {
        options.requested_worker_count = 0U;
        return true;
    }
    if (selector.empty()) return false;
    std::uint32_t parsed{};
    const auto parsed_result = std::from_chars(selector.data(), selector.data() + selector.size(),
                                               parsed, 10);
    if (parsed_result.ec != std::errc{} || parsed_result.ptr != selector.data() + selector.size() ||
        parsed == 0U) {
        return false;
    }
    options.requested_worker_count = parsed;
    return true;
}

nlohmann::json timing_json(const noemancer::TextureCookProduct& product) {
    nlohmann::json stages = nlohmann::json::array();
    for (const auto& stage : product.stage_timings) {
        stages.push_back(nlohmann::json{
            {"name", stage.name},
            {"microseconds", stage.microseconds}
        });
    }
    return nlohmann::json{
        {"totalMicroseconds", product.total_microseconds},
        {"stages", std::move(stages)}
    };
}

int verify_pressure_worker_selector_parser() {
    using noemancer::TextureCookExecutionOptions;
    TextureCookExecutionOptions options;
    if (!parse_pressure_worker_selector("1", options) || options.requested_worker_count != 1U ||
        !parse_pressure_worker_selector("8", options) || options.requested_worker_count != 8U ||
        !parse_pressure_worker_selector("auto", options) || options.requested_worker_count != 0U ||
        parse_pressure_worker_selector("0", options) ||
        parse_pressure_worker_selector("not-a-worker-count", options)) {
        std::cerr << "KTX2 pressure worker selector parser contract failed.\n";
        return 18;
    }
    return 0;
}

int run_4k_pressure(const noemancer::TextureCookExecutionOptions execution_options) {
    using namespace noemancer;
    if (!ktx2_available()) {
        std::cout << nlohmann::json{
            {"schemaVersion", "noemancer.ktx2-cook-pressure/0.1"},
            {"valid", false},
            {"workerCount", 0U},
            {"requestedWorkerCount", execution_options.requested_worker_count},
            {"cacheHit", false},
            {"totalMicroseconds", 0U},
            {"timing", nlohmann::json{
                {"first", nlohmann::json{{"totalMicroseconds", 0U}, {"stages", nlohmann::json::array()}}},
                {"repeat", nlohmann::json{{"totalMicroseconds", 0U}, {"stages", nlohmann::json::array()}}}
            }},
            {"payloadFingerprint", ""},
            {"payloadBytes", 0U},
            {"error", "asset.ktx2-unavailable"}
        }.dump() << "\n";
        return 0;
    }
    constexpr std::uint32_t width = 4096U;
    constexpr std::uint32_t height = 4096U;
    TextureCookInput input{
        .width = width,
        .height = height,
        .rgba8 = std::vector<std::byte>(static_cast<std::size_t>(width) * height * 4U,
            std::byte{0x7f})
    };
    TextureCookSettings settings;
    settings.semantic = TextureSemantic::base_color;
    settings.alpha_mode = TextureAlphaMode::blend;
    settings.srgb = true;
    settings.generate_mipmaps = true;
    settings.quality = 2U;
    const auto profile = cook_platform_profile("windows-x64-debug");
    const CookSource source{
        .asset_id = "texture.test.pressure-4k",
        .source_uri = "asset://test/pressure-4k.rgba8",
        .source_hash = "sha256:pressure-4k-fixture",
        .source_bytes = input.rgba8.size(),
        .importer = "image/rgba8"
    };
    const auto first_started = std::chrono::steady_clock::now();
    const auto first = execute_texture_cook(source, input, profile, settings,
        TextureCookCompression::basis_lz, execution_options);
    const auto first_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - first_started).count();
    const auto repeat_started = std::chrono::steady_clock::now();
    const auto repeat = execute_texture_cook(source, input, profile, settings,
        TextureCookCompression::basis_lz, execution_options);
    const auto repeat_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - repeat_started).count();
    const auto expected_worker_count = execution_options.requested_worker_count == 0U
        ? first.worker_count
        : std::min(execution_options.requested_worker_count, kTextureCookMaxWorkerCount);
    if (!first.valid || !repeat.valid || first.payload != repeat.payload ||
        first.cache_hit || !repeat.cache_hit || first.worker_count != expected_worker_count ||
        repeat.worker_count != first.worker_count || first.worker_count == 0U ||
        first.worker_count > kTextureCookMaxWorkerCount ||
        repeat.requested_worker_count != execution_options.requested_worker_count ||
        !has_diagnostic(repeat, "cache:hit")) {
        std::cerr << "4K KTX2 pressure did not preserve payload identity or cache reuse\n";
        return 20;
    }
    const auto report = nlohmann::json{
        {"schemaVersion", "noemancer.ktx2-cook-pressure/0.1"},
        {"valid", true},
        {"workerCount", first.worker_count},
        {"requestedWorkerCount", execution_options.requested_worker_count},
        {"cacheHit", repeat.cache_hit},
        {"firstCacheHit", first.cache_hit},
        {"repeatCacheHit", repeat.cache_hit},
        {"firstWallMilliseconds", first_milliseconds},
        {"repeatWallMilliseconds", repeat_milliseconds},
        {"totalMicroseconds", first.total_microseconds},
        {"timing", {
            {"first", timing_json(first)},
            {"repeat", timing_json(repeat)}
        }},
        {"payloadFingerprint", first.payload_fingerprint},
        {"payloadBytes", first.payload.size()}
    };
    std::cout << report.dump() << "\n";
    return 0;
}

} // namespace

int main(const int argc,char** argv) {
    using namespace noemancer;
    if (const auto parser_status = verify_pressure_worker_selector_parser(); parser_status != 0)
        return parser_status;
    if (argc >= 2 && std::string_view(argv[1]) == "--pressure-4k") {
        if (argc > 3) {
            std::cerr << "Usage: ktx2_cook_adapter_tests --pressure-4k [1|8|auto]\n";
            return 64;
        }
        TextureCookExecutionOptions execution_options;
        if (argc == 3 && !parse_pressure_worker_selector(argv[2], execution_options)) {
            std::cerr << "Usage: ktx2_cook_adapter_tests --pressure-4k [1|8|auto]\n";
            return 64;
        }
        return run_4k_pressure(execution_options);
    }
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

    const auto first_started = std::chrono::steady_clock::now();
    const auto basis = execute_texture_cook(source, input, profile, settings, TextureCookCompression::basis_lz);
    const auto first_milliseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - first_started).count();
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
    if (basis.cache_hit || basis.requested_worker_count != 1U || basis.worker_count != 1U ||
        basis.input_fingerprint.empty() || basis.total_microseconds == 0U ||
        !has_stage(basis, "input-fingerprint") || !has_stage(basis, "mip-generation") ||
        !has_stage(basis, "ktx-load") || !has_stage(basis, "basis-encode") ||
        !has_stage(basis, "write-output") || !has_stage(basis, "verify-output")) {
        std::cerr << "KTX2 product did not expose measured execution stages or bounded default workers.\n";
        return 13;
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
    const auto repeat_started = std::chrono::steady_clock::now();
    const auto basis_repeat = execute_texture_cook(source, input, profile, settings, TextureCookCompression::basis_lz);
    const auto repeat_milliseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - repeat_started).count();
    if (!basis_repeat.valid || basis.payload != basis_repeat.payload ||
        basis.payload_fingerprint != basis_repeat.payload_fingerprint ||
        basis.level_count != basis_repeat.level_count || !basis_repeat.cache_hit ||
        basis_repeat.worker_count != basis.worker_count || !has_diagnostic(basis_repeat, "cache:hit") ||
        !has_stage(basis_repeat, "input-fingerprint") || !has_stage(basis_repeat, "cook-plan") ||
        !has_stage(basis_repeat, "cache-lookup") || has_stage(basis_repeat, "basis-encode")) {
        std::cerr << "BasisLZ KTX2 output was not deterministic.\n";
        return 3;
    }
    const auto basis_json = nlohmann::json::parse(texture_cook_product_json(basis));
    if (basis_json.at("schema") != "noemancer.texture-artifact/0.1" ||
        basis_json.at("payload").at("format") != "ktx2" ||
        basis_json.at("dimensions").at("levels") != 2U ||
        basis_json.at("compression") != "basis-lz" ||
        basis_json.at("source").at("hash") != source.source_hash ||
        basis_json.at("cacheHit") != false || basis_json.at("workerCount") != 1U ||
        basis_json.at("requestedWorkerCount") != 1U ||
        basis_json.at("inputFingerprint") != basis.input_fingerprint ||
        !basis_json.at("timing").at("stages").is_array() ||
        basis_json.at("timing").at("stages").size() < 7U) {
        std::cerr << "KTX2 product JSON did not expose stable engine-owned metadata.\n";
        return 4;
    }

    TextureCookExecutionOptions parallel_options;
    parallel_options.requested_worker_count = 2U;
    const auto parallel = execute_texture_cook(source, input, profile, settings,
        TextureCookCompression::basis_lz, parallel_options);
    if (!parallel.valid || parallel.cache_hit || parallel.worker_count != 2U ||
        parallel.requested_worker_count != 2U) {
        std::cerr << "Explicit KTX2 worker selection was not applied within the bounded policy.\n";
        return 14;
    }
    const auto parallel_repeat = execute_texture_cook(source, input, profile, settings,
        TextureCookCompression::basis_lz, parallel_options);
    if (!parallel_repeat.valid || !parallel_repeat.cache_hit ||
        parallel.payload != parallel_repeat.payload ||
        parallel.payload_fingerprint != parallel_repeat.payload_fingerprint) {
        std::cerr << "Explicit parallel KTX2 output did not preserve same-worker identity.\n";
        return 15;
    }

    TextureCookExecutionOptions automatic_options;
    automatic_options.requested_worker_count = 0U;
    const auto automatic = execute_texture_cook(source, input, profile, settings,
        TextureCookCompression::basis_lz, automatic_options);
    if (!automatic.valid || automatic.worker_count == 0U ||
        automatic.worker_count > kTextureCookMaxWorkerCount ||
        automatic.requested_worker_count != 0U) {
        std::cerr << "Automatic KTX2 worker selection escaped its bounded contract.\n";
        return 16;
    }

    TextureCookExecutionOptions oversized_options;
    oversized_options.requested_worker_count = std::numeric_limits<std::uint32_t>::max();
    const auto oversized = execute_texture_cook(source, input, profile, settings,
        TextureCookCompression::basis_lz, oversized_options);
    if (!oversized.valid || oversized.worker_count != kTextureCookMaxWorkerCount ||
        oversized.requested_worker_count != std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "Oversized KTX2 worker selection was not clamped to the public cap.\n";
        return 17;
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
    std::cout << "ktx2_cook_adapter_tests: ok first_us=" << first_milliseconds
              << " repeat_us=" << repeat_milliseconds << "\n";
    return 0;
}
