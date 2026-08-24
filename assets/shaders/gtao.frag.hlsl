struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

// Keep the GTAO ABI stable: two sampled textures, two samplers, and one
// eight-float (32-byte) settings block.  The depth is device-Z and the normal
// target stores an unencoded world-space normal.
Texture2D<float> sceneDepth : register(t0, space2);
Texture2D<float4> sceneNormal : register(t1, space2);
SamplerState depthSampler : register(s0, space2);
SamplerState normalSampler : register(s1, space2);

cbuffer GtaoSettings : register(b0, space3)
{
    float2 inverseResolution;
    float nearClip;
    float farClip;
    float radiusPixels;
    float intensity;
    float bias;
    float power;
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

float sample_horizon(float centerDepth, float3 centerNormal, float sampleDepth,
                     float3 sampleNormal, float sampleDistance)
{
    const float depthDifference = centerDepth - sampleDepth - max(bias, 0.0f);
    if (depthDifference <= 0.0f)
        return 0.0f;

    // A horizon contribution is strongest for a nearby depth discontinuity,
    // then falls off for unrelated geometry.  The normal term keeps a wall
    // or a coplanar surface from becoming a broad dark halo.
    const float depthRange = max(centerDepth * 0.35f, safe_near_clip() * 4.0f);
    const float horizon = saturate(depthDifference / max(depthRange, 0.0001f));
    const float rangeWeight = saturate(1.0f - abs(centerDepth - sampleDepth) /
        max(centerDepth * 0.5f, safe_near_clip() * 8.0f));
    const float normalWeight = saturate(0.5f + 0.5f * dot(centerNormal, sampleNormal));
    const float distanceWeight = rcp(1.0f + sampleDistance * 0.35f);
    return horizon * rangeWeight * normalWeight * distanceWeight;
}

float4 main(FragmentInput input) : SV_Target0
{
    const float deviceDepth = sceneDepth.SampleLevel(depthSampler, input.texcoord, 0.0f);
    if (deviceDepth >= 0.99999f)
        return 1.0f.xxxx;

    const float3 centerNormal = load_normal(input.texcoord);
    if (dot(centerNormal, centerNormal) < 0.5f)
        return 1.0f.xxxx;

    const float centerDepth = linear_depth(deviceDepth);
    const float radius = clamp(radiusPixels, 1.0f, 128.0f);
    const float2 directions[8] = {
        float2(1.0f, 0.0f), float2(0.0f, 1.0f),
        normalize(float2(1.0f, 1.0f)), normalize(float2(1.0f, -1.0f)),
        normalize(float2(2.0f, 1.0f)), normalize(float2(1.0f, -2.0f)),
        normalize(float2(-2.0f, 1.0f)), normalize(float2(1.0f, 2.0f))
    };

    float occlusion = 0.0f;
    [unroll]
    for (uint directionIndex = 0; directionIndex < 8; ++directionIndex)
    {
        float directionOcclusion = 0.0f;
        [unroll]
        for (uint stepIndex = 1; stepIndex <= 4; ++stepIndex)
        {
            const float stepDistance = (float)stepIndex * 0.25f;
            const float2 pixelOffset = directions[directionIndex] * inverseResolution * radius * stepDistance;
            [unroll]
            for (int side = -1; side <= 1; side += 2)
            {
                const float2 sampleUv = saturate(input.texcoord + pixelOffset * (float)side);
                const float sampleDeviceDepth = sceneDepth.SampleLevel(depthSampler, sampleUv, 0.0f);
                if (sampleDeviceDepth >= 0.99999f)
                    continue;

                const float3 sampleNormal = load_normal(sampleUv);
                if (dot(sampleNormal, sampleNormal) < 0.5f)
                    continue;

                const float contribution = sample_horizon(
                    centerDepth, centerNormal, linear_depth(sampleDeviceDepth), sampleNormal, stepDistance);
                directionOcclusion = max(directionOcclusion, contribution);
            }
        }
        occlusion += directionOcclusion;
    }

    const float normalizedOcclusion = saturate(occlusion / 8.0f);
    const float aoStrength = clamp(intensity, 0.0f, 4.0f);
    const float visibility = saturate(1.0f - aoStrength * normalizedOcclusion);
    return pow(max(visibility, 0.0f), max(power, 0.01f)).xxxx;
}
