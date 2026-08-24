struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texcoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 joints : TEXCOORD4;
    float4 weights : TEXCOORD5;
};

cbuffer ObjectData : register(b0, space1)
{
    float4x4 model;
    float4x4 viewProjection;
    float4x4 lightViewProjection;
    float4 objectColor;
    float4 materialParams;
    float4 emissiveNormal;
    float4 occlusionAlphaFlags;
    uint4 objectIdentity;
    float4x4 previousModel;
    float4x4 previousViewProjection;
};

cbuffer SkinningData : register(b1, space1)
{
    float4x4 jointMatrices[64];
};

cbuffer ShadowInstancingData : register(b2, space1)
{
    float4x4 instanceModels[16];
    float4 instanceColors[16];
    float4 instanceOcclusionAlphaFlags[16];
    uint4 instancingMetadata;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 occlusionAlphaFlags : TEXCOORD2;
};

VertexOutput main(VertexInput input, uint instanceId : SV_InstanceID)
{
    VertexOutput output;
    float4 localPosition = float4(input.position, 1.0f);
    if (objectIdentity.y > 0)
    {
        const uint4 joints = min((uint4)input.joints, objectIdentity.y - 1);
        const float4 weights = input.weights / max(dot(input.weights, 1.0f), 0.00001f);
        localPosition = mul(jointMatrices[joints.x], localPosition) * weights.x
            + mul(jointMatrices[joints.y], localPosition) * weights.y
            + mul(jointMatrices[joints.z], localPosition) * weights.z
            + mul(jointMatrices[joints.w], localPosition) * weights.w;
    }
    const bool instanced = instancingMetadata.x > 0;
    const uint safeInstanceId = min(instanceId, 15);
    const float4x4 activeModel = instanced ? instanceModels[safeInstanceId] : model;
    output.position = mul(lightViewProjection, mul(activeModel, localPosition));
    output.texcoord = input.texcoord;
    output.color = instanced ? instanceColors[safeInstanceId] : objectColor;
    output.occlusionAlphaFlags = instanced ? instanceOcclusionAlphaFlags[safeInstanceId] : occlusionAlphaFlags;
    return output;
}
