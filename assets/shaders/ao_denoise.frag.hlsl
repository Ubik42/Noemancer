struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float> ambientOcclusion : register(t0, space2);
Texture2D<float> sceneDepth : register(t1, space2);
Texture2D<float4> sceneNormal : register(t2, space2);
SamplerState aoSampler : register(s0, space2);
SamplerState depthSampler : register(s1, space2);
SamplerState normalSampler : register(s2, space2);

// The block is two complete 16-byte rows: inverseResolution/direction and
// near/far/depthSigma/normalPower.  direction is normally horizontal or
// vertical, so the same shader can be used for a separable two-pass filter.
cbuffer AoDenoiseSettings : register(b0, space3)
{
    float2 inverseResolution;
    float2 direction;
    float nearClip;
    float farClip;
    float depthSigma;
    float normalPower;
};

float safe_near_clip()
{
    return max(nearClip, 0.0001f);
}

float safe_far_clip()
{
    return max(farClip, safe_near_clip() + 0.0001f);
}

float linear_depth(float deviceDepth)
{
    const float z = saturate(deviceDepth);
    const float nearValue = safe_near_clip();
    const float farValue = safe_far_clip();
    const float denominator = max(farValue - z * (farValue - nearValue), 0.0001f);
    return clamp(nearValue * farValue / denominator, nearValue, farValue);
}

float3 load_normal(float2 uv)
{
    const float3 value = sceneNormal.SampleLevel(normalSampler, uv, 0.0f).xyz;
    const float lengthSquared = dot(value, value);
    return lengthSquared > 0.0001f ? value * rsqrt(lengthSquared) : 0.0f.xxx;
}

float spatial_weight(int tap)
{
    // A compact five-tap Gaussian approximation (sigma ~= 1.25).
    if (tap == 0) return 0.38774f;
    if (abs(tap) == 1) return 0.24477f;
    return 0.06136f;
}

float4 main(FragmentInput input) : SV_Target0
{
    const float centerDeviceDepth = sceneDepth.SampleLevel(depthSampler, input.texcoord, 0.0f);
    if (centerDeviceDepth >= 0.99999f)
        return 1.0f.xxxx;

    const float3 centerNormal = load_normal(input.texcoord);
    if (dot(centerNormal, centerNormal) < 0.5f)
        return saturate(ambientOcclusion.SampleLevel(aoSampler, input.texcoord, 0.0f)).xxxx;

    const float centerDepth = linear_depth(centerDeviceDepth);
    const float sigma = max(depthSigma, 0.0001f);
    const float normalExponent = max(normalPower, 0.01f);
    const float2 texelStep = direction * inverseResolution;
    float weightedVisibility = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for (int tap = -2; tap <= 2; ++tap)
    {
        const float2 sampleUv = saturate(input.texcoord + texelStep * (float)tap);
        const float sampleDeviceDepth = sceneDepth.SampleLevel(depthSampler, sampleUv, 0.0f);
        if (sampleDeviceDepth >= 0.99999f)
            continue;

        const float3 sampleNormal = load_normal(sampleUv);
        if (dot(sampleNormal, sampleNormal) < 0.5f)
            continue;

        const float sampleDepth = linear_depth(sampleDeviceDepth);
        const float depthDifference = abs(sampleDepth - centerDepth);
        const float depthWeight = exp(-depthDifference * sigma);
        const float normalAgreement = saturate(dot(centerNormal, sampleNormal));
        const float normalWeight = pow(normalAgreement, normalExponent);
        const float weight = spatial_weight(tap) * depthWeight * normalWeight;
        weightedVisibility += saturate(ambientOcclusion.SampleLevel(aoSampler, sampleUv, 0.0f)) * weight;
        totalWeight += weight;
    }

    const float centerVisibility = saturate(ambientOcclusion.SampleLevel(aoSampler, input.texcoord, 0.0f));
    const float filteredVisibility = totalWeight > 0.0001f
        ? weightedVisibility / totalWeight
        : centerVisibility;
    return saturate(filteredVisibility).xxxx;
}
