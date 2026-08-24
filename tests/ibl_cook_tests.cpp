#include "engine/ibl_cook.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    using namespace noemancer;
    IblCookProfile profile;
    profile.specular_resolution=4;
    profile.specular_mip_levels=3;
    profile.irradiance_resolution=2;
    profile.brdf_lut_resolution=4;
    profile.specular_samples=8;
    profile.irradiance_samples=8;
    profile.brdf_lut_samples=8;

    const auto unique=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto cache_root=std::filesystem::temp_directory_path()/("noemancer-ibl-test-"+unique);
    const auto first=load_or_cook_split_sum_ibl(cache_root,nullptr,"asset.environment.test","builtin:test/1",profile);
    if (!first.product.valid || first.cache_hit || !std::filesystem::is_regular_file(first.artifact_path) ||
        first.product.specular_rgba16f.size()!=504U || first.product.irradiance_rgba16f.size()!=96U ||
        first.product.brdf_lut_rg16f.size()!=32U) {
        std::cerr<<"First deterministic IBL Cook did not produce the expected cache artifact\n";
        std::filesystem::remove_all(cache_root); return 1;
    }

    const auto second=load_or_cook_split_sum_ibl(cache_root,nullptr,"asset.environment.test","builtin:test/1",profile);
    if (!second.product.valid || !second.cache_hit || second.product.specular_rgba16f!=first.product.specular_rgba16f ||
        second.artifact_bytes!=first.artifact_bytes) {
        std::cerr<<"Second IBL Cook did not reuse an identical verified artifact\n";
        std::filesystem::remove_all(cache_root); return 2;
    }

    { std::ofstream corrupt(second.artifact_path,std::ios::binary|std::ios::trunc); corrupt<<"corrupt"; }
    const auto rebuilt=load_or_cook_split_sum_ibl(cache_root,nullptr,"asset.environment.test","builtin:test/1",profile);
    if (!rebuilt.product.valid || rebuilt.cache_hit || !rebuilt.cache_rebuilt ||
        rebuilt.product.specular_rgba16f!=first.product.specular_rgba16f) {
        std::cerr<<"Corrupt IBL cache was not detected and rebuilt deterministically\n";
        std::filesystem::remove_all(cache_root); return 3;
    }
    std::filesystem::remove_all(cache_root);
    return 0;
}
