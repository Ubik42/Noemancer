// Noemancer screen-space diffuse GI gather.
//
// This pass is intentionally a bounded, visibility-aware radiance gather. It
// samples the already-lit scene, uses the shared RG32F min/max HiZ pyramid as
// a conservative line-of-sight check, and emits an irradiance estimate rather
// than modifying the scene in place. The composite pass later replaces only
// the eligible diffuse-IBL fallback, so a miss never becomes a second copy of
// the entire scene.
//
// SDL_GPU graphics ABI (space2 sampled textures, space3 constants):
//   t0/s0 = scene HDR (RGBA16F), source radiance samples
//   t1/s1 = device depth, current and sample surface depth
//   t2/s2 = shared RG32F depth pyramid; .x=min/.y=max linear view depth
//   t3/s3 = world normal (RGBA16F; xyz world normal)
//   t4/s4 = surface material properties (RGBA16F; rgb base color, a metallic)
//   b0    = SsgiGatherSettings (240 bytes; fields documented below)
//
// Target 0: RGB diffuse GI radiance/irradiance, A confidence.
// Target 1: RGB bent normal encoded to [0,1], A visibility/confidence.

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
Texture2D<float4> materialProperties : register(t4, space2);
SamplerState materialSampler : register(s4, space2);

cbuffer SsgiGatherSettings : register(b0, space3)
{
    // Matrices use the row-vector convention shared by the renderer.
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    // xyz = world-space camera position, w = maximum world-space ray distance.
    float4 cameraPosition;
    // xyz = normalized camera forward, w = maximum ray-march steps (<= 32).
    float4 cameraForward;
    // xy = physical resolution, zw = reciprocal resolution.
    float4 resolution;
    // x = near clip, y = far clip, z = world-space sample radius, w = thickness.
    float4 depthAndRadius;
    // x = maximum taps, y = maximum HiZ mip, z = material validity threshold, w = enabled.
    float4 quality;
    // x = debug mode, y = frame number, z = edge fade start, w = edge fade end.
    float4 debug;
    // x = normal bias, y = distance falloff, z = radiance power,
    // w = projected ray-step pixel scale.
    float4 gatherPolicy;
};

static const float EPSILON = 0.00001f;
static const float MAX_HDR = 65504.0f;
static const uint MAX_TAPS = 16u;
static const uint MAX_RAY_STEPS = 32u;

static const float2 SAMPLE_PATTERN[MAX_TAPS] = {
    float2(0.130526f, 0.991445f), float2(-0.500000f, 0.866025f),
    float2(-0.923880f, 0.382683f), float2(-0.991445f, -0.130526f),
    float2(-0.707107f, -0.707107f), float2(-0.130526f, -0.991445f),
    float2(0.500000f, -0.866025f), float2(0.923880f, -0.382683f),
    float2(0.353553f, 0.353553f), float2(-0.353553f, 0.353553f),
    float2(-0.353553f, -0.353553f), float2(0.353553f, -0.353553f),
    float2(0.707107f, 0.000000f), float2(0.000000f, 0.707107f),
    float2(-0.707107f, 0.000000f), float2(0.000000f, -0.707107f)
};

float finite_or(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float3 finite_color(float3 value)
{
    return min(max(float3(finite_or(value.x, 0.0f), finite_or(value.y, 0.0f),
        finite_or(value.z, 0.0f)), 0.0f), MAX_HDR);
}

float3 safe_normalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && isfinite(lengthSquared) && lengthSquared > EPSILON * EPSILON
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float2 safe_resolution()
{
    return max(float2(finite_or(resolution.x, 1.0f), finite_or(resolution.y, 1.0f)),
        float2(1.0f, 1.0f));
}

float linear_depth(float deviceDepth)
{
    const float nearClip = max(finite_or(depthAndRadius.x, 0.05f), EPSILON);
    const float farClip = max(finite_or(depthAndRadius.y, 1000.0f), nearClip + EPSILON);
    const float normalizedDepth = saturate(finite_or(deviceDepth, 1.0f));
    const float denominator = max(farClip - normalizedDepth * (farClip - nearClip), EPSILON);
    return clamp(finite_or(nearClip * farClip / denominator, farClip), nearClip, farClip);
}

float3 reconstruct_world(float2 uv, float deviceDepth)
{
    float4 world = mul(inverseViewProjection,
        float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f,
            saturate(deviceDepth), 1.0f));
    const float safeW = abs(world.w) > EPSILON
        ? world.w
        : (world.w < 0.0f ? -EPSILON : EPSILON);
    return world.xyz / safeW;
}

