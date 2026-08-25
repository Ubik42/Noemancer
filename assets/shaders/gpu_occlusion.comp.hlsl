// Noemancer GPU-driven visibility: conservative hierarchical-Z occlusion.
//
// This is an optional successor pass to gpu_visibility.comp.  The original
// shader remains the frustum-only ABI, so existing callers can keep using it
// unchanged.  A caller that wants HiZ occlusion binds this shader with the
// same instance/batch/visible/indirect buffers and the additional resources
// documented below.
//
// ABI (SDL_GPU compute):
//   t0/s0, space0 = RG32F min/max depth pyramid plus a point-clamp sampler.
//               Load().x is the conservative minimum linear view depth and
//               Load().y the maximum.  The explicit Load is the proof source;
//               the sampler is retained for SDL's texture-sampler binding ABI.
//   t1, space0 = StructuredBuffer<GpuDrivenInstance>
//   t2, space0 = StructuredBuffer<GpuDrivenBatch>
//   u0, space1 = visible instance indices (same append protocol as the
//               frustum-only pass)
//   u1, space1 = indexed indirect commands (instance_count at byte +4)
//   u2, space1 = uint[8] statistics counters; see stat_* constants below.
//   b0, space2 = GpuOcclusionParameters (224 bytes).
//
// GpuOcclusionParameters:
//   frustumPlanes[6]          96 bytes, identical plane equations to the
//                              existing gpu_visibility.comp ABI.
//   viewProjection             64 bytes, the renderer's mul(matrix, vector)
//                              world-to-clip convention.
//   viewport                   xy = physical width/height, zw = reciprocal.
//   depthParameters            x = near clip, y = far clip, z = reversed-Z,
//                              w = non-negative world/view-depth bias.
//   occlusionParameters        x = enable HiZ (0 = frustum-only),
//                              y = requested highest HiZ mip,
//                              z = available HiZ mip count,
//                              w = enable statistics writes.
//   dispatchParameters         x = candidate count, y = batch count,
//                              z = ABI version (1), w = reserved.
//
// The pass is deliberately conservative.  Invalid matrices, invalid bounds,
// near-plane crossing, offscreen projected bounds, malformed HiZ pairs, and
// any footprint that cannot be covered by the selected mip all remain
// visible.  Occlusion only occurs when the farthest depth in every HiZ texel
// touched by the projected sphere is closer than the sphere's nearest depth.
// This uses the .y (maximum) member of the RG32F hierarchy; using .x here
// would permit false culls when one texel contains an opening.

struct GpuDrivenInstance
{
    float4x4 model;
    float4x4 previousModel;
    float4 color;
    float4 material;
    float4 emissiveNormal;
    float4 occlusionAlphaFlags;
    uint4 objectIdentity;
    float4 bounds;
};

struct GpuDrivenBatch
{
    uint candidateOffset;
    uint candidateCount;
    uint visibleOffset;
    uint padding;
};

Texture2D<float2> depthPyramid : register(t0, space0);
SamplerState depthPyramidSampler : register(s0, space0);
StructuredBuffer<GpuDrivenInstance> instances : register(t1, space0);
StructuredBuffer<GpuDrivenBatch> batches : register(t2, space0);

RWStructuredBuffer<uint> visibleIndices : register(u0, space1);
RWByteAddressBuffer indirectCommands : register(u1, space1);
RWStructuredBuffer<uint> visibilityStats : register(u2, space1);

cbuffer GpuOcclusionParameters : register(b0, space2)
{
    float4 frustumPlanes[6];
    float4x4 viewProjection;
    float4 viewport;
    float4 depthParameters;
    float4 occlusionParameters;
    uint4 dispatchParameters;
};

static const float EPSILON = 0.00001f;
static const uint OCCLUSION_ABI_VERSION = 1u;
static const uint MAX_SPHERE_CORNERS = 8u;
static const uint MAX_HIZ_FOOTPRINT = 4u;

// Counter ABI: all counters are uint and are reset by the owner before the
// dispatch. Counter 0 includes all candidates; STAT_HIZ_TESTED is an attempt
// metric and therefore overlaps STAT_HIZ_CULLED or STAT_ACCEPTED_VISIBLE.
// The remaining outcome counters describe the conservative terminal path.
static const uint STAT_CANDIDATES = 0u;
static const uint STAT_FRUSTUM_CULLED = 1u;
static const uint STAT_HIZ_TESTED = 2u;
static const uint STAT_HIZ_CULLED = 3u;
static const uint STAT_INVALID_VISIBLE = 4u;
static const uint STAT_OFFSCREEN_VISIBLE = 5u;
static const uint STAT_DISABLED_VISIBLE = 6u;
static const uint STAT_ACCEPTED_VISIBLE = 7u;

