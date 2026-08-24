#pragma once

#include <array>

namespace noemancer {

struct VisibilitySphere final {
    std::array<float,3> center{};
    float radius{};
};

struct VisibilityPlane final {
    std::array<float,3> normal{};
    float distance{};
};

struct VisibilityFrustum final {
    bool valid{};
    std::array<VisibilityPlane,6> planes{};
};

[[nodiscard]] VisibilityFrustum extract_visibility_frustum(const std::array<float,16>& column_major_view_projection);
[[nodiscard]] bool sphere_intersects_frustum(const VisibilityFrustum& frustum,const VisibilitySphere& sphere);

} // namespace noemancer