bool project_world(float3 world, out float2 uv, out float deviceDepth)
{
    const float4 clip = mul(viewProjection, float4(world, 1.0f));
    if (!all(isfinite(clip)) || abs(clip.w) <= EPSILON)
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

float hash01(uint2 value)
{
    uint hash = value.x * 1664525u + value.y * 1013904223u + 374761393u;
    hash ^= hash >> 16u;
    hash *= 2246822519u;
    hash ^= hash >> 13u;
    return float(hash) * 2.3283064365386963e-10f;
}

float2 rotate_sample(float2 sample, float angle)
{
    const float sine = sin(angle);
    const float cosine = cos(angle);
    return float2(sample.x * cosine - sample.y * sine,
        sample.x * sine + sample.y * cosine);
}

float edge_fade(float2 uv)
{
    const float edgeCoordinate = max(abs(uv.x * 2.0f - 1.0f), abs(uv.y * 2.0f - 1.0f));
    const float fadeStart = saturate(finite_or(debug.z, 0.65f));
    const float fadeEnd = max(saturate(finite_or(debug.w, 1.0f)), fadeStart + EPSILON);
    return 1.0f - smoothstep(fadeStart, fadeEnd, edgeCoordinate);
}

bool valid_material(float4 material, out float3 albedo, out float metallic)
{
    albedo = saturate(float3(finite_or(material.x, 0.0f), finite_or(material.y, 0.0f),
        finite_or(material.z, 0.0f)));
    metallic = saturate(finite_or(material.w, 1.0f));
    const bool valid = isfinite(material.w) && metallic >= 0.0f && metallic <= 1.0f;
    return valid && (1.0f - metallic) >= saturate(finite_or(quality.z, 1.0f));
}

bool valid_hiz_range(float2 range)
{
    return all(isfinite(range)) && range.y >= range.x && range.y > 0.0f;
}

float3 hemisphere_direction(float3 normal, float2 pattern, float radial)
{
    const float3 reference = abs(normal.y) < 0.999f
        ? float3(0.0f, 1.0f, 0.0f)
        : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = safe_normalize(cross(reference, normal), float3(1.0f, 0.0f, 0.0f));
    const float3 bitangent = safe_normalize(cross(normal, tangent), float3(0.0f, 0.0f, 1.0f));
    const float cosTheta = sqrt(saturate(1.0f - radial));
    const float sinTheta = sqrt(saturate(radial));
    return safe_normalize(normal * cosTheta +
        (tangent * pattern.x + bitangent * pattern.y) * sinTheta, normal);
}

struct GatherOutput
{
    float4 radiance : SV_Target0;
    float4 bentNormal : SV_Target1;
};

GatherOutput empty_output(float debugMode)
{
    GatherOutput output;
    if (debugMode > 0.5f && debugMode < 1.5f)
    {
        output.radiance = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.bentNormal = float4(0.5f, 0.5f, 1.0f, 0.0f);
    }
    else
    {
        output.radiance = 0.0f.xxxx;
        output.bentNormal = float4(0.5f, 0.5f, 1.0f, 0.0f);
    }
    return output;
}

GatherOutput main(FragmentInput input)
{
    const float2 uv = saturate(input.texcoord);
    const float deviceDepth = sceneDepth.SampleLevel(depthSampler, uv, 0.0f);
    const float4 material = materialProperties.SampleLevel(materialSampler, uv, 0.0f);
    float3 albedo;
    float metallic;
    const float enabled = finite_or(quality.w, 0.0f);
    if (!isfinite(deviceDepth) || deviceDepth <= 0.0f || deviceDepth >= 1.0f ||
        !valid_material(material, albedo, metallic) || enabled < 0.5f)
        return empty_output(debug.x);

    const float3 origin = reconstruct_world(uv, deviceDepth);
    const float3 rawNormal = worldNormal.SampleLevel(normalSampler, uv, 0.0f).xyz;
    const float normalLength = dot(rawNormal, rawNormal);
    if (!all(isfinite(origin)) || !isfinite(normalLength) || normalLength < 0.25f ||
        !all(isfinite(cameraPosition.xyz)))
        return empty_output(debug.x);
    const float3 normal = safe_normalize(rawNormal, float3(0.0f, 1.0f, 0.0f));
    const float3 cameraForwardDirection = safe_normalize(cameraForward.xyz,
        float3(0.0f, 0.0f, 1.0f));
    const uint tapCount = min((uint)max(finite_or(quality.x, 8.0f), 1.0f), MAX_TAPS);
    const uint maxMip = min((uint)max(finite_or(quality.y, 0.0f), 0.0f), 15u);
    const float thickness = max(finite_or(depthAndRadius.w, 0.05f), 0.0001f);
    const float radius = max(finite_or(depthAndRadius.z, 1.0f), thickness * 2.0f);
    const float maxDistance = max(finite_or(cameraPosition.w, radius), thickness * 2.0f);
    const float traceDistance = min(maxDistance, radius);
    const float defaultSteps = max(4.0f, min(32.0f, float(tapCount * 2u)));
    const uint rayStepCount = min((uint)max(finite_or(cameraForward.w, defaultSteps), 1.0f),
        MAX_RAY_STEPS);
    const float nearClip = max(finite_or(depthAndRadius.x, 0.05f), EPSILON);
    const float farClip = max(finite_or(depthAndRadius.y, 1000.0f), nearClip + EPSILON);
    const float normalBias = max(finite_or(gatherPolicy.x, 0.0f), thickness * 0.25f);
    const float distanceFalloff = max(finite_or(gatherPolicy.y, 0.1f), 0.0001f);
    const float radiancePower = max(finite_or(gatherPolicy.z, 1.0f), 0.0f);
    const float rayStepPixels = max(finite_or(gatherPolicy.w, 1.0f), 0.25f);
    const uint2 pixel = (uint2)(uv * safe_resolution());
    const uint frame = (uint)max(finite_or(debug.y, 0.0f), 0.0f);
    const float rotation = (hash01(pixel + uint2(frame, frame * 3u)) - 0.5f) * 6.28318530718f;
    const float3 originBiased = origin + normal * normalBias;

    float3 radianceSum = 0.0f.xxx;
    float3 bentDirectionSum = 0.0f.xxx;
    float radianceWeight = 0.0f;
    float validSamples = 0.0f;
    [loop]
    for (uint tap = 0u; tap < MAX_TAPS; ++tap)
    {
        if (tap >= tapCount)
            break;
        const uint2 tapSeed = pixel + uint2(tap * 17u + frame, tap * 31u + frame * 7u);
        const float2 pattern = rotate_sample(SAMPLE_PATTERN[tap], rotation +
            hash01(tapSeed) * 0.25f);
        const float radialJitter = (hash01(tapSeed + uint2(19u, 47u)) - 0.5f) * 0.12f;
        const float radial = saturate((float(tap) + 0.5f) / float(tapCount) + radialJitter);
        const float3 rayDirection = hemisphere_direction(normal, pattern, radial);
        const float originFacing = saturate(dot(normal, rayDirection));
        const float minHitDistance = max(thickness * 2.0f, normalBias * 2.0f);
        float previousDepthDelta = 0.0f;
        bool previousDepthValid = false;

        // March in world space, project every bounded step, and use the
        // shared min/max HiZ only to reject empty intervals. Full-resolution
        // device depth remains the hit authority for both D3D12 and Vulkan.
        [loop]
        for (uint step = 0u; step < MAX_RAY_STEPS; ++step)
        {
            if (step >= rayStepCount)
                break;
            const float stepFraction = (float(step) + 1.0f) / float(rayStepCount);
            // SSGI is a local diffuse bounce. Respect the authored gather
            // radius and bias samples toward the near field so thin nearby
            // geometry cannot fall between coarse linear steps.
            const float distanceAlongRay = max(thickness,
                traceDistance * stepFraction * stepFraction);
            const float3 rayPoint = originBiased + rayDirection * distanceAlongRay;
            float2 sampleUv;
            float projectedDepth;
            if (!project_world(rayPoint, sampleUv, projectedDepth) ||
                any(sampleUv <= 0.0f) || any(sampleUv >= 1.0f) ||
                projectedDepth <= 0.0f || projectedDepth >= 1.0f)
                break;

            const float rayDepth = dot(rayPoint - cameraPosition.xyz, cameraForwardDirection);
            if (!isfinite(rayDepth) || rayDepth <= nearClip || rayDepth >= farClip)
                break;
            const float pixelTravel = length((sampleUv - uv) * safe_resolution());
            const float mipValue = min(max(log2(max(pixelTravel * rayStepPixels, 1.0f)), 0.0f),
                float(maxMip));
            const uint mip = (uint)mipValue;
            const float2 pyramidRange = depthPyramid.SampleLevel(pyramidSampler, sampleUv,
                float(mip));
            if (!valid_hiz_range(pyramidRange))
                continue;
            const float conservativeThickness = thickness * 1.5f;
            const bool inPyramidInterval = rayDepth + conservativeThickness >= pyramidRange.x &&
                rayDepth - conservativeThickness <= pyramidRange.y;
            const bool crossedPyramidInterval = rayDepth > pyramidRange.y + conservativeThickness;
            if (!inPyramidInterval && !crossedPyramidInterval)
                continue;

            const float hitDeviceDepth = sceneDepth.SampleLevel(depthSampler, sampleUv, 0.0f);
            if (!isfinite(hitDeviceDepth) || hitDeviceDepth <= 0.0f || hitDeviceDepth >= 1.0f)
                continue;
            const float hitLinearDepth = linear_depth(hitDeviceDepth);
            const float depthDelta = rayDepth - hitLinearDepth;
            const float hitTolerance = thickness + max(hitLinearDepth * 0.0025f, 0.001f);
            const bool crossedSurface = depthDelta >= -hitTolerance &&
                (!previousDepthValid || previousDepthDelta < -hitTolerance);
            const bool depthMatch = abs(depthDelta) <= hitTolerance;
            if (distanceAlongRay < minHitDistance || (!depthMatch && !crossedSurface))
            {
                previousDepthDelta = depthDelta;
                previousDepthValid = true;
                continue;
            }

            const float4 hitNormalSample = worldNormal.SampleLevel(normalSampler, sampleUv, 0.0f);
            const float hitNormalLength = dot(hitNormalSample.xyz, hitNormalSample.xyz);
            const bool hitNormalValid = all(isfinite(hitNormalSample.xyz)) &&
                isfinite(hitNormalLength) && hitNormalLength > 0.25f;
            const float3 hitNormal = hitNormalValid
                ? safe_normalize(hitNormalSample.xyz, normal)
                : normal;
            const float hitFacing = hitNormalValid
                ? saturate(dot(hitNormal, -rayDirection))
                : 1.0f;
            const float depthConfidence = saturate(1.0f - abs(depthDelta) /
                max(hitTolerance, EPSILON));
            const float distanceWeight = rcp(1.0f + distanceAlongRay * distanceFalloff);
            const float cosineWeight = sqrt(saturate(originFacing * hitFacing));
            const float weight = depthConfidence * distanceWeight * cosineWeight;
            if (weight > EPSILON)
            {
                const float3 hitRadiance = finite_color(sceneHdr.SampleLevel(
                    sceneSampler, sampleUv, 0.0f).rgb);
                radianceSum += hitRadiance * weight;
                bentDirectionSum += rayDirection * weight;
                radianceWeight += weight;
                validSamples += 1.0f;
            }
            break;
        }
    }

    // Confidence measures the quality of accepted hits; visibility below
    // separately measures hit coverage. Dividing the accumulated weight by
    // the full ray budget here and multiplying by coverage again in the
    // composite would square sparse-hit probability and erase valid GI.
    const float averageHitWeight = radianceWeight / max(validSamples, 1.0f);
    const float confidence = sqrt(saturate(averageHitWeight)) * edge_fade(uv);
    const float3 irradiance = finite_color(radianceWeight > EPSILON
        ? radianceSum / radianceWeight * radiancePower
        : 0.0f.xxx);
    const float3 bentDirection = radianceWeight > EPSILON
        ? safe_normalize(normal * 0.5f + bentDirectionSum / radianceWeight, normal)
        : normal;
    // Square-root remapping keeps a single high-quality ray useful at the
    // low bounded budgets selected for the production path. Temporal/spatial
    // resolve still rejects unstable sparse hits.
    const float visibility = sqrt(saturate(validSamples / max(float(tapCount), 1.0f)));
    GatherOutput output;
    const float mode = finite_or(debug.x, 0.0f);
    if (mode > 0.5f && mode < 1.5f)
    {
        output.radiance = float4(confidence.xxx, confidence);
        output.bentNormal = float4(bentDirection * 0.5f + 0.5f, visibility);
    }
    else if (mode > 1.5f && mode < 2.5f)
    {
        output.radiance = float4(irradiance, confidence);
        output.bentNormal = float4(bentDirection * 0.5f + 0.5f, visibility);
    }
    else
    {
        output.radiance = float4(irradiance, confidence);
        output.bentNormal = float4(bentDirection * 0.5f + 0.5f, visibility);
    }
    return output;
}
