#pragma once

#include "engine/image_decoder.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace noemancer {

struct IblCookProfile final {
    std::uint32_t specular_resolution{64};
    std::uint32_t specular_mip_levels{7};
    std::uint32_t irradiance_resolution{16};
    std::uint32_t brdf_lut_resolution{128};
    std::uint32_t specular_samples{128};
    std::uint32_t irradiance_samples{64};
    std::uint32_t brdf_lut_samples{128};
};

struct IblCookProduct final {
    bool valid{};
    std::string code;
    std::string detail;
    std::string source_id;
    std::string source_fingerprint;
    IblCookProfile profile;
    std::vector<std::uint16_t> specular_rgba16f;
    std::vector<std::uint16_t> irradiance_rgba16f;
    std::vector<std::uint16_t> brdf_lut_rg16f;
};

struct IblCacheResult final {
    IblCookProduct product;
    bool cache_hit{};
    bool cache_rebuilt{};
    std::filesystem::path artifact_path;
    std::uintmax_t artifact_bytes{};
    double cook_microseconds{};
};

[[nodiscard]] IblCookProduct cook_split_sum_ibl(
    const DecodedHdrImage* source,
    std::string source_id,
    std::string source_fingerprint,
    const IblCookProfile& profile = {});

[[nodiscard]] IblCacheResult load_or_cook_split_sum_ibl(
    const std::filesystem::path& cache_root,
    const DecodedHdrImage* source,
    std::string source_id,
    std::string source_fingerprint,
    const IblCookProfile& profile = {});

[[nodiscard]] std::string ibl_profile_fingerprint(const IblCookProfile& profile);

} // namespace noemancer
