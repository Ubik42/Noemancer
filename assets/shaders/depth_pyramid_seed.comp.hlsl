// Noemancer screen-space depth pyramid seed.
//
// ABI (SDL_GPU compute):
//   t0/s0, space0 = scene device-depth Texture2D<float> with point-clamp sampler.
//   u0, space1 = RG32_FLOAT RWTexture2D<float2>.  .x is conservative minimum
//                linear view-depth and .y is conservative maximum depth.
//   b0, space2 = DepthPyramidSeedParameters (16 bytes).
//
// The seed normally has the same dimensions as the scene depth target.  The
// integer mapping also makes a deliberately smaller seed well-defined: each
// output pixel samples the source pixel at the centre of its corresponding
// source footprint.  The reduction stage, rather than this stage, is
// responsible for combining a footprint into a conservative range.
//
// depthParameters:
//   x = near clip distance (metres), y = far clip distance (metres),
//   z = non-zero when device depth is reversed (1 = near, 0 = far),
//   w = reserved and must be zero.

Texture2D<float> sceneDepth : register(t0, space0);
SamplerState sceneDepthSampler : register(s0, space0);
RWTexture2D<float2> depthPyramid : register(u0, space1);

cbuffer DepthPyramidSeedParameters : register(b0, space2)
{
    float4 depthParameters;
};

static const float MIN_VALUE = 0.0001f;

float finite_or(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float safe_near_clip()
{
    return max(finite_or(depthParameters.x, MIN_VALUE), MIN_VALUE);
}

float safe_far_clip(float nearClip)
{
    const float authoredFar = finite_or(depthParameters.y, nearClip + 1.0f);
    return max(authoredFar, nearClip + MIN_VALUE);
}

float safe_device_depth(float value)
{
    // An invalid source sample is treated as the far plane.  This prevents a
    // single NaN from poisoning every parent level of the pyramid.
    const float farDeviceDepth = depthParameters.z > 0.5f ? 0.0f : 1.0f;
    return saturate(finite_or(value, farDeviceDepth));
}

float linear_view_depth(float deviceDepth)
{
    const float nearClip = safe_near_clip();
    const float farClip = safe_far_clip(nearClip);
    float z = safe_device_depth(deviceDepth);
    if (depthParameters.z > 0.5f)
        z = 1.0f - z;

    // Forward-Z perspective device depth.  The reversed-Z branch above maps
    // to the same equation, so all pyramid levels have one comparable unit.
    const float denominator = max(farClip - z * (farClip - nearClip), MIN_VALUE);
    return clamp(finite_or(nearClip * farClip / denominator, farClip),
        nearClip, farClip);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint outputWidth;
    uint outputHeight;
    depthPyramid.GetDimensions(outputWidth, outputHeight);
    if (dispatchThreadId.x >= outputWidth || dispatchThreadId.y >= outputHeight)
        return;

    uint sourceWidth;
    uint sourceHeight;
    sceneDepth.GetDimensions(sourceWidth, sourceHeight);
    if (sourceWidth == 0u || sourceHeight == 0u)
    {
        const float fallbackDepth = safe_far_clip(safe_near_clip());
        depthPyramid[dispatchThreadId.xy] = float2(fallbackDepth, fallbackDepth);
        return;
    }

    // Integer arithmetic avoids a half-pixel rounding ambiguity between
    // DXIL and SPIR-V when the seed target is not the source resolution.
    const uint sourceX = min((dispatchThreadId.x * sourceWidth) / max(outputWidth, 1u),
        sourceWidth - 1u);
    const uint sourceY = min((dispatchThreadId.y * sourceHeight) / max(outputHeight, 1u),
        sourceHeight - 1u);
    const float2 sourceUv=(float2(sourceX,sourceY)+0.5f)/float2(sourceWidth,sourceHeight);
    const float viewDepth = linear_view_depth(sceneDepth.SampleLevel(sceneDepthSampler,sourceUv,0.0f));
    depthPyramid[dispatchThreadId.xy] = float2(viewDepth, viewDepth);
}
