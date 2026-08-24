struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> resolvedScene : register(t0, space2);
Texture2D<float> previousExposure : register(t1, space2);
SamplerState sceneSampler : register(s0, space2);
SamplerState exposureSampler : register(s1, space2);

cbuffer AutoExposureSettings : register(b0, space3)
{
    float minimumExposure;
    float maximumExposure;
    float keyValue;
    float deltaSeconds;
    float speedUp;
    float speedDown;
    float historyValid;
    float padding;
};

float4 main(FragmentInput input) : SV_Target0
{
    float logLuminance = 0.0f;
    [unroll] for (uint y = 0; y < 8; ++y)
    {
        [unroll] for (uint x = 0; x < 8; ++x)
        {
            const float2 uv = (float2(x, y) + 0.5f) / 8.0f;
            const float3 color = max(resolvedScene.SampleLevel(sceneSampler, uv, 0.0f).rgb, 0.0f);
            const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
            logLuminance += log2(max(luminance, 0.0001f));
        }
    }
    const float averageLuminance = exp2(logLuminance / 64.0f);
    const float target = clamp(keyValue / max(averageLuminance, 0.0001f), minimumExposure, maximumExposure);
    const float previous = historyValid > 0.5f
        ? previousExposure.SampleLevel(exposureSampler, float2(0.5f, 0.5f), 0.0f)
        : target;
    const float speed = target > previous ? speedUp : speedDown;
    const float adapted = lerp(previous, target, 1.0f - exp(-speed * deltaSeconds));
    return adapted.xxxx;
}
