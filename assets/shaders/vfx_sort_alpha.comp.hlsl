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
RWStructuredBuffer<uint> alphaIndices : register(u1, space1);
RWByteAddressBuffer alphaIndirectDraw : register(u2, space1);

cbuffer SortParameters : register(b0, space2)
{
    float3 cameraPosition;
    uint capacity;
    uint sequenceLength;
    uint compareStride;
    uint sortSpan;
    uint padding;
};

int compareDesired(uint leftLane, uint rightLane)
{
    const uint leftIndex = alphaIndices[leftLane];
    const uint rightIndex = alphaIndices[rightLane];
    const bool leftValid = leftIndex < capacity;
    const bool rightValid = rightIndex < capacity;
    if (leftValid != rightValid)
        return leftValid ? -1 : 1;
    if (!leftValid)
        return 0;

    const ParticleState left = particles[leftIndex];
    const ParticleState right = particles[rightIndex];
    const float3 leftDelta = left.positionAge.xyz - cameraPosition;
    const float3 rightDelta = right.positionAge.xyz - cameraPosition;
    const float leftDistance = dot(leftDelta, leftDelta);
    const float rightDistance = dot(rightDelta, rightDelta);
    if (leftDistance != rightDistance)
        return leftDistance > rightDistance ? -1 : 1;
    if (left.identity.y != right.identity.y)
        return left.identity.y < right.identity.y ? -1 : 1;
    if (left.identity.x != right.identity.x)
        return left.identity.x < right.identity.x ? -1 : 1;
    return 0;
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint count = min(alphaIndirectDraw.Load(4), min(capacity, sortSpan));
    if (count <= 1)
        return;
    const uint lane = dispatchThreadId.x;
    if (lane >= sortSpan)
        return;
    const uint partner = lane ^ compareStride;
    if (partner <= lane || partner >= sortSpan)
        return;

    const int order = compareDesired(lane, partner);
    const bool finalDirection = (lane & sequenceLength) == 0;
    const bool shouldSwap = finalDirection ? order > 0 : order < 0;
    if (shouldSwap)
    {
        const uint value = alphaIndices[lane];
        alphaIndices[lane] = alphaIndices[partner];
        alphaIndices[partner] = value;
    }
}
