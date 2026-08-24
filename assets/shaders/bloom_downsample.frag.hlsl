struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

// Binding order is part of the SDL_GPU post-process ABI:
// t0 = source bloom level, s0 = clamp/linear sampler.
Texture2D<float4> sourceBloom : register(t0, space2);
SamplerState linearSampler : register(s0, space2);

// 32-byte ABI, mirrored by the renderer's BloomDownsampleSettings:
// inverseSourceResolution, threshold, softKnee, applyThreshold, padding[3].
cbuffer BloomDownsampleSettings : register(b0, space3)
{
    float2 inverseSourceResolution;
    float threshold;
    float softKnee;
    float applyThreshold;
    float3 padding;
};

float3 sample_source(float2 uv)
{
    // Explicit LOD keeps the kernel stable across backends and avoids
    // derivative-dependent mip selection during a full-screen pass.
    return max(sourceBloom.SampleLevel(linearSampler, uv, 0.0f).rgb, 0.0f);
}

float3 prefilter(float3 color)
{
    if (applyThreshold < 0.5f || threshold <= 0.0f)
        return color;

    const float brightness = max(color.r, max(color.g, color.b));
    const float knee = max(threshold * max(softKnee, 0.0f), 0.0001f);
    float soft = brightness - threshold + knee;
    soft = saturate(soft / (2.0f * knee));
    soft = soft * soft * knee;
    const float contribution = max(brightness - threshold, soft) /
        max(brightness, 0.0001f);
    return color * contribution;
}

float3 sample_prefiltered(float2 uv)
{
    return prefilter(sample_source(uv));
}

float3 tent_filter(float2 uv)
{
    // A normalized 3x3 tent kernel. The explicit weights sum to one and the
    // fixed footprint avoids frame-to-frame changes from implicit LODs.
    const float2 texel = inverseSourceResolution;
    float3 result = sample_prefiltered(uv) * 0.25f;
    result += sample_prefiltered(uv + float2(texel.x, 0.0f)) * 0.125f;
    result += sample_prefiltered(uv - float2(texel.x, 0.0f)) * 0.125f;
    result += sample_prefiltered(uv + float2(0.0f, texel.y)) * 0.125f;
    result += sample_prefiltered(uv - float2(0.0f, texel.y)) * 0.125f;
    result += sample_prefiltered(uv + texel) * 0.0625f;
    result += sample_prefiltered(uv + float2(texel.x, -texel.y)) * 0.0625f;
    result += sample_prefiltered(uv + float2(-texel.x, texel.y)) * 0.0625f;
    result += sample_prefiltered(uv - texel) * 0.0625f;
    return max(result, 0.0f);
}

float4 main(FragmentInput input) : SV_Target0
{
    return float4(tent_filter(input.texcoord), 1.0f);
}
