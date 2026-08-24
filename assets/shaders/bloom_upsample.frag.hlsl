struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

// Binding order is part of the SDL_GPU post-process ABI:
// t0 = existing higher-resolution bloom, t1 = lower-resolution bloom,
// s0/s1 = clamp/linear samplers. SDL_GPU's sampled-texture ABI keeps one
// sampler slot paired with each texture binding, even when both samplers
// use the same filtering state.
Texture2D<float4> highResolutionBloom : register(t0, space2);
Texture2D<float4> lowResolutionBloom : register(t1, space2);
SamplerState highLinearSampler : register(s0, space2);
SamplerState lowLinearSampler : register(s1, space2);

// 16-byte ABI, mirrored by the renderer's BloomUpsampleSettings:
// inverseLowResolution, scatter, padding.
cbuffer BloomUpsampleSettings : register(b0, space3)
{
    float2 inverseLowResolution;
    float scatter;
    float padding;
};

float3 sample_low(float2 uv)
{
    return max(lowResolutionBloom.SampleLevel(lowLinearSampler, uv, 0.0f).rgb, 0.0f);
}

float3 low_tent_filter(float2 uv)
{
    // The low-resolution image is filtered with the same normalized tent
    // footprint as downsample. This makes each level's contribution smooth
    // and resistant to shimmer when the camera moves by a fraction of a pixel.
    const float2 texel = inverseLowResolution;
    float3 result = sample_low(uv) * 0.25f;
    result += sample_low(uv + float2(texel.x, 0.0f)) * 0.125f;
    result += sample_low(uv - float2(texel.x, 0.0f)) * 0.125f;
    result += sample_low(uv + float2(0.0f, texel.y)) * 0.125f;
    result += sample_low(uv - float2(0.0f, texel.y)) * 0.125f;
    result += sample_low(uv + texel) * 0.0625f;
    result += sample_low(uv + float2(texel.x, -texel.y)) * 0.0625f;
    result += sample_low(uv + float2(-texel.x, texel.y)) * 0.0625f;
    result += sample_low(uv - texel) * 0.0625f;
    return max(result, 0.0f);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float3 high = max(highResolutionBloom.SampleLevel(highLinearSampler, input.texcoord, 0.0f).rgb, 0.0f);
    // Saturating scatter bounds the per-level addition to one normalized
    // low-resolution contribution, preventing uncontrolled energy growth.
    const float3 contribution = saturate(scatter) * low_tent_filter(input.texcoord);
    return float4(max(high + contribution, 0.0f), 1.0f);
}
