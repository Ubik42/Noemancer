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

struct GpuDrivenInstance
{
    float4x4 model;
    float4x4 previousModel;
    float4 color;
    float4 material;
    float4 emissiveNormal;
    float4 occlusionAlphaFlags;
    uint4 objectIdentity;
    float4 bounds;
};

StructuredBuffer<GpuDrivenInstance> instances : register(t0, space0);
StructuredBuffer<uint> visibleIndices : register(t1, space0);

cbuffer GpuDrivenDrawData : register(b0, space1)
{
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    // drawMetadata.x = visible-index offset for this batch.
    // drawMetadata.y = candidate count belonging to this batch.
    // drawMetadata.z = global candidate count in the instance/visible-index buffers.
    uint4 drawMetadata;
};

static const uint gpuDrivenInstanceCapacity = 16384u;

VertexOutput clipped_vertex_output()
{
    VertexOutput output;
    output.position = float4(0.0f, 0.0f, -2.0f, 1.0f);
    output.worldPosition = float3(0.0f, 0.0f, 0.0f);
    output.worldNormal = float3(0.0f, 0.0f, 0.0f);
    output.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.material = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.texcoord = float2(0.0f, 0.0f);
    output.worldTangent = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.emissiveNormal = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.occlusionAlphaFlags = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.motion = float2(0.0f, 0.0f);
    output.objectId = 0u;
    return output;
}

VertexOutput main(VertexInput input, uint instanceId : SV_InstanceID)
{
    // Validate the metadata range before forming an index into visibleIndices.
    // The fixed capacity is the allocation contract shared with the runtime.
    if (drawMetadata.x > gpuDrivenInstanceCapacity ||
        drawMetadata.z > gpuDrivenInstanceCapacity ||
        drawMetadata.x > drawMetadata.z)
        return clipped_vertex_output();
    const uint remainingCandidateCount = drawMetadata.z - drawMetadata.x;
    if (instanceId >= drawMetadata.y || instanceId >= remainingCandidateCount)
        return clipped_vertex_output();

    const uint visibleIndexOffset = drawMetadata.x + instanceId;
    const uint candidateIndex = visibleIndices[visibleIndexOffset];
    // Validate the index read from visibleIndices before reading instances.
    if (candidateIndex >= drawMetadata.z)
        return clipped_vertex_output();
    const GpuDrivenInstance instance = instances[candidateIndex];
    const float4 localPosition = float4(input.position, 1.0f);
    const float4 world = mul(instance.model, localPosition);
    const float4 currentClip = mul(viewProjection, world);
    const float4 previousClip = mul(previousViewProjection, mul(instance.previousModel, localPosition));
    VertexOutput output;
    output.position = currentClip;
    output.worldPosition = world.xyz;
    output.worldNormal = normalize(mul((float3x3)instance.model, input.normal));
    output.worldTangent = float4(normalize(mul((float3x3)instance.model, input.tangent.xyz)), input.tangent.w);
    output.color = instance.color;
    output.material = instance.material;
    output.texcoord = input.texcoord;
    output.emissiveNormal = instance.emissiveNormal;
    output.occlusionAlphaFlags = instance.occlusionAlphaFlags;
    output.objectId = instance.objectIdentity.x;
    const float2 currentUv = currentClip.xy / max(currentClip.w, 0.00001f) * float2(0.5f, -0.5f);
    const float2 previousUv = previousClip.xy / max(previousClip.w, 0.00001f) * float2(0.5f, -0.5f);
    output.motion = currentUv - previousUv;
    return output;
}
