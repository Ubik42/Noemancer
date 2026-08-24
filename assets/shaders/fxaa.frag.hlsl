struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> sceneColor : register(t0, space2);
SamplerState linearSampler : register(s0, space2);

cbuffer FxaaSettings : register(b0, space3)
{
    float2 inverseResolution;
    float edgeThreshold;
    float edgeThresholdMin;
};

float luma(float3 color)
{
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = input.texcoord;
    const float3 rgbM = sceneColor.Sample(linearSampler, uv).rgb;
    const float lumaM = luma(rgbM);
    const float lumaN = luma(sceneColor.Sample(linearSampler, uv + float2(0.0f, -inverseResolution.y)).rgb);
    const float lumaS = luma(sceneColor.Sample(linearSampler, uv + float2(0.0f, inverseResolution.y)).rgb);
    const float lumaW = luma(sceneColor.Sample(linearSampler, uv + float2(-inverseResolution.x, 0.0f)).rgb);
    const float lumaE = luma(sceneColor.Sample(linearSampler, uv + float2(inverseResolution.x, 0.0f)).rgb);
    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range = lumaMax - lumaMin;
    if (range < max(edgeThresholdMin, lumaMax * edgeThreshold)) return float4(rgbM, 1.0f);

    float2 direction;
    direction.x = -((lumaN + lumaS) - 2.0f * lumaM);
    direction.y =  ((lumaW + lumaE) - 2.0f * lumaM);
    const float directionReduce = max((lumaN + lumaS + lumaW + lumaE) * 0.03125f, 0.0078125f);
    const float reciprocalMin = 1.0f / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * reciprocalMin, -8.0f, 8.0f) * inverseResolution;
    const float3 rgbA = 0.5f * (
        sceneColor.Sample(linearSampler, uv + direction * (1.0f / 3.0f - 0.5f)).rgb +
        sceneColor.Sample(linearSampler, uv + direction * (2.0f / 3.0f - 0.5f)).rgb);
    const float3 rgbB = rgbA * 0.5f + 0.25f * (
        sceneColor.Sample(linearSampler, uv + direction * -0.5f).rgb +
        sceneColor.Sample(linearSampler, uv + direction * 0.5f).rgb);
    const float lumaB = luma(rgbB);
    return float4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0f);
}
