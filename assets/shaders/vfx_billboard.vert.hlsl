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

StructuredBuffer<ParticleState> particles : register(t0, space0);
StructuredBuffer<uint> compactAliveIndices : register(t1, space0);

cbuffer VfxCamera : register(b0, space1)
{
    float4x4 viewProjection;
    float4 cameraRight;
    float4 cameraUp;
    float deltaSeconds;
    float3 padding;
    float2 renderSize;
    float worldUnitsPerPixel;
    uint hybridPixelFlags;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float2 motion : TEXCOORD2;
    nointerpolation uint profileSampling : TEXCOORD3;
};

float2 finite_render_size()
{
    if (!isfinite(renderSize.x) || !isfinite(renderSize.y) || renderSize.x <= 0.0f || renderSize.y <= 0.0f)
        return float2(0.0f, 0.0f);
    return min(renderSize, float2(65536.0f, 65536.0f));
}

float4 snap_center_to_virtual_pixel(float4 centerClip)
{
    const float2 size = finite_render_size();
    if (size.x <= 0.0f || size.y <= 0.0f ||
        !isfinite(centerClip.x) || !isfinite(centerClip.y) ||
        !isfinite(centerClip.z) || !isfinite(centerClip.w) || abs(centerClip.w) < 0.00001f)
        return centerClip;

    const float2 ndc = centerClip.xy / centerClip.w;
    if (!isfinite(ndc.x) || !isfinite(ndc.y))
        return centerClip;

    // The y sign matches the motion-vector UV convention below, so virtual
    // pixels are addressed in the same top-left-origin space as the target.
    const float2 virtualPixel = (ndc * float2(0.5f, -0.5f) + 0.5f) * size;
    const float2 snappedPixel = floor(virtualPixel) + 0.5f;
    const float2 snappedNdc = (snappedPixel / size - 0.5f) * float2(2.0f, -2.0f);
    centerClip.xy = snappedNdc * centerClip.w;
    return centerClip;
}

float quantize_billboard_size(const float size)
{
    if (!isfinite(size) || size <= 0.0f ||
        !isfinite(worldUnitsPerPixel) || worldUnitsPerPixel <= 0.00001f)
        return max(size, 0.001f);

    const float pixelSize = clamp(worldUnitsPerPixel, 0.00001f, 65536.0f);
    const float pixelCount = size / pixelSize;
    if (!isfinite(pixelCount))
        return max(size, 0.001f);

    const float quantized = max(floor(pixelCount + 0.5f), 1.0f) * pixelSize;
    return isfinite(quantized) && quantized > 0.0f ? quantized : max(size, 0.001f);
}

VertexOutput main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1.0f, -1.0f), float2(1.0f, -1.0f), float2(1.0f, 1.0f),
        float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(-1.0f, 1.0f)
    };
    const ParticleState particle = particles[compactAliveIndices[instanceId]];
    const float2 corner = corners[vertexId];
    const float normalizedAge = saturate(particle.positionAge.w / max(particle.velocityLifetime.w, 0.0001f));
    const float4 color = lerp(particle.colorStartSizeStart, particle.colorEndSizeEnd, normalizedAge);
    const float size = max(color.w, 0.001f);
    const float3 previousCenter = particle.positionAge.xyz - particle.velocityLifetime.xyz * deltaSeconds;
    const bool snapCenter = (hybridPixelFlags & 1u) != 0u &&
        (particle.renderMetadata.x & 2u) != 0u;
    const bool quantizeSize = (hybridPixelFlags & 2u) != 0u &&
        (particle.renderMetadata.x & 4u) != 0u;
    const float billboardSize = quantizeSize ? quantize_billboard_size(size) : size;
    const float3 offset = (cameraRight.xyz * corner.x + cameraUp.xyz * corner.y) * billboardSize;
    float4 currentClip;
    float4 previousClip;
    if (!snapCenter && !quantizeSize)
    {
        // Keep the non-Hybrid-Pixel path bit-for-bit equivalent to the
        // original billboard transform and motion calculation.
        currentClip = mul(viewProjection, float4(particle.positionAge.xyz + offset, 1.0f));
        previousClip = mul(viewProjection, float4(previousCenter + offset, 1.0f));
    }
    else
    {
        // Snap only the centers in clip/NDC space.  Applying the same offset
        // in clip space keeps the current and previous billboards coherent.
        const float4 currentCenterClip = mul(viewProjection, float4(particle.positionAge.xyz, 1.0f));
        const float4 previousCenterClip = mul(viewProjection, float4(previousCenter, 1.0f));
        const float4 snappedCurrentCenter = snapCenter ? snap_center_to_virtual_pixel(currentCenterClip) : currentCenterClip;
        const float4 snappedPreviousCenter = snapCenter ? snap_center_to_virtual_pixel(previousCenterClip) : previousCenterClip;
        const float4 offsetClip = mul(viewProjection, float4(offset, 0.0f));
        currentClip = snappedCurrentCenter + offsetClip;
        previousClip = snappedPreviousCenter + offsetClip;
    }
    VertexOutput output;
    output.position = currentClip;
    output.uv = corner;
    output.color = float4(color.rgb, lerp(asfloat(particle.identity.z), asfloat(particle.identity.w), normalizedAge));
    const float2 currentUv = currentClip.xy / max(currentClip.w, 0.00001f) * float2(0.5f, -0.5f);
    const float2 previousUv = previousClip.xy / max(previousClip.w, 0.00001f) * float2(0.5f, -0.5f);
    output.motion = currentUv - previousUv;
    output.profileSampling = ((hybridPixelFlags & 4u) != 0u &&
        (particle.renderMetadata.x & 8u) != 0u) ? 1u : 0u;
    return output;
}
