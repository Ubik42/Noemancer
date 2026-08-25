// Noemancer atmosphere transmittance LUT.
//
// This is an original compute adaptation of the LUT-stage architecture used
// by WickedEngine's MIT sky atmosphere implementation (commit
// f4a0d2635d5224b4509da164fa75d90fbdaaea26).  The source deliberately keeps
// the contract self-contained: no third-party shader include, global binding
// table or backend handle crosses into this artifact.
//
// Contract:
//   SDL compute ABI: u0 in space1, b0 in space2.
//   u0 is the RGBA16F transmittance result.

RWTexture2D<float4> transmittanceLut : register(u0, space1);

cbuffer SkyAtmosphereLutParameters : register(b0, space2)
{
    // x: planet radius (m), y: atmosphere height (m), z: camera height (m),
    // w: maximum ray distance (m; reserved for later camera-volume stages).
    float4 planetParameters;
    // x: Rayleigh scale height, y: Mie scale height, z: ozone centre height,
    // w: ozone full width (all metres).
    float4 densityParameters;
    float4 groundAlbedo;
    float4 rayleighScattering;
    float4 rayleighAbsorption;
    float4 mieScattering;
    float4 mieAbsorption;
    float4 ozoneAbsorption;
    // xyz: unit vector from planet to sun; w: sun angular radius (radians).
    float4 sunDirection;
    float4 sunIrradiance;
    // x/y: logical target dimensions; z/w are reserved for future LUT epochs.
    float4 targetParameters;
    // x: transmittance samples, y: multi-scattering samples, z: sky-view
    // samples, w: flags (bit 0 enables temporal dither in later stages).
    uint4 quality;
};

static const float MIN_VALUE = 0.0001f;
static const float MAX_OPTICAL_DEPTH = 80.0f;

float finite_or_zero(float value)
{
    return isfinite(value) ? value : 0.0f;
}

float3 finite_or_zero(float3 value)
{
    return float3(finite_or_zero(value.x), finite_or_zero(value.y), finite_or_zero(value.z));
}

float3 safe_normalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > MIN_VALUE * MIN_VALUE ? value * rsqrt(lengthSquared) : fallback;
}

float safe_radius(float value)
{
    return max(finite_or_zero(value), MIN_VALUE);
}

float3 safe_sun_direction()
{
    return safe_normalize(finite_or_zero(sunDirection.xyz), float3(0.0f, 1.0f, 0.0f));
}

float2 ray_sphere_intersection(float3 origin, float3 direction, float radius)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
        return float2(-1.0f, -1.0f);
    const float root = sqrt(max(discriminant, 0.0f));
    return float2(-b - root, -b + root);
}

struct MediumSample
{
    float3 scattering;
    float3 extinction;
};

MediumSample sample_medium(float3 position)
{
    const float radius = safe_radius(planetParameters.x);
    const float height = max(length(position) - radius, 0.0f);
    const float rayleighHeight = max(finite_or_zero(densityParameters.x), MIN_VALUE);
    const float mieHeight = max(finite_or_zero(densityParameters.y), MIN_VALUE);
    const float ozoneWidth = max(finite_or_zero(densityParameters.w), MIN_VALUE);
    const float ozoneCentre = finite_or_zero(densityParameters.z);
    const float rayleighDensity = exp(-min(height / rayleighHeight, MAX_OPTICAL_DEPTH));
    const float mieDensity = exp(-min(height / mieHeight, MAX_OPTICAL_DEPTH));
    const float ozoneDensity = saturate(1.0f - abs(height - ozoneCentre) / (0.5f * ozoneWidth));
    const float3 rayleigh = max(finite_or_zero(rayleighScattering.rgb), 0.0f) * rayleighDensity;
    const float3 mie = max(finite_or_zero(mieScattering.rgb), 0.0f) * mieDensity;
    const float3 extinction = rayleigh + mie +
        max(finite_or_zero(rayleighAbsorption.rgb), 0.0f) * rayleighDensity +
        max(finite_or_zero(mieAbsorption.rgb), 0.0f) * mieDensity +
        max(finite_or_zero(ozoneAbsorption.rgb), 0.0f) * ozoneDensity;
    MediumSample result;
    result.scattering = max(rayleigh + mie, 0.0f);
    result.extinction = max(extinction, 0.0f);
    return result;
}

float3 integrate_optical_depth(float3 origin, float3 direction, uint samples)
{
    const float radius = safe_radius(planetParameters.x);
    const float atmosphereRadius = radius + max(finite_or_zero(planetParameters.y), MIN_VALUE);
    const float2 atmosphereHit = ray_sphere_intersection(origin, direction, atmosphereRadius);
    if (atmosphereHit.y <= 0.0f)
        return float3(MAX_OPTICAL_DEPTH, MAX_OPTICAL_DEPTH, MAX_OPTICAL_DEPTH);

    const float2 groundHit = ray_sphere_intersection(origin, direction, radius);
    float start = max(atmosphereHit.x, 0.0f);
    float end = atmosphereHit.y;
    if (groundHit.x > start)
        end = min(end, groundHit.x);
    if (end <= start + MIN_VALUE)
        return 0.0f;

    const uint sampleCount = clamp(samples, 4u, 256u);
    const float stepLength = (end - start) / (float)sampleCount;
    float3 opticalDepth = 0.0f;
    [loop]
    for (uint index = 0u; index < sampleCount; ++index)
    {
        const float distance = start + ((float)index + 0.5f) * stepLength;
        const MediumSample medium = sample_medium(origin + direction * distance);
        opticalDepth = min(opticalDepth + medium.extinction * stepLength,
            float3(MAX_OPTICAL_DEPTH, MAX_OPTICAL_DEPTH, MAX_OPTICAL_DEPTH));
    }
    return opticalDepth;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    transmittanceLut.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) /
        max(float2((float)width, (float)height), float2(1.0f, 1.0f));
    const float radius = safe_radius(planetParameters.x);
    const float altitude = uv.y * max(finite_or_zero(planetParameters.y), MIN_VALUE);
    const float viewHeight = radius + altitude;
    // The LUT stores an outward ray from a point on the positive Y axis.  A
    // 1-D height plus zenith-cosine parameterization is stable and can be
    // sampled by both the multi-scattering and sky-view stages.
    const float viewCosine = uv.x * 2.0f - 1.0f;
    const float viewSine = sqrt(saturate(1.0f - viewCosine * viewCosine));
    const float3 origin = float3(0.0f, viewHeight, 0.0f);
    const float3 direction = safe_normalize(float3(viewSine, viewCosine, 0.0f),
        float3(0.0f, 1.0f, 0.0f));
    const float3 opticalDepth = integrate_optical_depth(origin, direction, quality.x);
    const float3 transmittance = finite_or_zero(exp(-min(opticalDepth,
        float3(MAX_OPTICAL_DEPTH, MAX_OPTICAL_DEPTH, MAX_OPTICAL_DEPTH))));
    transmittanceLut[dispatchThreadId.xy] = float4(saturate(transmittance), 1.0f);
}
