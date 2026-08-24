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

struct GpuDrivenBatch
{
    uint candidateOffset;
    uint candidateCount;
    uint visibleOffset;
    uint padding;
};

StructuredBuffer<GpuDrivenInstance> instances : register(t0, space0);
StructuredBuffer<GpuDrivenBatch> batches : register(t1, space0);
RWStructuredBuffer<uint> visibleIndices : register(u0, space1);
RWByteAddressBuffer indirectCommands : register(u1, space1);

cbuffer VisibilityParameters : register(b0, space2)
{
    float4 frustumPlanes[6];
    uint candidateCount;
    uint batchCount;
    uint2 padding;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;
    if (candidateIndex >= candidateCount)
        return;

    const GpuDrivenInstance instance = instances[candidateIndex];
    bool visible = true;
    [unroll]
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
    {
        if (dot(frustumPlanes[planeIndex].xyz, instance.bounds.xyz) + frustumPlanes[planeIndex].w < -instance.bounds.w)
        {
            visible = false;
            break;
        }
    }
    if (!visible)
        return;

    const uint batchIndex = instance.objectIdentity.w;
    if (batchIndex >= batchCount)
        return;
    const GpuDrivenBatch batch = batches[batchIndex];
    uint localVisibleIndex;
    indirectCommands.InterlockedAdd(batchIndex * 20 + 4, 1, localVisibleIndex);
    if (localVisibleIndex < batch.candidateCount)
        visibleIndices[batch.visibleOffset + localVisibleIndex] = candidateIndex;
}
