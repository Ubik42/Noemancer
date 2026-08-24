struct VertexInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texcoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 joints : TEXCOORD4;
    float4 weights : TEXCOORD5;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 color : TEXCOORD2;
    float4 material : TEXCOORD3;
    float2 texcoord : TEXCOORD4;
    float4 worldTangent : TEXCOORD5;
    float4 emissiveNormal : TEXCOORD6;
    float4 occlusionAlphaFlags : TEXCOORD7;
    float2 motion : TEXCOORD8;
    nointerpolation uint objectId : TEXCOORD9;
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

cbuffer PreviousSkinningData : register(b2, space1)
{
    float4x4 previousJointMatrices[64];
};

cbuffer InstancingData : register(b3, space1)
{
    float4x4 instanceModels[16];
    float4x4 instancePreviousModels[16];
    uint4 instanceObjectIdentities[16];
    float4 instanceColors[16];
    float4 instanceMaterials[16];
    float4 instanceEmissiveNormals[16];
    float4 instanceOcclusionAlphaFlags[16];
    uint4 instancingMetadata;
};

VertexOutput main(VertexInput input, uint instanceId : SV_InstanceID)
{
    VertexOutput output;
    float4 localPosition = float4(input.position, 1.0f);
    float4 previousLocalPosition = localPosition;
    float3 localNormal = input.normal;
    float3 localTangent = input.tangent.xyz;
    if (objectIdentity.y > 0)
    {
        const uint4 joints = min((uint4)input.joints, objectIdentity.y - 1);
        const float4 weights = input.weights / max(dot(input.weights, 1.0f), 0.00001f);
        localPosition = mul(jointMatrices[joints.x], localPosition) * weights.x
            + mul(jointMatrices[joints.y], localPosition) * weights.y
            + mul(jointMatrices[joints.z], localPosition) * weights.z
            + mul(jointMatrices[joints.w], localPosition) * weights.w;
        const float4 sourceNormal = float4(localNormal, 0.0f);
        const float4 sourceTangent = float4(localTangent, 0.0f);
        localNormal = (mul(jointMatrices[joints.x], sourceNormal) * weights.x
            + mul(jointMatrices[joints.y], sourceNormal) * weights.y
            + mul(jointMatrices[joints.z], sourceNormal) * weights.z
            + mul(jointMatrices[joints.w], sourceNormal) * weights.w).xyz;
        localTangent = (mul(jointMatrices[joints.x], sourceTangent) * weights.x
            + mul(jointMatrices[joints.y], sourceTangent) * weights.y
            + mul(jointMatrices[joints.z], sourceTangent) * weights.z
            + mul(jointMatrices[joints.w], sourceTangent) * weights.w).xyz;
    }
    if (objectIdentity.z > 0)
    {
        const uint4 previousJoints = min((uint4)input.joints, objectIdentity.z - 1);
        const float4 previousWeights = input.weights / max(dot(input.weights, 1.0f), 0.00001f);
        previousLocalPosition = mul(previousJointMatrices[previousJoints.x], previousLocalPosition) * previousWeights.x
            + mul(previousJointMatrices[previousJoints.y], previousLocalPosition) * previousWeights.y
            + mul(previousJointMatrices[previousJoints.z], previousLocalPosition) * previousWeights.z
            + mul(previousJointMatrices[previousJoints.w], previousLocalPosition) * previousWeights.w;
    }
    const bool instanced = instancingMetadata.x > 0;
    const uint safeInstanceId = min(instanceId, 15);
    const float4x4 activeModel = instanced ? instanceModels[safeInstanceId] : model;
    const float4x4 activePreviousModel = instanced ? instancePreviousModels[safeInstanceId] : previousModel;
    const float4 world = mul(activeModel, localPosition);
    const float4 currentClip = mul(viewProjection, world);
    const float4 previousClip = mul(previousViewProjection, mul(activePreviousModel, previousLocalPosition));
    output.position = currentClip;
    const float2 currentUv = currentClip.xy / max(currentClip.w, 0.00001f) * float2(0.5f, -0.5f);
    const float2 previousUv = previousClip.xy / max(previousClip.w, 0.00001f) * float2(0.5f, -0.5f);
    output.motion = currentUv - previousUv;
    output.worldPosition = world.xyz;
    output.worldNormal = normalize(mul((float3x3)activeModel, localNormal));
    output.worldTangent = float4(normalize(mul((float3x3)activeModel, localTangent)), input.tangent.w);
    output.color = instanced ? instanceColors[safeInstanceId] : objectColor;
    output.material = instanced ? instanceMaterials[safeInstanceId] : materialParams;
    output.texcoord = input.texcoord;
    output.emissiveNormal = instanced ? instanceEmissiveNormals[safeInstanceId] : emissiveNormal;
    output.occlusionAlphaFlags = instanced ? instanceOcclusionAlphaFlags[safeInstanceId] : occlusionAlphaFlags;
    output.objectId = instanced ? instanceObjectIdentities[safeInstanceId].x : objectIdentity.x;
    return output;
}
