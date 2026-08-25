// Noemancer screen-space depth pyramid conservative 2x2 reduction.
//
// ABI (SDL_GPU compute):
//   t0/s0, space0 = parent Texture2D<float2> with point-clamp sampler where .x
//                   is minimum linear view-depth and .y is maximum view-depth.
//   u0, space1 = child RG32_FLOAT RWTexture2D<float2> with the same pair.
//   b0, space2 = DepthPyramidReduceParameters (16 bytes).
//
// A source mip chain is normally bound as one Texture2D view.  The explicit
// sourceMip field is therefore part of the ABI: Load(coord, 0) would always
// read mip 0 and silently make every reduction level wrong.
//
// reduceParameters:
//   x = source mip level; y/z = source width/height at that mip;
//   w = source mip count (used to clamp the requested level).  Destination
//       dimensions come from u0.
//
// The destination is normally ceil(sourceSize / 2).  Every source coordinate
// is bounds-checked before Load, so odd source widths/heights and a final
// one-pixel row/column are included exactly once.  Invalid source pairs are
// ignored; an all-invalid footprint becomes [0, 0] instead of propagating a
// NaN into later occlusion/SSR decisions.

Texture2D<float2> sourceDepthPyramid : register(t0, space0);
SamplerState sourceDepthSampler : register(s0, space0);
RWTexture2D<float2> reducedDepthPyramid : register(u0, space1);

cbuffer DepthPyramidReduceParameters : register(b0, space2)
{
    uint4 reduceParameters;
};

static const float FINITE_MAX = 3.402823466e+38f;

void include_sample(float2 sampleValue, inout float minimumDepth,
                    inout float maximumDepth, inout uint validSamples)
{
    if (!isfinite(sampleValue.x) || !isfinite(sampleValue.y))
        return;

    // A malformed pair is repaired locally rather than allowing min/max order
    // to become a backend-dependent result.
    const float sampleMinimum = min(sampleValue.x, sampleValue.y);
    const float sampleMaximum = max(sampleValue.x, sampleValue.y);
    minimumDepth = min(minimumDepth, sampleMinimum);
    maximumDepth = max(maximumDepth, sampleMaximum);
    validSamples += 1u;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint outputWidth;
    uint outputHeight;
    reducedDepthPyramid.GetDimensions(outputWidth, outputHeight);
    if (dispatchThreadId.x >= outputWidth || dispatchThreadId.y >= outputHeight)
        return;

    const uint sourceMipCount = max(reduceParameters.w, 1u);
    const uint sourceMip = min(reduceParameters.x, sourceMipCount - 1u);
    const uint sourceWidth = reduceParameters.y;
    const uint sourceHeight = reduceParameters.z;

    float minimumDepth = FINITE_MAX;
    float maximumDepth = -FINITE_MAX;
    uint validSamples = 0u;
    const uint2 base = dispatchThreadId.xy * 2u;

    // Explicit four taps preserve the conservative range and make the odd
    // boundary behavior independent of texture sampler state.
    [unroll]
    for (uint y = 0u; y < 2u; ++y)
    {
        [unroll]
        for (uint x = 0u; x < 2u; ++x)
        {
            const uint2 source = base + uint2(x, y);
            if (source.x >= sourceWidth || source.y >= sourceHeight)
                continue;
            const float2 sourceUv=(float2(source)+0.5f)/float2(sourceWidth,sourceHeight);
            include_sample(sourceDepthPyramid.SampleLevel(sourceDepthSampler,sourceUv,float(sourceMip)),
                minimumDepth, maximumDepth, validSamples);
        }
    }

    if (validSamples == 0u)
        reducedDepthPyramid[dispatchThreadId.xy] = 0.0f.xx;
    else
        reducedDepthPyramid[dispatchThreadId.xy] = float2(minimumDepth, maximumDepth);
}