float finite_or(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

bool finite_float2(float2 value)
{
    return all(isfinite(value));
}

bool finite_float3(float3 value)
{
    return all(isfinite(value));
}

bool finite_float4(float4 value)
{
    return all(isfinite(value));
}

void stat_add(uint counter)
{
    if (occlusionParameters.w > 0.5f && counter < 8u)
    {
        uint previous;
        InterlockedAdd(visibilityStats[counter], 1u, previous);
    }
}

bool frustum_visible(float4 bounds)
{
    if (!finite_float4(bounds) || bounds.w < 0.0f)
        return true;

    [unroll]
    for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        const float4 plane = frustumPlanes[planeIndex];
        if (dot(plane.xyz, bounds.xyz) + plane.w < -bounds.w)
            return false;
    }
    return true;
}

bool static_transform(GpuDrivenInstance instance)
{
    // Previous-frame HiZ is only authoritative for geometry whose transform
    // did not change since that depth was produced. Moving instances stay
    // visible until a future reprojection/motion-bounds contract exists.
    [unroll]
    for (uint row = 0u; row < 4u; ++row)
    {
        if (any(abs(instance.model[row] - instance.previousModel[row]) > 0.000001f))
            return false;
    }
    return true;
}

// Frustum planes are intentionally stored in the first six float4 registers
// of the parameters ABI.  The helper is kept separate so the implementation
// can be switched to the legacy VisibilityParameters block without changing
// the testable cull predicate.
struct ProjectedSphere
{
    float2 uvMin;
    float2 uvMax;
    float nearestDepth;
    float farthestDepth;
    uint status;
};

static const uint PROJECTED_VALID = 0u;
static const uint PROJECTED_INVALID = 1u;
static const uint PROJECTED_OFFSCREEN = 2u;
static const uint PROJECTED_UNCERTAIN = 3u;

float safe_near_clip()
{
    return max(finite_or(depthParameters.x, EPSILON), EPSILON);
}

float safe_far_clip(float nearClip)
{
    return max(finite_or(depthParameters.y, nearClip + 1.0f), nearClip + EPSILON);
}

float linear_view_depth(float deviceDepth)
{
    const float nearClip = safe_near_clip();
    const float farClip = safe_far_clip(nearClip);
    float z = saturate(finite_or(deviceDepth, 1.0f));
    if (depthParameters.z > 0.5f)
        z = 1.0f - z;
    const float denominator = max(farClip - z * (farClip - nearClip), EPSILON);
    return clamp(finite_or(nearClip * farClip / denominator, farClip),
        nearClip, farClip);
}

ProjectedSphere project_sphere(float3 center, float radius)
{
    ProjectedSphere result;
    result.uvMin = 0.0f.xx;
    result.uvMax = 0.0f.xx;
    result.nearestDepth = 0.0f;
    result.farthestDepth = 0.0f;
    result.status = PROJECTED_INVALID;

    if (!finite_float3(center) || !isfinite(radius) || radius < 0.0f ||
        !finite_float4(viewport) || viewport.x < 1.0f || viewport.y < 1.0f)
        return result;

    float2 uvMin = float2(1.0e30f, 1.0e30f);
    float2 uvMax = float2(-1.0e30f, -1.0e30f);
    float nearestDepth = 1.0e30f;
    float farthestDepth = -1.0e30f;

    [unroll]
    for (uint corner = 0u; corner < MAX_SPHERE_CORNERS; ++corner)
    {
        const float3 sign = float3(
            (corner & 1u) != 0u ? 1.0f : -1.0f,
            (corner & 2u) != 0u ? 1.0f : -1.0f,
            (corner & 4u) != 0u ? 1.0f : -1.0f);
        const float4 clip = mul(viewProjection, float4(center + sign * radius, 1.0f));
        if (!finite_float4(clip) || clip.w <= EPSILON)
            return result;

        const float3 ndc = clip.xyz / clip.w;
        if (!finite_float3(ndc) || ndc.z < 0.0f || ndc.z > 1.0f)
        {
            result.status = PROJECTED_UNCERTAIN;
            return result;
        }

        const float2 uv = float2(ndc.x * 0.5f + 0.5f,
            0.5f - ndc.y * 0.5f);
        if (!finite_float2(uv))
            return result;
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        const float depth = linear_view_depth(ndc.z);
        if (!isfinite(depth))
            return result;
        nearestDepth = min(nearestDepth, depth);
        farthestDepth = max(farthestDepth, depth);
    }

    if (!finite_float2(uvMin) || !finite_float2(uvMax) ||
        !isfinite(nearestDepth) || !isfinite(farthestDepth) ||
        any(uvMax <= uvMin) || nearestDepth > farthestDepth)
        return result;

    result.uvMin = uvMin;
    result.uvMax = uvMax;
    result.nearestDepth = nearestDepth;
    result.farthestDepth = farthestDepth;
    if (uvMin.x < 0.0f || uvMin.y < 0.0f || uvMax.x > 1.0f || uvMax.y > 1.0f)
    {
        result.status = PROJECTED_OFFSCREEN;
        return result;
    }
    result.status = PROJECTED_VALID;
    return result;
}

