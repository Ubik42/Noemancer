struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> sceneHdr : register(t0, space2);
Texture2D<float4> indirectLighting : register(t1, space2);
Texture2D<float> filteredAmbientOcclusion : register(t2, space2);
SamplerState sceneSampler : register(s0, space2);
SamplerState indirectSampler : register(s1, space2);
SamplerState aoSampler : register(s2, space2);

float4 main(FragmentInput input) : SV_Target0
{
    const float4 sceneSample = sceneHdr.SampleLevel(sceneSampler, input.texcoord, 0.0f);
    const float3 scene = max(sceneSample.rgb, 0.0f);
    const float3 indirect = max(indirectLighting.SampleLevel(indirectSampler, input.texcoord, 0.0f).rgb, 0.0f);
    const float visibility = saturate(filteredAmbientOcclusion.SampleLevel(aoSampler, input.texcoord, 0.0f));

    // Remove the unoccluded indirect term before applying filtered AO.  This
    // is explicit arithmetic rather than a multiply blend, so the result is
    // correct for HDR values and remains non-negative for malformed inputs.
    const float3 composited = max(scene - indirect + indirect * visibility, 0.0f);
    return float4(composited, sceneSample.a);
}
