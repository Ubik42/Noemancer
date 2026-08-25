// Noemancer screen-space diffuse GI spatial resolve.
//
// The gather pass is intentionally noisy and may contain holes at screen
// borders. This resolve is a bounded two-axis cross bilateral filter over
// radiance and bent normal. Depth and normal rejection are explicit so the
// filter does not smear GI across thin walls, silhouettes or the edge of a
// tile.
//
// SDL_GPU graphics ABI (space2 sampled textures, space3 constants):
//   t0/s0 = gathered GI radiance/confidence (RGBA16F)
//   t1/s1 = gathered bent normal/visibility (RGBA16F)
//   t2/s2 = device depth
//   t3/s3 = world normal
//   t4/s4 = surface material properties (rgb base color, a metallic)
//   b0    = SsgiSpatialSettings (48 bytes)
//
// Target 0: resolved RGB GI radiance, A confidence.
// Target 1: resolved bent normal encoded to [0,1], A visibility.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> gatheredRadiance : register(t0, space2);
SamplerState radianceSampler : register(s0, space2);
Texture2D<float4> gatheredBentNormal : register(t1, space2);
SamplerState bentNormalSampler : register(s1, space2);
Texture2D<float> sceneDepth : register(t2, space2);
SamplerState depthSampler : register(s2, space2);
Texture2D<float4> worldNormal : register(t3, space2);
SamplerState normalSampler : register(s3, space2);
Texture2D<float4> materialProperties : register(t4, space2);
SamplerState materialSampler : register(s4, space2);

cbuffer SsgiSpatialSettings : register(b0, space3)
{
    // xy = inverse resolution, z = depth sigma, w = normal power.
    float4 resolutionAndFilter;
    // x = minimum diffuse weight, y = material validity threshold,
    // z = enabled, w = debug mode.
    float4 policy;
    // x = edge-preserving radius in pixels, yzw reserved.
    float4 output;
};

static const float EPSILON = 0.00001f;
static const float MAX_HDR = 65504.0f;

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

float linear_weight(int tap)
{
    if (tap == 0) return 0.38774f;
    if (abs(tap) == 1) return 0.24477f;
    return 0.06136f;
}

float3 load_normal(float2 uv)
{
    const float3 value = worldNormal.SampleLevel(normalSampler, saturate(uv), 0.0f).xyz;
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 0.25f
        ? safe_normalize(value, float3(0.0f, 1.0f, 0.0f))
        : 0.0f.xxx;
}

float load_depth(float2 uv)
{
    return finite_or(sceneDepth.SampleLevel(depthSampler, saturate(uv), 0.0f), 1.0f);
}

struct SpatialOutput
{
    float4 radiance : SV_Target0;
    float4 bentNormal : SV_Target1;
};

