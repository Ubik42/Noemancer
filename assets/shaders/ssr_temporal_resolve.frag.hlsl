// Noemancer shared-history SSR resolve.
//
// This pass intentionally follows the same bounded neighborhood-clamp and
// motion/depth rejection shape as temporal_denoise.frag.hlsl.  Its history
// slot is owned by TemporalHistoryAuthority in Runtime; the shader only
// consumes the imported previous slot and never invents a second reset policy.
//
// SDL_GPU graphics ABI (space2 sampled textures, space3 constants):
//   t0/s0 = raw SSR radiance/confidence (RGBA16F)
//   t1/s1 = previous SSR history radiance/confidence (RGBA16F)
//   t2/s2 = current device depth
//   t3/s3 = previous SSR history depth (linearized by the same settings)
//   t4/s4 = motion vectors (current UV - previous UV)
//   t5/s5 = reactive/disocclusion mask
//   t6/s6 = current world normal
//   t7/s7 = previous shared normal history
//   b0    = SsrTemporalSettings (64 bytes)
// Outputs:
//   SV_Target0 = resolved reflection radiance/confidence
//   SV_Target1 = clean history copy for the next shared-history transaction.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> rawReflection : register(t0, space2);
SamplerState rawSampler : register(s0, space2);
Texture2D<float4> previousHistory : register(t1, space2);
SamplerState historySampler : register(s1, space2);
Texture2D<float> sceneDepth : register(t2, space2);
SamplerState depthSampler : register(s2, space2);
Texture2D<float> previousHistoryDepth : register(t3, space2);
SamplerState historyDepthSampler : register(s3, space2);
Texture2D<float2> motionVectors : register(t4, space2);
SamplerState motionSampler : register(s4, space2);
Texture2D<float> reactiveMask : register(t5, space2);
SamplerState reactiveSampler : register(s5, space2);
Texture2D<float4> currentNormal : register(t6, space2);
SamplerState currentNormalSampler : register(s6, space2);
Texture2D<float4> previousNormal : register(t7, space2);
SamplerState previousNormalSampler : register(s7, space2);

