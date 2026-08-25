// Noemancer screen-space reflection trace.
//
// This is the first, deliberately bounded production slice of SSR.  It uses
// the shared RG32F depth pyramid as a conservative coarse rejection, then
// validates a candidate against the full-resolution depth buffer.  Keeping
// the validation in this pass prevents a broad HiZ interval from becoming a
// visible false reflection on thin geometry or odd-sized render targets.
//
// SDL_GPU graphics ABI (space2 sampled textures, space3 constants):
//   t0/s0 = scene HDR (RGBA16F), sampled for hit radiance
//   t1/s1 = device depth (D32/SFLOAT), current surface and hit validation
//   t2/s2 = shared RG32F depth pyramid; .x=min and .y=max linear view depth
//   t3/s3 = world normal (RGBA16F; xyz normal, a currently reserved)
//   t4/s4 = surface reflection properties (RGBA16F; rgb F0, a roughness)
//   b0    = SsrTraceSettings (256 bytes; see field comments below)
//
// The current pass writes one RGBA16F target: rgb is hit scene radiance and a
// is confidence.  A miss is represented by (0, 0, 0, 0); the composite pass
// therefore preserves the existing specular fallback instead of adding a
// black or guessed environment color.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> sceneHdr : register(t0, space2);
SamplerState sceneSampler : register(s0, space2);
Texture2D<float> sceneDepth : register(t1, space2);
SamplerState depthSampler : register(s1, space2);
Texture2D<float2> depthPyramid : register(t2, space2);
SamplerState pyramidSampler : register(s2, space2);
Texture2D<float4> worldNormal : register(t3, space2);
SamplerState normalSampler : register(s3, space2);
Texture2D<float4> reflectionProperties : register(t4, space2);
SamplerState propertiesSampler : register(s4, space2);

cbuffer SsrTraceSettings : register(b0, space3)
{
    // Matrices use the row-vector convention shared by the existing renderer.
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 cameraForward;

    // xy = physical resolution, zw = reciprocal resolution.
    float4 resolution;
    // x = near clip, y = far clip, z = world-space thickness, w = max ray distance.
    float4 depthAndDistance;
    // x = maximum ray steps, y = highest HiZ mip, z = roughness cutoff, w = enabled.
    float4 quality;
    // x = debug mode, y = frame number, z = edge fade in pixels, w = fallback mode.
    float4 debug;
    // x = start mip, y = binary refinement steps, z = initial step pixels,
    // w = mip bias.
    float4 rayPolicy;
    // x/y = normalized screen edge fade start/end, z = minimum confidence,
    // w = reserved.
    float4 edgePolicy;
};

static const float EPSILON = 0.00001f;
static const float MAX_HDR = 65504.0f;
static const uint MAX_TRACE_STEPS = 128u;
static const uint MAX_BINARY_STEPS = 5u;