uint ceil_log2_positive(float value)
{
    if (!isfinite(value) || value <= 1.0f)
        return 0u;
    return min((uint)ceil(log2(value)), 30u);
}

uint mip_dimension(uint baseDimension, uint mip)
{
    const uint shift = min(mip, 30u);
    const uint divisor = 1u << shift;
    return max((baseDimension + divisor - 1u) / divisor, 1u);
}

uint declared_mip_count()
{
    const float authored = finite_or(occlusionParameters.z, 0.0f);
    if (!isfinite(authored) || authored < 1.0f)
        return 0u;
    return min((uint)authored, 31u);
}

bool load_hiz_max_depth(uint2 coordinate, uint mip, out float maximumDepth)
{
    uint baseWidth;
    uint baseHeight;
    uint actualMipCount;
    depthPyramid.GetDimensions(0u, baseWidth, baseHeight, actualMipCount);
    const uint authoredMipCount = declared_mip_count();
    const uint availableMips = min(min(max(actualMipCount, 1u), 31u), authoredMipCount);
    if (baseWidth == 0u || baseHeight == 0u || availableMips == 0u || mip >= availableMips)
    {
        maximumDepth = 0.0f;
        return false;
    }

    const uint width = mip_dimension(baseWidth, mip);
    const uint height = mip_dimension(baseHeight, mip);
    if (coordinate.x >= width || coordinate.y >= height)
    {
        maximumDepth = 0.0f;
        return false;
    }

    // Keep the SDL texture-sampler binding live without using filtered data
    // in the cull proof.  A malformed sampled value is uncertainty, never a
    // reason to cull.  The point-clamp sampler is still part of the ABI so the
    // same binding path can be used on D3D12 and Vulkan.
    const float2 probe = depthPyramid.SampleLevel(depthPyramidSampler,
        (float2(coordinate) + 0.5f) / float2(width, height), float(mip));
    if (!finite_float2(probe))
    {
        maximumDepth = 0.0f;
        return false;
    }
    const float2 pair = depthPyramid.Load(int3(coordinate, mip));
    if (!finite_float2(pair) || pair.x < safe_near_clip() - EPSILON ||
        pair.y < pair.x || pair.y > safe_far_clip(safe_near_clip()) + EPSILON)
    {
        maximumDepth = 0.0f;
        return false;
    }
    maximumDepth = pair.y;
    return isfinite(maximumDepth);
}

