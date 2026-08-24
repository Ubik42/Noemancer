struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 worldTangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
    float2 motion : TEXCOORD4;
    nointerpolation uint objectId : TEXCOORD5;
    nointerpolation uint alphaMode : TEXCOORD6;
    nointerpolation float4 materialParameters : TEXCOORD7;
    nointerpolation float4 emissiveColor : TEXCOORD8;
    nointerpolation float4 surfaceParameters : TEXCOORD9;
};

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

cbuffer SpriteDrawData : register(b0, space1)
{
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    uint4 drawMetadata;
};

static const float MAX_FINITE_VALUE = 65504.0f;

float3 safe_normalize(float3 value, float3 fallback)
{
    const float3 finite = select(isfinite(value), value, fallback);
    const float3 bounded = min(max(finite, float3(-MAX_FINITE_VALUE, -MAX_FINITE_VALUE, -MAX_FINITE_VALUE)),
        float3(MAX_FINITE_VALUE, MAX_FINITE_VALUE, MAX_FINITE_VALUE));
    const float lengthSquared = dot(bounded, bounded);
    return lengthSquared > 1.0e-8f ? bounded * rsqrt(lengthSquared) : fallback;
}

VertexOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint drawIndex = drawMetadata.x + min(instanceId, max(drawMetadata.y, 1) - 1);
    const SpriteInstance instance = spriteInstances[spriteDrawIndices[drawIndex]];
    const float4 uvRect = instance.uvRect;
    const float4 localRect = instance.localRect;
    const uint4 flags = instance.identityFlags;
    static const uint corners[6] = {0, 1, 2, 0, 2, 3};
    const uint corner = corners[min(vertexId, 5)];
    const float2 unit = float2((corner == 1 || corner == 2) ? 1.0f : 0.0f,
                               (corner >= 2) ? 1.0f : 0.0f);
    const float2 local = lerp(localRect.xy, localRect.zw, unit);
    float2 uvUnit = unit;
    if (flags.z != 0) uvUnit.x = 1.0f - uvUnit.x;
    if (flags.w != 0) uvUnit.y = 1.0f - uvUnit.y;
    const float4 world = mul(instance.model, float4(local, 0.0f, 1.0f));
    const float4 previousWorld = mul(instance.previousModel, float4(local, 0.0f, 1.0f));
    const float4 currentClip = mul(viewProjection, world);
    const float4 previousClip = mul(previousViewProjection, previousWorld);
    const float2 currentUv = currentClip.xy / max(currentClip.w, 0.00001f) * float2(0.5f, -0.5f);
    const float2 previousUv = previousClip.xy / max(previousClip.w, 0.00001f) * float2(0.5f, -0.5f);
    const float3 modelNormal = mul((float3x3)instance.model, float3(0.0f, 0.0f, 1.0f));
    const float3 normal = safe_normalize(modelNormal, float3(0.0f, 0.0f, 1.0f));
    const float3 modelTangent = mul((float3x3)instance.model, float3(1.0f, 0.0f, 0.0f));
    const float3 tangentProjection = modelTangent - normal * dot(modelTangent, normal);
    const float3 tangentFallback = abs(normal.z) < 0.999f
        ? safe_normalize(cross(float3(0.0f, 0.0f, 1.0f), normal), float3(1.0f, 0.0f, 0.0f))
        : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = safe_normalize(tangentProjection, tangentFallback);
    const float3 modelBitangent = mul((float3x3)instance.model, float3(0.0f, 1.0f, 0.0f));
    const float handedness = dot(cross(normal, tangent), modelBitangent) < 0.0f ? -1.0f : 1.0f;
    VertexOutput output;
    output.position = currentClip;
    output.worldPosition = world.xyz;
    output.worldNormal = normal;
    output.worldTangent = float4(tangent, handedness);
    output.texcoord = lerp(uvRect.xy, uvRect.zw, uvUnit);
    output.motion = currentUv - previousUv;
    output.objectId = flags.x;
    output.alphaMode = flags.y;
    output.materialParameters = instance.materialParameters;
    output.emissiveColor = instance.emissiveColor;
    output.surfaceParameters = instance.surfaceParameters;
    return output;
}
