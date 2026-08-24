#include "engine/visibility_culling.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

int main() {
    using namespace noemancer;
    const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const auto frustum=extract_visibility_frustum(identity);
    if (!frustum.valid || !sphere_intersects_frustum(frustum,{{0,0,0.5F},0.1F}) ||
        sphere_intersects_frustum(frustum,{{2,0,0.5F},0.1F}) ||
        sphere_intersects_frustum(frustum,{{0,0,-0.2F},0.1F}) ||
        !sphere_intersects_frustum(frustum,{{1.05F,0,0.5F},0.1F})) {
        std::cerr<<"D3D clip-space frustum sphere classification failed\n"; return 1;
    }

    struct Candidate final {
        std::uint32_t id{};
        VisibilitySphere sphere{};
    };
    const std::array<Candidate,20> candidates{{
        {0,{{0.0F,0.0F,0.5F},0.10F}},
        {1,{{0.5F,0.0F,0.5F},0.20F}},
        {2,{{-0.75F,0.0F,0.25F},0.20F}},
        {3,{{0.0F,0.75F,0.5F},0.25F}},
        {4,{{0.0F,-0.70F,0.90F},0.20F}},
        {5,{{1.25F,0.0F,0.5F},0.25F}},
        {6,{{-1.25F,0.0F,0.5F},0.25F}},
        {7,{{0.0F,0.0F,-0.25F},0.25F}},
        {8,{{0.0F,0.0F,1.25F},0.25F}},
        {9,{{1.26F,0.0F,0.5F},0.25F}},
        {10,{{-1.26F,0.0F,0.5F},0.25F}},
        {11,{{0.0F,1.26F,0.5F},0.25F}},
        {12,{{0.0F,-1.26F,0.5F},0.25F}},
        {13,{{0.0F,0.0F,-0.26F},0.25F}},
        {14,{{0.0F,0.0F,1.26F},0.25F}},
        {15,{{2.0F,0.0F,0.5F},0.10F}},
        {16,{{0.0F,2.0F,0.5F},0.10F}},
        {17,{{-0.25F,0.25F,0.5F},0.0F}},
        {18,{{0.99F,0.0F,0.5F},0.01F}},
        {19,{{0.0F,-0.99F,0.5F},0.01F}}
    }};
    std::vector<std::uint32_t> cpu_reference_ids;
    for (const auto& candidate : candidates)
        if (sphere_intersects_frustum(frustum,candidate.sphere)) cpu_reference_ids.push_back(candidate.id);
    const std::vector<std::uint32_t> expected_visible{0,1,2,3,4,5,6,7,8,17,18,19};
    if (cpu_reference_ids != expected_visible || cpu_reference_ids.size() != 12U) {
        std::cerr<<"Deterministic partial-visibility CPU oracle returned an unexpected candidate set\n";
        return 3;
    }
    const auto outside_count=candidates.size()-cpu_reference_ids.size();
    if (outside_count*4U < candidates.size() || outside_count*2U > candidates.size()) {
        std::cerr<<"Partial-visibility workload does not keep 25-50% of candidates outside the frustum\n";
        return 4;
    }

    auto degenerate=identity;
    degenerate[2]=degenerate[6]=degenerate[10]=degenerate[14]=0.0F;
    if (extract_visibility_frustum(degenerate).valid) {
        std::cerr<<"Frustum extraction accepted a matrix with a zero-length clip plane\n";
        return 5;
    }
    auto non_finite=identity;
    non_finite[0]=std::numeric_limits<float>::quiet_NaN();
    if (extract_visibility_frustum(non_finite).valid) {
        std::cerr<<"Frustum extraction accepted a non-finite projection matrix\n";
        return 6;
    }

    auto invalid=identity; invalid.fill(0.0F);
    if (extract_visibility_frustum(invalid).valid || sphere_intersects_frustum(extract_visibility_frustum(invalid),{{0,0,0},1})) {
        std::cerr<<"Degenerate frustum was not rejected\n"; return 2;
    }
    return 0;
}
