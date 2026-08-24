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

struct SpawnGraphParameters
{
    float4 originAge;
    float4 lifetimeSpeed;
    float4 colorStartSizeStart;
    float4 colorEndSizeEnd;
    float4 gravityDrag;
    uint4 seedBlend;
};

RWStructuredBuffer<ParticleState> particles : register(u0, space1);
RWStructuredBuffer<uint> outputAliveIndices : register(u1, space1);
RWStructuredBuffer<uint> deadIndices : register(u2, space1);
RWByteAddressBuffer outputIndirectDraw : register(u3, space1);
RWByteAddressBuffer deadCounter : register(u4, space1);
RWStructuredBuffer<uint4> spawnIdentities : register(u5, space1);
RWStructuredBuffer<SpawnGraphParameters> spawnGraphs : register(u6, space1);

cbuffer SpawnParameters : register(b0, space2)
{
    uint spawnCount;
    uint capacity;
    uint2 padding;
};

uint randomU32(uint2 seed, uint particleIndex, uint channel)
{
    uint value = seed.x ^ ((seed.y << 16) | (seed.y >> 16)) ^
        (particleIndex * 0x9e3779b9u) ^ (channel * 0x85ebca6bu);
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float randomRange(uint2 seed, uint particleIndex, uint channel, float minimumValue, float maximumValue)
{
    const float unit = float(randomU32(seed, particleIndex, channel) >> 8) * (1.0f / 16777216.0f);
    return lerp(minimumValue, maximumValue, unit);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= spawnCount)
        return;

    uint observed = deadCounter.Load(0);
    uint original = observed;
    while (observed > 0)
    {
        deadCounter.InterlockedCompareExchange(0, observed, observed - 1, original);
        if (original == observed)
        {
            const uint particleIndex = deadIndices[observed - 1];
            const uint4 spawnIdentity = spawnIdentities[dispatchThreadId.x];
            const SpawnGraphParameters graph = spawnGraphs[spawnIdentity.w];
            const uint2 seed = graph.seedBlend.xy;
            const float z = randomRange(seed, spawnIdentity.z, 0, -1.0f, 1.0f);
            const float angle = randomRange(seed, spawnIdentity.z, 1, 0.0f, 6.28318530718f);
            const float radial = sqrt(max(0.0f, 1.0f - z * z));
            const float speed = randomRange(seed, spawnIdentity.z, 2, graph.lifetimeSpeed.z, graph.lifetimeSpeed.w);
            const float lifetime = randomRange(seed, spawnIdentity.z, 3, graph.lifetimeSpeed.x, graph.lifetimeSpeed.y);

            ParticleState particle;
            particle.positionAge = float4(graph.originAge.xyz, graph.originAge.w);
            particle.velocityLifetime = float4(cos(angle) * radial * speed, abs(z) * speed, sin(angle) * radial * speed, lifetime);
            particle.colorStartSizeStart = graph.colorStartSizeStart;
            particle.colorEndSizeEnd = graph.colorEndSizeEnd;
            const float alphaStart = float(graph.seedBlend.w & 0xffffu) * (1.0f / 65535.0f);
            const float alphaEnd = float(graph.seedBlend.w >> 16) * (1.0f / 65535.0f);
            particle.identity = uint4(spawnIdentity.xy, asuint(alphaStart), asuint(alphaEnd));
            particle.gravityDrag = graph.gravityDrag;
            particle.renderMetadata = uint4(graph.seedBlend.z, 0, 0, 0);

            // Catch a newly resident particle up to the CPU fixed-step
            // reference without uploading its dynamic position or velocity.
            float remainingAge = graph.originAge.w;
            particle.positionAge.xyz = graph.originAge.xyz;
            [loop]
            for (uint step = 0; step < 512 && remainingAge > 0.000001f; ++step)
            {
                const float deltaSeconds = min(remainingAge, 1.0f / 60.0f);
                const float dragFactor = max(0.0f, 1.0f - graph.gravityDrag.w * deltaSeconds);
                particle.velocityLifetime.xyz =
                    (particle.velocityLifetime.xyz + graph.gravityDrag.xyz * deltaSeconds) * dragFactor;
                particle.positionAge.xyz += particle.velocityLifetime.xyz * deltaSeconds;
                remainingAge -= deltaSeconds;
            }

            particles[particleIndex] = particle;
            uint outputIndex;
            outputIndirectDraw.InterlockedAdd(4, 1, outputIndex);
            if (outputIndex < capacity)
                outputAliveIndices[outputIndex] = particleIndex;
            return;
        }
        observed = original;
    }
}
