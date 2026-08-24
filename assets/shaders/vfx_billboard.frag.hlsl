struct FragmentInput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float2 motion : TEXCOORD2;
    nointerpolation uint profileSampling : TEXCOORD3;
};

struct FragmentOutput
{
    float4 color : SV_Target0;
    uint objectId : SV_Target1;
    float4 worldNormal : SV_Target2;
    float2 motion : SV_Target3;
    float reactive : SV_Target4;
    float4 indirectLighting : SV_Target5;
};

FragmentOutput main(FragmentInput input)
{
    const float radiusSquared = dot(input.uv, input.uv);
    if (radiusSquared > 1.0f)
        discard;
    // Profile sampling is a nearest-style, pixel-center coverage decision.
    // Ordinary Raster retains the existing soft analytic falloff.
    const float edge = input.profileSampling != 0u
        ? 1.0f : saturate((1.0f - radiusSquared) * 4.0f);
    const float alpha = input.color.a * edge;
    FragmentOutput output;
    // Deliberately HDR emissive: the same particles feed bloom and tone mapping.
    output.color = float4(input.color.rgb * (1.5f + edge * 2.0f), alpha);
    output.objectId = 0;
    output.worldNormal = float4(0.0f, 0.0f, 1.0f, 0.0f);
    output.motion = input.motion;
    output.reactive = saturate(alpha);
    output.indirectLighting = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return output;
}