bool hiz_occluded(ProjectedSphere sphere)
{
    if (sphere.status != PROJECTED_VALID)
        return false;

    uint baseWidth;
    uint baseHeight;
    uint actualMipCount;
    depthPyramid.GetDimensions(0u, baseWidth, baseHeight, actualMipCount);
    const uint authoredMipCount = declared_mip_count();
    const uint availableMips = min(min(max(actualMipCount, 1u), 31u), authoredMipCount);
    if (baseWidth == 0u || baseHeight == 0u || availableMips == 0u)
        return false;
    const uint requestedMip = min((uint)max(finite_or(occlusionParameters.y, 0.0f), 0.0f),
        availableMips - 1u);
    const float2 pixelExtent = (sphere.uvMax - sphere.uvMin) * viewport.xy;
    if (!finite_float2(pixelExtent) || any(pixelExtent <= 0.0f))
        return false;

    const uint requiredMip = ceil_log2_positive(max(pixelExtent.x, pixelExtent.y));
    const uint mip = min(requiredMip, requestedMip);
    const uint width = mip_dimension(baseWidth, mip);
    const uint height = mip_dimension(baseHeight, mip);
    if (width == 0u || height == 0u)
        return false;

    // The selected level normally covers the whole rectangle in one texel.
    // Enumerate the exact touched texels anyway; if numerical rounding makes
    // the footprint larger than the bounded loop, remain visible.
    int2 minimumCoordinate = (int2)floor(sphere.uvMin * float2(width, height));
    int2 maximumCoordinate = (int2)ceil(sphere.uvMax * float2(width, height)) - 1;
    minimumCoordinate = clamp(minimumCoordinate, int2(0, 0),
        int2((int)width - 1, (int)height - 1));
    maximumCoordinate = clamp(maximumCoordinate, int2(0, 0),
        int2((int)width - 1, (int)height - 1));
    const int2 footprint = maximumCoordinate - minimumCoordinate + 1;
    if (any(footprint <= 0) || footprint.x > MAX_HIZ_FOOTPRINT ||
        footprint.y > MAX_HIZ_FOOTPRINT)
        return false;

    float maximumOccluderDepth = -1.0f;
    [loop]
    for (int y = minimumCoordinate.y; y <= maximumCoordinate.y; ++y)
    {
        [loop]
        for (int x = minimumCoordinate.x; x <= maximumCoordinate.x; ++x)
        {
            float sampleMaximum;
            if (!load_hiz_max_depth(uint2(x, y), mip, sampleMaximum))
                return false;
            maximumOccluderDepth = max(maximumOccluderDepth, sampleMaximum);
        }
    }

    if (!isfinite(maximumOccluderDepth) || maximumOccluderDepth < safe_near_clip())
        return false;
    const float bias = max(finite_or(depthParameters.w, 0.0f), 0.0f);
    // Strictly farther than the farthest depth in every touched HiZ texel is
    // the only cull condition.  Equality remains visible for rasterisation
    // and floating-point safety.
    return sphere.nearestDepth > maximumOccluderDepth + bias;
}

void append_visible(uint candidateIndex, uint batchIndex, GpuDrivenBatch batch)
{
    uint localVisibleIndex;
    indirectCommands.InterlockedAdd(batchIndex * 20u + 4u, 1u, localVisibleIndex);
    if (localVisibleIndex < batch.candidateCount)
        visibleIndices[batch.visibleOffset + localVisibleIndex] = candidateIndex;
    stat_add(STAT_ACCEPTED_VISIBLE);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;
    const uint candidateCount = dispatchParameters.x;
    const uint batchCount = dispatchParameters.y;
    if (candidateIndex >= candidateCount)
        return;
    stat_add(STAT_CANDIDATES);

    const GpuDrivenInstance instance = instances[candidateIndex];
    const float4 bounds = instance.bounds;
    if (!frustum_visible(bounds))
    {
        stat_add(STAT_FRUSTUM_CULLED);
        return;
    }

    const uint batchIndex = instance.objectIdentity.w;
    if (batchIndex >= batchCount)
    {
        stat_add(STAT_INVALID_VISIBLE);
        return;
    }
    const GpuDrivenBatch batch = batches[batchIndex];

    const bool occlusionEnabled = occlusionParameters.x > 0.5f &&
        dispatchParameters.z == OCCLUSION_ABI_VERSION;
    if (!occlusionEnabled || !static_transform(instance))
    {
        stat_add(STAT_DISABLED_VISIBLE);
        append_visible(candidateIndex, batchIndex, batch);
        return;
    }

    const ProjectedSphere projected = project_sphere(bounds.xyz, bounds.w);
    if (projected.status == PROJECTED_OFFSCREEN)
    {
        stat_add(STAT_OFFSCREEN_VISIBLE);
        append_visible(candidateIndex, batchIndex, batch);
        return;
    }
    if (projected.status != PROJECTED_VALID)
    {
        stat_add(STAT_INVALID_VISIBLE);
        append_visible(candidateIndex, batchIndex, batch);
        return;
    }

    stat_add(STAT_HIZ_TESTED);
    if (hiz_occluded(projected))
    {
        stat_add(STAT_HIZ_CULLED);
        return;
    }
    append_visible(candidateIndex, batchIndex, batch);
}
