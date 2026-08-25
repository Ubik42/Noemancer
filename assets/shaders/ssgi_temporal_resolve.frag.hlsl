// Noemancer shared-history SSGI temporal resolve.
//
// Temporal history is imported from TemporalHistoryAuthority by Runtime. This
// shader only evaluates compatibility and writes the resolved color/bent
// normal pair; it does not own resets or invent a second history lifecycle.
// A miss can borrow a compatible prior sample, while reactive/disoccluded
// pixels immediately fall back to the current spatial estimate.
//
// SDL_GPU graphics ABI (space2 sampled textures, space3 constants):
//   t0/s0 = spatial GI radiance/confidence (RGBA16F)
//   t1/s1 = spatial bent normal/visibility (RGBA16F)
//   t2/s2 = previous GI radiance history (RGBA16F)
//   t3/s3 = previous bent normal/visibility history (RGBA16F)
//   t4/s4 = current device depth
//   t5/s5 = previous history device depth
//   t6/s6 = motion vectors (current UV - previous UV)
//   t7/s7 = reactive/disocclusion mask
//   b0    = SsgiTemporalSettings (48 bytes)
//
// Target 0/1: resolved GI radiance and bent normal (debug views allowed).
// Target 2/3: non-debug history copies for the next frame.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> spatialRadiance : register(t0, space2);
SamplerState spatialSampler : register(s0, space2);
Texture2D<float4> spatialBentNormal : register(t1, space2);
SamplerState spatialBentSampler : register(s1, space2);
Texture2D<float4> previousRadianceHistory : register(t2, space2);
SamplerState previousRadianceSampler : register(s2, space2);
Texture2D<float4> previousBentHistory : register(t3, space2);
SamplerState previousBentSampler : register(s3, space2);
Texture2D<float> sceneDepth : register(t4, space2);
SamplerState depthSampler : register(s4, space2);
Texture2D<float> previousHistoryDepth : register(t5, space2);
SamplerState historyDepthSampler : register(s5, space2);
Texture2D<float2> motionVectors : register(t6, space2);
SamplerState motionSampler : register(s6, space2);
Texture2D<float> reactiveMask : register(t7, space2);
SamplerState reactiveSampler : register(s7, space2);

cbuffer SsgiTemporalSettings : register(b0, space3)
{
    // xy = inverse resolution, z = history weight, w = history valid.
    float4 resolutionAndHistory;
    // x = depth relative tolerance, y = reactive scale, z = max motion pixels, w = enabled.
    float4 rejection;
    // x = debug mode, y = current frame, z = enabled, w reserved.
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

float linear_depth(float deviceDepth)
{
    // The temporal block does not need near/far for rejection precision: the
    // stored device depths are compared in a bounded relative domain first.
    return saturate(finite_or(deviceDepth, 1.0f));
}

float3 clamp_radiance_neighborhood(float2 uv, float3 history)
{
    const float2 texel = max(float2(finite_or(resolutionAndHistory.x, 1.0f),
        finite_or(resolutionAndHistory.y, 1.0f)), EPSILON.xx);
    float3 minimumValue = 0.0f.xxx;
    float3 maximumValue = 0.0f.xxx;
    bool initialized = false;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 sampleValue = finite_color(spatialRadiance.SampleLevel(
                spatialSampler, saturate(uv + float2(x, y) * texel), 0.0f).rgb);
            if (!initialized)
            {
                minimumValue = sampleValue;
                maximumValue = sampleValue;
                initialized = true;
            }
            else
            {
                minimumValue = min(minimumValue, sampleValue);
                maximumValue = max(maximumValue, sampleValue);
            }
        }
    }
    return clamp(history, minimumValue, maximumValue);
}

struct TemporalOutput
{
    float4 radiance : SV_Target0;
    float4 bentNormal : SV_Target1;
    float4 radianceHistory : SV_Target2;
    float4 bentNormalHistory : SV_Target3;
};

