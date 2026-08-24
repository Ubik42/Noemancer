struct FragmentInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> currentHdr : register(t0, space2);
SamplerState currentSampler : register(s0, space2);
Texture2D<float2> motionVectors : register(t1, space2);
SamplerState motionSampler : register(s1, space2);
Texture2D<float> sceneDepth : register(t2, space2);
SamplerState depthSampler : register(s2, space2);
Texture2D<float4> historyHdr : register(t3, space2);
SamplerState historySampler : register(s3, space2);
Texture2D<float> historyDepth : register(t4, space2);
SamplerState historyDepthSampler : register(s4, space2);
Texture2D<float> reactiveMask : register(t5, space2);
SamplerState reactiveSampler : register(s5, space2);

cbuffer TaaSettings : register(b0, space3)
{
    float2 inverseResolution;
    float historyWeight;
    float historyValid;
    float nearClip;
    float farClip;
    float reactiveScale;
    float debugMode;
};

struct FragmentOutput
{
    float4 resolved : SV_Target0;
    float4 history : SV_Target1;
    float historyDepth : SV_Target2;
};

float linearize_depth(float depth)
{
    return nearClip * farClip / max(farClip - saturate(depth) * (farClip - nearClip), 0.000001f);
}

FragmentOutput main(FragmentInput input)
{
    const float4 current = currentHdr.Sample(currentSampler, input.uv);
    float3 neighborhoodMin = current.rgb;
    float3 neighborhoodMax = current.rgb;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 sampleColor = currentHdr.Sample(currentSampler, input.uv + float2(x, y) * inverseResolution).rgb;
            neighborhoodMin = min(neighborhoodMin, sampleColor);
            neighborhoodMax = max(neighborhoodMax, sampleColor);
        }
    const float2 motion = motionVectors.Sample(motionSampler, input.uv);
    const float2 historyUv = input.uv - motion;
    const bool inside = all(historyUv >= 0.0f) && all(historyUv <= 1.0f);
    const float3 historySample = clamp(historyHdr.Sample(historySampler, historyUv).rgb, neighborhoodMin, neighborhoodMax);
    const float currentLuma = dot(current.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    const float historyLuma = dot(historySample, float3(0.2126f, 0.7152f, 0.0722f));
    const float disagreement = abs(currentLuma - historyLuma) / max(max(currentLuma, historyLuma), 0.05f);
    const float motionPixels = length(motion / inverseResolution);
    const float depth = sceneDepth.Sample(depthSampler, input.uv);
    const float previousDepth = historyDepth.Sample(historyDepthSampler, historyUv);
    const float linearDepth = linearize_depth(depth);
    const float previousLinearDepth = linearize_depth(previousDepth);
    const float depthTolerance = max(0.10f, linearDepth * 0.02f);
    const float depthCompatible = abs(linearDepth - previousLinearDepth) <= depthTolerance ? 1.0f : 0.0f;
    const float reactive = saturate(reactiveMask.Sample(reactiveSampler, input.uv) * reactiveScale);
    float weight = historyWeight * saturate(1.0f - disagreement) * saturate(1.0f - motionPixels / 64.0f);
    weight *= historyValid * (inside ? 1.0f : 0.0f) * (depth < 1.0f ? 1.0f : 0.0f) * depthCompatible * (1.0f - reactive);
    const float4 resolved = float4(lerp(current.rgb, historySample, weight), current.a);
    FragmentOutput output;
    float3 displayColor = resolved.rgb;
    if (debugMode > 0.5f && debugMode < 1.5f)
        displayColor = float3(motion * 32.0f + 0.5f, saturate(motionPixels / 16.0f));
    else if (debugMode > 1.5f && debugMode < 2.5f)
        displayColor = float3(reactive, reactive * 0.2f, reactive * 0.05f);
    else if (debugMode > 2.5f && debugMode < 3.5f)
        displayColor = depthCompatible > 0.5f ? float3(0.05f, 0.65f, 0.10f) : float3(0.90f, 0.05f, 0.02f);
    else if (debugMode > 3.5f)
        displayColor = weight.xxx;
    output.resolved = float4(displayColor, 1.0f);
    output.history = resolved;
    output.historyDepth = depth;
    return output;
}