SpatialOutput main(FragmentInput input)
{
    const float2 uv = saturate(input.texcoord);
    const float centerDeviceDepth = load_depth(uv);
    const float4 centerMaterial = materialProperties.SampleLevel(materialSampler, uv, 0.0f);
    const float centerMetallic = saturate(finite_or(centerMaterial.w, 1.0f));
    const float centerDiffuseWeight = saturate(1.0f - centerMetallic);
    const float3 centerNormal = load_normal(uv);
    const float2 texel = max(float2(finite_or(resolutionAndFilter.x, 1.0f),
        finite_or(resolutionAndFilter.y, 1.0f)), EPSILON.xx);
    const float minimumDiffuseWeight = saturate(finite_or(policy.x, 0.0f));
    const float materialValidityThreshold = saturate(finite_or(policy.y, 0.0f));
    if (centerDeviceDepth <= 0.0f || centerDeviceDepth >= 1.0f ||
        dot(centerNormal, centerNormal) < 0.25f ||
        centerDiffuseWeight < max(minimumDiffuseWeight, materialValidityThreshold) ||
        finite_or(policy.z, 0.0f) < 0.5f)
    {
        SpatialOutput empty;
        empty.radiance = gatheredRadiance.SampleLevel(radianceSampler, uv, 0.0f);
        empty.bentNormal = gatheredBentNormal.SampleLevel(bentNormalSampler, uv, 0.0f);
        return empty;
    }

    const float radius = clamp(finite_or(output.x, 1.0f), 1.0f, 2.0f);
    const float depthSigma = max(finite_or(resolutionAndFilter.z, 1.0f), 0.0001f);
    const float normalPower = max(finite_or(resolutionAndFilter.w, 1.0f), 0.01f);
    float3 radianceSum = 0.0f.xxx;
    float3 bentSum = 0.0f.xxx;
    float weightSum = 0.0f;
    float visibilitySum = 0.0f;
    [unroll]
    for (int axis = 0; axis < 2; ++axis)
    {
        [unroll]
        for (int tap = -2; tap <= 2; ++tap)
        {
            // The center is shared by both axes; count it only once.
            if ((axis == 1 && tap == 0) || abs(float(tap)) > radius)
                continue;
            const float2 direction = axis == 0
                ? float2(1.0f, 0.0f)
                : float2(0.0f, 1.0f);
            const float2 sampleUv = saturate(uv + direction * float(tap) * texel);
            const float sampleDepth = load_depth(sampleUv);
            if (sampleDepth <= 0.0f || sampleDepth >= 1.0f)
                continue;
            const float3 sampleNormal = load_normal(sampleUv);
            if (dot(sampleNormal, sampleNormal) < 0.25f)
                continue;
            const float depthWeight = exp(-abs(sampleDepth - centerDeviceDepth) * depthSigma);
            const float normalAgreement = pow(saturate(dot(centerNormal, sampleNormal)), normalPower);
            const float sampleConfidence = saturate(finite_or(
                gatheredRadiance.SampleLevel(radianceSampler, sampleUv, 0.0f).a, 0.0f));
            const float weight = linear_weight(tap) * depthWeight * normalAgreement * sampleConfidence;
            const float4 sampleRadiance = gatheredRadiance.SampleLevel(radianceSampler, sampleUv, 0.0f);
            const float4 sampleBent = gatheredBentNormal.SampleLevel(bentNormalSampler, sampleUv, 0.0f);
            const float3 sampleBentDirection = safe_normalize(sampleBent.rgb * 2.0f - 1.0f, centerNormal);
            radianceSum += finite_color(sampleRadiance.rgb) * weight;
            bentSum += sampleBentDirection * weight;
            visibilitySum += saturate(finite_or(sampleBent.a, 0.0f)) * weight;
            weightSum += weight;
        }
    }

    const float4 centerRadiance = gatheredRadiance.SampleLevel(radianceSampler, uv, 0.0f);
    const float4 centerBent = gatheredBentNormal.SampleLevel(bentNormalSampler, uv, 0.0f);
    const float centerConfidence = saturate(finite_or(centerRadiance.a, 0.0f));
    const float3 centerBentDirection = safe_normalize(centerBent.rgb * 2.0f - 1.0f, centerNormal);
    const float3 resolvedRadiance = weightSum > EPSILON
        ? finite_color(radianceSum / weightSum)
        : finite_color(centerRadiance.rgb);
    const float expectedWeight = linear_weight(0) +
        (radius >= 1.0f ? 4.0f * linear_weight(1) : 0.0f) +
        (radius >= 2.0f ? 4.0f * linear_weight(2) : 0.0f);
    const float resolvedConfidence = weightSum > EPSILON
        ? saturate(weightSum / max(expectedWeight, EPSILON))
        : centerConfidence;
    const float3 resolvedBent = weightSum > EPSILON && all(isfinite(bentSum))
        ? safe_normalize(bentSum / weightSum, centerNormal)
        : centerBentDirection;
    const float resolvedVisibility = weightSum > EPSILON
        ? saturate(visibilitySum / weightSum)
        : saturate(finite_or(centerBent.a, 0.0f));

    SpatialOutput outputValue;
    const float mode = finite_or(policy.w, 0.0f);
    if (mode > 0.5f && mode < 1.5f)
    {
        outputValue.radiance = float4(resolvedConfidence.xxx, resolvedConfidence);
        outputValue.bentNormal = float4(resolvedBent * 0.5f + 0.5f, resolvedVisibility);
    }
    else if (mode > 1.5f && mode < 2.5f)
    {
        outputValue.radiance = float4(resolvedRadiance, resolvedConfidence);
        outputValue.bentNormal = float4(resolvedBent * 0.5f + 0.5f, resolvedVisibility);
    }
    else
    {
        outputValue.radiance = float4(resolvedRadiance, resolvedConfidence);
        outputValue.bentNormal = float4(resolvedBent * 0.5f + 0.5f, resolvedVisibility);
    }
    return outputValue;
}