TemporalOutput main(FragmentInput input)
{
    const float2 uv = saturate(input.texcoord);
    const float4 currentRadiance = spatialRadiance.SampleLevel(spatialSampler, uv, 0.0f);
    const float4 currentBent = spatialBentNormal.SampleLevel(spatialBentSampler, uv, 0.0f);
    const float2 motionSample = motionVectors.SampleLevel(motionSampler, uv, 0.0f);
    const float2 motion = float2(finite_or(motionSample.x, 0.0f), finite_or(motionSample.y, 0.0f));
    const float2 previousUv = uv - motion;
    const bool inside = all(previousUv >= 0.0f) && all(previousUv <= 1.0f);
    const float4 previousRadiance = previousRadianceHistory.SampleLevel(
        previousRadianceSampler, saturate(previousUv), 0.0f);
    const float4 previousBent = previousBentHistory.SampleLevel(
        previousBentSampler, saturate(previousUv), 0.0f);
    const float currentDepth = linear_depth(sceneDepth.SampleLevel(depthSampler, uv, 0.0f));
    const float oldDepth = linear_depth(previousHistoryDepth.SampleLevel(
        historyDepthSampler, saturate(previousUv), 0.0f));
    const float depthTolerance = max(0.0005f, currentDepth * max(finite_or(rejection.x, 0.02f), 0.0001f));
    const float depthCompatible = abs(currentDepth - oldDepth) <= depthTolerance ? 1.0f : 0.0f;
    const float reactiveScale = max(finite_or(rejection.y, 1.0f), 0.0f);
    const float reactive = saturate(finite_or(reactiveMask.SampleLevel(reactiveSampler, uv, 0.0f), 0.0f) * reactiveScale);
    const float2 inverseResolution = max(float2(finite_or(resolutionAndHistory.x, 1.0f),
        finite_or(resolutionAndHistory.y, 1.0f)), EPSILON.xx);
    const float motionPixels = length(motion / inverseResolution);
    const float motionLimit = max(finite_or(rejection.z, 64.0f), 1.0f);
    const float currentConfidence = saturate(finite_or(currentRadiance.a, 0.0f));
    const float previousConfidence = saturate(finite_or(previousRadiance.a, 0.0f));
    const float currentLuma = dot(finite_color(currentRadiance.rgb), float3(0.2126f, 0.7152f, 0.0722f));
    const float previousLuma = dot(finite_color(previousRadiance.rgb), float3(0.2126f, 0.7152f, 0.0722f));
    const float disagreement = abs(currentLuma - previousLuma) /
        max(max(currentLuma, previousLuma), 0.05f);
    float historyWeight = saturate(finite_or(resolutionAndHistory.z, 0.0f)) * saturate(1.0f - disagreement) *
        saturate(1.0f - motionPixels / motionLimit);
    historyWeight *= saturate(finite_or(resolutionAndHistory.w, 0.0f)) *
        saturate(finite_or(rejection.w, 0.0f)) * (inside ? 1.0f : 0.0f) *
        depthCompatible * (1.0f - reactive) * (previousConfidence > 0.001f ? 1.0f : 0.0f);
    if (finite_or(output.z, 0.0f) < 0.5f)
        historyWeight = 0.0f;

    const float3 previousColor = clamp_radiance_neighborhood(uv, finite_color(previousRadiance.rgb));
    const float3 currentColor = finite_color(currentRadiance.rgb);
    const float3 resolvedColor = finite_color(lerp(currentColor, previousColor, historyWeight));
    const float3 fallbackBent = safe_normalize(currentBent.rgb * 2.0f - 1.0f,
        float3(0.0f, 1.0f, 0.0f));
    const float3 historyBent = safe_normalize(previousBent.rgb * 2.0f - 1.0f, fallbackBent);
    const float3 resolvedBent = safe_normalize(lerp(fallbackBent, historyBent, historyWeight), fallbackBent);
    const float resolvedConfidence = saturate(max(currentConfidence, previousConfidence * historyWeight));
    const float resolvedVisibility = saturate(max(finite_or(currentBent.a, 0.0f),
        finite_or(previousBent.a, 0.0f) * historyWeight));

    TemporalOutput result;
    float3 debugColor = resolvedColor;
    const float mode = finite_or(output.x, 0.0f);
    if (mode > 0.5f && mode < 1.5f)
        debugColor = resolvedConfidence.xxx;
    else if (mode > 1.5f && mode < 2.5f)
        debugColor = historyWeight.xxx;
    else if (mode > 2.5f && mode < 3.5f)
        debugColor = depthCompatible.xxx;
    else if (mode > 3.5f && mode < 4.5f)
        debugColor = float3(reactive, reactive * 0.2f, reactive * 0.05f);
    else if (mode > 4.5f)
        debugColor = resolvedBent * 0.5f + 0.5f;
    result.radiance = float4(debugColor, resolvedConfidence);
    result.bentNormal = float4(resolvedBent * 0.5f + 0.5f, resolvedVisibility);
    result.radianceHistory = float4(resolvedColor, resolvedConfidence);
    result.bentNormalHistory = result.bentNormal;
    return result;
}
