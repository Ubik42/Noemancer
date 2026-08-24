struct SpriteInstance
{
    float4x4 model;
    float4x4 previousModel;
    float4 uvRect;
    float4 localRect;
    uint4 identityFlags;
    float4 materialParameters;
    float4 emissiveColor;
    float4 surfaceParameters;
};

StructuredBuffer<SpriteInstance> spriteInstances : register(t0, space0);
StructuredBuffer<uint> spriteDrawIndices : register(t1, space0);

cbuffer SpriteShadowDrawData : register(b0, space1)
{
    float4x4 lightViewProjection;
    float4x4 unusedPreviousViewProjection;
    uint4 drawMetadata;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 occlusionAlphaFlags : TEXCOORD2;
};

VertexOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint drawIndex = drawMetadata.x + min(instanceId, max(drawMetadata.y, 1) - 1);
    const SpriteInstance instance = spriteInstances[spriteDrawIndices[drawIndex]];
    static const uint corners[6] = {0, 1, 2, 0, 2, 3};
    const uint corner = corners[min(vertexId, 5)];
    const float2 unit = float2((corner == 1 || corner == 2) ? 1.0f : 0.0f,
                               (corner >= 2) ? 1.0f : 0.0f);
    float2 uvUnit = unit;
    if (instance.identityFlags.z != 0) uvUnit.x = 1.0f - uvUnit.x;
    if (instance.identityFlags.w != 0) uvUnit.y = 1.0f - uvUnit.y;
    const float2 local = lerp(instance.localRect.xy, instance.localRect.zw, unit);

    VertexOutput output;
    output.position = mul(lightViewProjection, mul(instance.model, float4(local, 0.0f, 1.0f)));
    output.texcoord = lerp(instance.uvRect.xy, instance.uvRect.zw, uvUnit);
    output.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    output.occlusionAlphaFlags = float4(1.0f, 0.5f, 1.0f, 0.0f);
    return output;
}