float finite_or(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float3 finite_color(float3 value)
{
    return min(max(float3(finite_or(value.x, 0.0f), finite_or(value.y, 0.0f),
        finite_or(value.z, 0.0f)), 0.0f), MAX_HDR);
}

float2 safe_resolution()
{
    return max(resolution.xy, float2(1.0f, 1.0f));
}

float linear_depth_from_device(float deviceDepth)
{
    const float nearClip = max(depthAndDistance.x, EPSILON);
    const float farClip = max(depthAndDistance.y, nearClip + EPSILON);
    const float denominator = max(farClip - saturate(deviceDepth) * (farClip - nearClip), EPSILON);
    return clamp(finite_or(nearClip * farClip / denominator, farClip), nearClip, farClip);
}

float3 reconstruct_world(float2 uv, float deviceDepth)
{
    const float2 safeUv = saturate(uv);
    // The renderer's clip-space Y is inverted for texture coordinates.
    float4 world = mul(inverseViewProjection,
        float4(safeUv.x * 2.0f - 1.0f, 1.0f - safeUv.y * 2.0f,
            saturate(deviceDepth), 1.0f));
    const float safeW = abs(world.w) > EPSILON
        ? world.w
        : (world.w < 0.0f ? -EPSILON : EPSILON);
    return world.xyz / safeW;
}

bool project_world(float3 world, out float2 uv, out float deviceDepth)
{
    float4 clip = mul(viewProjection, float4(world, 1.0f));
    if (!isfinite(clip.x) || !isfinite(clip.y) || !isfinite(clip.z) ||
        !isfinite(clip.w) || abs(clip.w) <= EPSILON)
    {
        uv = 0.0f.xx;
        deviceDepth = 1.0f;
        return false;
    }
    const float3 ndc = clip.xyz / clip.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    deviceDepth = ndc.z;
    return all(isfinite(uv)) && isfinite(deviceDepth);
}

float edge_fade(float2 uv)
{
    const float edgeCoordinate = max(abs(uv.x * 2.0f - 1.0f), abs(uv.y * 2.0f - 1.0f));
    const float fadeStart = saturate(edgePolicy.x);
    const float fadeEnd = max(saturate(edgePolicy.y), fadeStart + EPSILON);
    return 1.0f - smoothstep(fadeStart, fadeEnd, edgeCoordinate);
}

bool valid_pyramid_sample(float2 range)
{
    return all(isfinite(range)) && range.y >= range.x && range.y > 0.0f;
}

float roughness_validity(float4 properties, out float roughness, out float3 f0)
{
    f0 = saturate(float3(finite_or(properties.x, 0.04f), finite_or(properties.y, 0.04f),
        finite_or(properties.z, 0.04f)));
    roughness = finite_or(properties.w, 1.0f);
    const bool valid = isfinite(properties.w) && roughness >= 0.0f && roughness <= 1.0f;
    roughness = saturate(roughness);
    return valid && roughness <= saturate(quality.z) && quality.w > 0.5f ? 1.0f : 0.0f;
}

struct TraceResult
{
    float3 radiance;
    float confidence;
    float hitMip;
    float hitDepth;
    bool hit;
};

TraceResult miss_result()
{
    TraceResult result;
    result.radiance = 0.0f.xxx;
    result.confidence = 0.0f;
    result.hitMip = 0.0f;
    result.hitDepth = 0.0f;
    result.hit = false;
    return result;
}

TraceResult trace_reflection(float2 uv, float deviceDepth, float3 normal,
    float roughness)
{
    TraceResult result = miss_result();
    const float3 surface = reconstruct_world(uv, deviceDepth);
    const float3 toView = normalize(cameraPosition.xyz - surface);
    if (!all(isfinite(surface)) || !all(isfinite(toView)) || dot(normal, toView) <= 0.0f)
        return result;

    const float3 rayDirection = normalize(reflect(-toView, normalize(normal)));
    if (!all(isfinite(rayDirection)) || dot(rayDirection, normal) <= 0.0f)
        return result;

    const float thickness = max(depthAndDistance.z, 0.0001f);
    const float maxDistance = max(depthAndDistance.w, thickness);
    const uint stepCount = min(max((uint)quality.x, 1u), MAX_TRACE_STEPS);
    const uint maxMip = min(max((uint)quality.y, 0u), 15u);
    const uint startMip = min((uint)max(rayPolicy.x, 0.0f), maxMip);
    const uint binarySteps = min((uint)max(rayPolicy.y, 0.0f), MAX_BINARY_STEPS);
    // The first implementation incorrectly treated initialStepPixels as a
    // multiplier for maxDistance / stepCount. At the high preset that skipped
    // the first 3.125 world units and missed nearby reflected geometry. Keep
    // the policy screen-space-like by using it only to scale a bounded near
    // step, then distribute the remaining samples quadratically.
    const float initialDistance = max(thickness * 0.5f,
        maxDistance / max(float(stepCount * stepCount), 1.0f)) *
        max(rayPolicy.z, 0.25f);
    // Reject the source surface until the reflected ray has separated by at
    // least two thicknesses along the geometric normal. Without this guard a
    // shallow ray immediately re-hits the same floor, samples its own color,
    // and never reaches reflected geometry.
    const float selfRejectDistance = max(initialDistance * 2.0f,
        thickness * 2.0f / max(abs(dot(rayDirection, normalize(normal))), 0.1f));
    const float3 origin = surface + normalize(normal) * (thickness * 0.5f);
    float2 previousUv = uv;
    float previousDistance = 0.0f;
    float previousDepthDelta = -thickness * 2.0f;

    [loop]
    for (uint step = 0u; step < MAX_TRACE_STEPS; ++step)
    {
        if (step >= stepCount)
            break;
        // A mild quadratic spacing gives the near field more detail while the
        // explicit step cap keeps the pass predictable on every backend.
        const float normalizedStep = (float(step) + 1.0f) / float(stepCount);
        const float distanceAlongRay = min(maxDistance,
            initialDistance + (maxDistance - initialDistance) *
                normalizedStep * normalizedStep);
        const float3 samplePoint = origin + rayDirection * distanceAlongRay;
        float2 sampleUv;
        float sampleDeviceDepth;
        if (!project_world(samplePoint, sampleUv, sampleDeviceDepth) ||
            any(sampleUv <= 0.0f) || any(sampleUv >= 1.0f) || sampleDeviceDepth <= 0.0f || sampleDeviceDepth >= 1.0f)
            break;

        const float rayDepth = dot(samplePoint - cameraPosition.xyz, normalize(cameraForward.xyz));
        if (!isfinite(rayDepth) || rayDepth <= depthAndDistance.x || rayDepth >= depthAndDistance.y)
            break;

        const float pixelTravel = length((sampleUv - previousUv) * safe_resolution());
        const uint mip = clamp((uint)max(log2(max(pixelTravel, 1.0f)) + rayPolicy.w, 0.0f), startMip, maxMip);
        const float2 pyramidRange = depthPyramid.SampleLevel(pyramidSampler, sampleUv, float(mip));
        if (valid_pyramid_sample(pyramidRange))
        {
            const float conservativeThickness = thickness * (1.0f + roughness * 2.0f);
            const bool inPyramidInterval = rayDepth + conservativeThickness >= pyramidRange.x &&
                rayDepth - conservativeThickness <= pyramidRange.y;
            const bool crossedInterval = rayDepth > pyramidRange.y + conservativeThickness;
            if (inPyramidInterval || crossedInterval)
            {
                // Full-resolution validation is the final authority.  HiZ is
                // used for early rejection and LOD selection, never as a
                // license to accept an unverified coarse hit.
                const float hitDeviceDepth = sceneDepth.SampleLevel(depthSampler, sampleUv, 0.0f);
                const float hitLinearDepth = linear_depth_from_device(hitDeviceDepth);
                const float depthDelta = rayDepth - hitLinearDepth;
                const float depthError = abs(depthDelta);
                const float hitTolerance = thickness + max(hitLinearDepth * 0.0025f, 0.001f);
                const bool crossedVisibleSurface = previousDepthDelta < -hitTolerance &&
                    depthDelta >= -hitTolerance;
                if (distanceAlongRay >= selfRejectDistance &&
                    hitDeviceDepth > 0.0f && hitDeviceDepth < 1.0f &&
                    (depthError <= hitTolerance || crossedVisibleSurface))
                {
                    float2 refinedUv = sampleUv;
                    float3 refinedPoint = samplePoint;
                    // A short binary refinement reduces the depth error without
                    // introducing an unbounded loop or a second authority.
                    float lowDistance = previousDistance;
                    float highDistance = distanceAlongRay;
                    [loop]
                    for (uint refine = 0u; refine < MAX_BINARY_STEPS; ++refine)
                    {
                        if(refine >= binarySteps) break;
                        const float midDistance = 0.5f * (lowDistance + highDistance);
                        const float3 midPoint = origin + rayDirection * midDistance;
                        float2 midUv;
                        float midDeviceDepth;
                        if (!project_world(midPoint, midUv, midDeviceDepth))
                            break;
                        const float midSurfaceDepth = linear_depth_from_device(
                            sceneDepth.SampleLevel(depthSampler, saturate(midUv), 0.0f));
                        const float midRayDepth = dot(midPoint - cameraPosition.xyz, normalize(cameraForward.xyz));
                        refinedUv = midUv;
                        refinedPoint = midPoint;
                        if (midRayDepth > midSurfaceDepth)
                            highDistance = midDistance;
                        else
                            lowDistance = midDistance;
                    }

                    const float refinedEdge = edge_fade(refinedUv);
                    const float facing = saturate(dot(normalize(normal), toView));
                    const float roughnessFade = saturate(1.0f - roughness * roughness);
                    result.radiance = finite_color(sceneHdr.SampleLevel(sceneSampler, saturate(refinedUv), 0.0f).rgb);
                    result.confidence = saturate(refinedEdge * facing * roughnessFade);
                    if(result.confidence < saturate(edgePolicy.z))result.confidence=0.0f;
                    result.hitMip = float(mip);
                    result.hitDepth = dot(refinedPoint - cameraPosition.xyz, normalize(cameraForward.xyz));
                    result.hit = result.confidence > 0.0001f;
                    return result;
                }
                previousDepthDelta = depthDelta;
            }
        }
        previousUv = sampleUv;
        previousDistance = distanceAlongRay;
    }
    return result;
}

float4 debug_trace(TraceResult result, float roughness, float validity, float3 normal)
{
    const float mode = debug.x;
    if (mode > 0.5f && mode < 1.5f)
        return float4(result.confidence.xxx, result.confidence);
    if (mode > 1.5f && mode < 2.5f)
        return float4(saturate(result.hitDepth / max(depthAndDistance.w, EPSILON)).xxx,
            result.confidence);
    if (mode > 2.5f && mode < 3.5f)
        return float4(roughness.xxx, validity);
    if (mode > 3.5f && mode < 4.5f)
        return result.hit ? float4(0.05f, 0.85f, 0.15f, result.confidence)
                          : float4(0.85f, 0.03f, 0.02f, 0.0f);
    if (mode > 4.5f && mode < 5.5f)
        return float4(normalize(normal) * 0.5f + 0.5f, validity);
    return float4(result.radiance, result.confidence);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = saturate(input.texcoord);
    const float deviceDepth = sceneDepth.SampleLevel(depthSampler, uv, 0.0f);
    const float4 properties = reflectionProperties.SampleLevel(propertiesSampler, uv, 0.0f);
    float roughness;
    float3 f0;
    const float validity = roughness_validity(properties, roughness, f0);
    const float3 sampledNormal = worldNormal.SampleLevel(normalSampler, uv, 0.0f).xyz;
    const float sampledNormalLength = dot(sampledNormal, sampledNormal);
    const float3 surface = reconstruct_world(uv, deviceDepth);
    float3 reconstructedNormal = cross(ddx(surface), ddy(surface));
    const float reconstructedLength = dot(reconstructedNormal, reconstructedNormal);
    reconstructedNormal = reconstructedLength > EPSILON
        ? reconstructedNormal * rsqrt(reconstructedLength)
        : float3(0.0f, 1.0f, 0.0f);
    const float3 toView = normalize(cameraPosition.xyz - surface);
    if (dot(reconstructedNormal, toView) < 0.0f)
        reconstructedNormal = -reconstructedNormal;
    // Imported and procedural paths normally provide a world-space normal.
    // Depth derivatives are a deterministic fallback for formats/drivers that
    // return a cleared auxiliary MRT sample, and preserve the material buffer
    // as the sole eligibility authority.
    float3 normal = sampledNormalLength > 0.25f && isfinite(sampledNormalLength)
        ? normalize(sampledNormal)
        : reconstructedNormal;
    if (dot(normal, toView) < 0.0f)
        normal = -normal;
    if (validity < 0.5f || deviceDepth <= 0.0f || deviceDepth >= 1.0f ||
        !all(isfinite(normal)) || dot(normal, normal) < 0.25f)
    {
        TraceResult empty = miss_result();
        return debug_trace(empty, roughness, validity, normal);
    }
    return debug_trace(trace_reflection(uv, deviceDepth, normal, roughness), roughness, validity, normal);
}
