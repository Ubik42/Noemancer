#include "engine/visibility_culling.hpp"

#include <cmath>

namespace noemancer {

VisibilityFrustum extract_visibility_frustum(const std::array<float,16>& matrix) {
    VisibilityFrustum result;
    const auto row=[&](const int index){return std::array<float,4>{matrix[index],matrix[4+index],matrix[8+index],matrix[12+index]};};
    const auto r0=row(0),r1=row(1),r2=row(2),r3=row(3);
    const std::array<std::array<float,4>,6> source{{
        {r3[0]+r0[0],r3[1]+r0[1],r3[2]+r0[2],r3[3]+r0[3]},
        {r3[0]-r0[0],r3[1]-r0[1],r3[2]-r0[2],r3[3]-r0[3]},
        {r3[0]+r1[0],r3[1]+r1[1],r3[2]+r1[2],r3[3]+r1[3]},
        {r3[0]-r1[0],r3[1]-r1[1],r3[2]-r1[2],r3[3]-r1[3]},
        {r2[0],r2[1],r2[2],r2[3]},
        {r3[0]-r2[0],r3[1]-r2[1],r3[2]-r2[2],r3[3]-r2[3]}}};
    for (std::size_t index=0;index<source.size();++index) {
        const auto& plane=source[index]; const float length=std::sqrt(plane[0]*plane[0]+plane[1]*plane[1]+plane[2]*plane[2]);
        if (!std::isfinite(length)||length<1.0e-8F) return result;
        result.planes[index]={{plane[0]/length,plane[1]/length,plane[2]/length},plane[3]/length};
    }
    result.valid=true; return result;
}

bool sphere_intersects_frustum(const VisibilityFrustum& frustum,const VisibilitySphere& sphere) {
    if (!frustum.valid||!std::isfinite(sphere.radius)||sphere.radius<0.0F) return false;
    for (const auto& plane:frustum.planes) {
        const float distance=plane.normal[0]*sphere.center[0]+plane.normal[1]*sphere.center[1]+plane.normal[2]*sphere.center[2]+plane.distance;
        if (distance < -sphere.radius) return false;
    }
    return true;
}

} // namespace noemancer
