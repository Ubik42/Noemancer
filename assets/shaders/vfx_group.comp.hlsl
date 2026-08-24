struct ParticleState
{
    float4 positionAge;
    float4 velocityLifetime;
    float4 colorStartSizeStart;
    float4 colorEndSizeEnd;
    uint4 identity;
    float4 gravityDrag;
    uint4 renderMetadata;
};

RWStructuredBuffer<ParticleState> particles : register(u0, space1);
RWStructuredBuffer<uint> aliveIndices : register(u1, space1);
RWByteAddressBuffer aliveIndirectDraw : register(u2, space1);
RWStructuredBuffer<uint> additiveIndices : register(u3, space1);
RWStructuredBuffer<uint> alphaIndices : register(u4, space1);
RWByteAddressBuffer additiveIndirectDraw : register(u5, space1);
RWByteAddressBuffer alphaIndirectDraw : register(u6, space1);

cbuffer GroupParameters : register(b0, space2)
{
    uint capacity;
    uint mode;
    uint2 padding;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= capacity)
        return;
    if (mode == 0)
    {
        additiveIndices[dispatchThreadId.x] = capacity;
        alphaIndices[dispatchThreadId.x] = capacity;
        return;
    }
    const uint aliveCount = min(aliveIndirectDraw.Load(4), capacity);
    if (dispatchThreadId.x >= aliveCount)
        return;

    const uint particleIndex = aliveIndices[dispatchThreadId.x];
    const bool alphaBlended = (particles[particleIndex].renderMetadata.x & 1u) != 0u;
    uint outputIndex;
    if (alphaBlended)
    {
        alphaIndirectDraw.InterlockedAdd(4, 1, outputIndex);
        if (outputIndex < capacity)
            alphaIndices[outputIndex] = particleIndex;
    }
    else
    {
        additiveIndirectDraw.InterlockedAdd(4, 1, outputIndex);
        if (outputIndex < capacity)
            additiveIndices[outputIndex] = particleIndex;
    }
}