cbuffer SsrTemporalSettings : register(b0, space3)
{
    // xy = inverse resolution, z = requested history weight, w = history valid.
    float4 resolutionAndHistory;
    // x = near clip, y = far clip, z = depth relative tolerance, w = reactive scale.
    float4 rejection;
    // x = maximum motion in pixels, y = debug mode, z = enabled, w = reserved.
    float4 output;
    // x = minimum compatible normal dot; yzw reserved.
    float4 normalRejection;
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

float linear_depth(float deviceDepth)
{
    const float nearClip = max(rejection.x, EPSILON);
    const float farClip = max(rejection.y, nearClip + EPSILON);
    return clamp(finite_or(nearClip * farClip /
        max(farClip - saturate(deviceDepth) * (farClip - nearClip), EPSILON), farClip),
        nearClip, farClip);
}

float3 neighborhood_clamp(float2 uv, float3 history)
{
    const float2 texel = max(resolutionAndHistory.xy, 0.0f.xx);
    float3 minimumValue = 0.0f.xxx;
    float3 maximumValue = 0.0f.xxx;
    bool initialized = false;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 sampleValue = finite_color(rawReflection.SampleLevel(
                rawSampler, saturate(uv + float2(x, y) * texel), 0.0f).rgb);
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

struct ResolveOutput
{
    float4 resolved : SV_Target0;
    float4 history : SV_Target1;
};

ResolveOutput main(FragmentInput input)
{
    const float2 uv = saturate(input.texcoord);
    const float4 current = rawReflection.SampleLevel(rawSampler, uv, 0.0f);
    const float2 motion = motionVectors.SampleLevel(motionSampler, uv, 0.0f);
    const float2 historyUv = uv - motion;
    const bool inside = all(historyUv >= 0.0f) && all(historyUv <= 1.0f);
    const float4 historyRaw = previousHistory.SampleLevel(historySampler, saturate(historyUv), 0.0f);
    const float3 historyColor = neighborhood_clamp(uv, finite_color(historyRaw.rgb));
    const float currentDepth = sceneDepth.SampleLevel(depthSampler, uv, 0.0f);
    const float oldDepth = previousHistoryDepth.SampleLevel(historyDepthSampler, saturate(historyUv), 0.0f);
    const float currentLinearDepth = linear_depth(currentDepth);
    const float oldLinearDepth = linear_depth(oldDepth);
    const float depthTolerance = max(0.10f, currentLinearDepth * max(rejection.z, 0.0001f));
    const float depthCompatible = abs(currentLinearDepth - oldLinearDepth) <= depthTolerance ? 1.0f : 0.0f;
    const float reactive = saturate(finite_or(reactiveMask.SampleLevel(reactiveSampler, uv, 0.0f), 0.0f) * rejection.w);
    const float3 currentNormalValue=currentNormal.SampleLevel(currentNormalSampler,uv,0.0f).xyz;
    const float3 previousNormalValue=previousNormal.SampleLevel(previousNormalSampler,saturate(historyUv),0.0f).xyz;
    const float currentNormalLength=dot(currentNormalValue,currentNormalValue);
    const float previousNormalLength=dot(previousNormalValue,previousNormalValue);
    const bool normalsValid=isfinite(currentNormalLength)&&isfinite(previousNormalLength)&&
        currentNormalLength>0.000001f&&previousNormalLength>0.000001f;
    const float normalCompatible=normalsValid&&dot(currentNormalValue*rsqrt(currentNormalLength),
        previousNormalValue*rsqrt(previousNormalLength))>=clamp(normalRejection.x,-1.0f,1.0f)?1.0f:0.0f;
    const float motionPixels = length(motion / max(resolutionAndHistory.xy, 0.000001f.xx));
    const float motionLimit = max(output.x, 1.0f);
    const float currentConfidence = saturate(finite_or(current.a, 0.0f));
    const float historyConfidence = saturate(finite_or(historyRaw.a, 0.0f));
    const float currentLuma = dot(finite_color(current.rgb), float3(0.2126f, 0.7152f, 0.0722f));
    const float historyLuma = dot(historyColor, float3(0.2126f, 0.7152f, 0.0722f));
    const float disagreement = abs(currentLuma - historyLuma) /
        max(max(currentLuma, historyLuma), 0.05f);
    float historyWeight = saturate(resolutionAndHistory.z) *
        saturate(1.0f - disagreement) * saturate(1.0f - motionPixels / motionLimit);
    historyWeight *= resolutionAndHistory.w * (inside ? 1.0f : 0.0f) *
        (currentDepth > 0.0f && currentDepth < 1.0f ? 1.0f : 0.0f) *
        depthCompatible * normalCompatible * (1.0f - reactive) * (historyConfidence > 0.001f ? 1.0f : 0.0f);
    if (output.z < 0.5f)
        historyWeight = 0.0f;

    const float3 currentColor = finite_color(current.rgb);
    const float3 resolvedColor = finite_color(lerp(currentColor, historyColor, historyWeight));
    // A miss may borrow a valid history sample, but its confidence remains
    // bounded by both the borrowed confidence and the rejection result.
    const float resolvedConfidence = saturate(max(currentConfidence,
        historyConfidence * historyWeight));
    ResolveOutput result;
    float3 debugColor = resolvedColor;
    const float mode = output.y;
    if (mode > 0.5f && mode < 1.5f)
        debugColor = resolvedConfidence.xxx;
    else if (mode > 1.5f && mode < 2.5f)
        debugColor = historyWeight.xxx;
    else if (mode > 2.5f && mode < 3.5f)
        debugColor = depthCompatible.xxx;
    else if (mode > 3.5f && mode < 4.5f)
        debugColor = float3(reactive, reactive * 0.2f, reactive * 0.05f);
    else if (mode > 4.5f)
        debugColor = currentColor;
    result.resolved = float4(debugColor, resolvedConfidence);
    result.history = float4(resolvedColor, resolvedConfidence);
    return result;
}
