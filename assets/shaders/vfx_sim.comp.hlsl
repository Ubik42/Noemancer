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
RWStructuredBuffer<uint> compactAliveIndices : register(u2, space1);
RWByteAddressBuffer indirectDraw : register(u3, space1);
RWStructuredBuffer<uint> deadIndices : register(u4, space1);
RWByteAddressBuffer deadCounter : register(u5, space1);
RWByteAddressBuffer inputIndirectDraw : register(u6, space1);

cbuffer SimulationParameters : register(b0, space2)
{
    float deltaSeconds;
    uint parameterInputCount;
    uint capacity;
    uint parameterPadding;
    float3 gravity;
    float drag;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint inputCount = inputIndirectDraw.Load(4);
    if (dispatchThreadId.x >= inputCount || dispatchThreadId.x >= capacity)
        return;

    const uint particleIndex = aliveIndices[dispatchThreadId.x];
    ParticleState particle = particles[particleIndex];
    particle.positionAge.w += deltaSeconds;
    const float dragFactor = max(0.0f, 1.0f - particle.gravityDrag.w * deltaSeconds);
    particle.velocityLifetime.xyz = (particle.velocityLifetime.xyz + particle.gravityDrag.xyz * deltaSeconds) * dragFactor;
    particle.positionAge.xyz += particle.velocityLifetime.xyz * deltaSeconds;

    if (particle.positionAge.w >= particle.velocityLifetime.w)
    {
        uint deadIndex;
        deadCounter.InterlockedAdd(0, 1, deadIndex);
        if (deadIndex < capacity)
            deadIndices[deadIndex] = particleIndex;
    }
    else
    {
        uint compactIndex;
        indirectDraw.InterlockedAdd(4, 1, compactIndex);
        if (compactIndex < capacity)
            compactAliveIndices[compactIndex] = particleIndex;
    }
    particles[particleIndex] = particle;
}
