// Noemancer atmosphere camera-volume / aerial-perspective LUT.
//
// This is an original clean-room implementation of the camera-volume stage
// used by the renderer-neutral sky contract.  It reuses the transmittance and
// multi-scattering LUTs produced by the preceding atmosphere stages; those
// resources are never recreated or copied into this stage.
//
// SDL compute ABI (deliberately explicit and shared with the other atmosphere
// compute stages):
//   t0/s0, space0 = transmittance RGBA16F LUT and linear sampler
//   t1/s1, space0 = multi-scattering RGBA16F LUT and linear sampler
//   u0,    space1 = camera volume RGBA16F UAV
//   b0,    space2 = SkyAtmosphereCameraVolumeParameters
//
// The output volume is addressed as [screen-x, screen-y, depth-slice].  RGB
// stores cumulative scene-linear in-scattered radiance from the camera to the
// slice depth.  A stores the mean view transmittance at that depth.  A caller
// can therefore apply aerial perspective to opaque geometry without exposing
// shader handles or a backend-specific texture type through the engine
// contract.  The volume is regenerated when the camera/atmosphere identity
// changes; temporal history belongs to the renderer adapter, not this kernel.

Texture2D<float4> transmittanceLut : register(t0, space0);
Texture2D<float4> multiScatteringLut : register(t1, space0);
SamplerState transmittanceSampler : register(s0, space0);
SamplerState multiScatteringSampler : register(s1, space0);
RWTexture3D<float4> cameraVolume : register(u0, space1);

cbuffer SkyAtmosphereCameraVolumeParameters : register(b0, space2)
{
    // x: planet radius (m), y: atmosphere height (m), z: reserved, w: max
    // physical atmosphere distance (m; used as a defensive clamp).
    float4 planetParameters;
    // x: Rayleigh scale height, y: Mie scale height, z: ozone centre height,
    // w: ozone full width (m).
    float4 densityParameters;
    float4 groundAlbedo;
    float4 rayleighScattering;
    float4 rayleighAbsorption;
    float4 mieScattering;
    // xyz: Mie absorption; w: Henyey-Greenstein anisotropy.
    float4 mieAbsorption;
    float4 ozoneAbsorption;
    // xyz: unit vector from the planet toward the sun; w: angular radius.
    float4 sunDirection;
    float4 sunIrradiance;
    // x/y: target dimensions, z: vertical tan-half-FOV, w: aspect ratio.
    float4 targetParameters;
    // x: near depth, y: far depth, z: depth distribution exponent, w reserved.
    float4 depthParameters;
    // Camera basis and position are in one camera-relative/rebased world.
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 planetCenter;
    // x: integration samples per cumulative slice, y/z reserved, w flags.
    uint4 quality;
};

static const float PI = 3.14159265359f;
static const float MIN_VALUE = 0.0001f;
static const float MAX_HDR = 65504.0f;

float finite_or_zero(float value)
{
    return isfinite(value) ? value : 0.0f;
}

float3 finite_or_zero(float3 value)
{
    return float3(finite_or_zero(value.x), finite_or_zero(value.y),
        finite_or_zero(value.z));
}

float3 safe_hdr(float3 value)
{
    return min(max(finite_or_zero(value), 0.0f), MAX_HDR);
}

float3 safe_normalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > MIN_VALUE * MIN_VALUE
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float safe_radius(float value)
{
    return max(finite_or_zero(value), MIN_VALUE);
}

float2 ray_sphere_intersection(float3 origin, float3 direction,
    float3 center, float radius)
{
    const float3 relativeOrigin = origin - center;
    const float b = dot(relativeOrigin, direction);
    const float c = dot(relativeOrigin, relativeOrigin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f || radius <= MIN_VALUE)
        return float2(-1.0f, -1.0f);
    const float root = sqrt(max(discriminant, 0.0f));
    return float2(-b - root, -b + root);
}

struct MediumSample
{
    float3 rayleigh;
    float3 mie;
    float3 extinction;
};

MediumSample sample_medium(float3 position)
{
    const float radius = safe_radius(planetParameters.x);
    const float height = max(length(position - planetCenter.xyz) - radius, 0.0f);
    const float rayleighHeight = max(finite_or_zero(densityParameters.x), MIN_VALUE);
    const float mieHeight = max(finite_or_zero(densityParameters.y), MIN_VALUE);
    const float ozoneWidth = max(finite_or_zero(densityParameters.w), MIN_VALUE);
    const float ozoneDensity = saturate(1.0f - abs(height -
        finite_or_zero(densityParameters.z)) / (0.5f * ozoneWidth));
    const float rayleighDensity = exp(-min(height / rayleighHeight, 80.0f));
    const float mieDensity = exp(-min(height / mieHeight, 80.0f));

    MediumSample result;
    result.rayleigh = max(finite_or_zero(rayleighScattering.rgb), 0.0f) *
        rayleighDensity;
    result.mie = max(finite_or_zero(mieScattering.rgb), 0.0f) * mieDensity;
    result.extinction = max(result.rayleigh + result.mie +
        max(finite_or_zero(rayleighAbsorption.rgb), 0.0f) * rayleighDensity +
        max(finite_or_zero(mieAbsorption.rgb), 0.0f) * mieDensity +
        max(finite_or_zero(ozoneAbsorption.rgb), 0.0f) * ozoneDensity, 0.0f);
    return result;
}

float3 sample_transmittance(float altitude01, float zenithCosine)
{
    uint width;
    uint height;
    transmittanceLut.GetDimensions(width, height);
    const float2 size = max(float2((float)width, (float)height),
        float2(1.0f, 1.0f));
    const float2 texel = 0.5f / size;
    const float2 uv = float2(saturate(zenithCosine * 0.5f + 0.5f),
        saturate(altitude01));
    return saturate(transmittanceLut.SampleLevel(transmittanceSampler,
        clamp(uv, texel, 1.0f - texel), 0.0f).rgb);
}

float3 sample_multi_scattering(float altitude01, float zenithCosine)
{
    uint width;
    uint height;
    multiScatteringLut.GetDimensions(width, height);
    const float2 size = max(float2((float)width, (float)height),
        float2(1.0f, 1.0f));
    const float2 texel = 0.5f / size;
    const float2 uv = float2(saturate(zenithCosine * 0.5f + 0.5f),
        saturate(altitude01));
    return safe_hdr(multiScatteringLut.SampleLevel(multiScatteringSampler,
        clamp(uv, texel, 1.0f - texel), 0.0f).rgb);
}

float rayleigh_phase(float cosine)
{
    return 3.0f * (1.0f + cosine * cosine) / (16.0f * PI);
}

float mie_phase(float cosine, float anisotropy)
{
    const float g = clamp(finite_or_zero(anisotropy), -0.95f, 0.95f);
    const float g2 = g * g;
    const float denominator = max(1.0f + g2 - 2.0f * g * cosine, MIN_VALUE);
    return (1.0f - g2) / (4.0f * PI * denominator * sqrt(denominator));
}

float3 make_view_direction(float2 uv, uint width, uint height)
{
    // The volume uses a top-left texture origin and a right-handed camera
    // basis.  targetParameters wins when authoring supplies a projection; the
    // finite fallback keeps a standalone compute dispatch deterministic.
    const float2 dimensions = max(float2((float)width, (float)height),
        float2(1.0f, 1.0f));
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float tanHalfFovY = max(finite_or_zero(targetParameters.z), 0.01f);
    const float aspect = max(finite_or_zero(targetParameters.w),
        dimensions.x / dimensions.y);
    const float3 forward = safe_normalize(cameraForward.xyz,
        float3(0.0f, 0.0f, -1.0f));
    const float3 right = safe_normalize(cameraRight.xyz,
        float3(1.0f, 0.0f, 0.0f));
    const float3 up = safe_normalize(cameraUp.xyz, float3(0.0f, 1.0f, 0.0f));
    return safe_normalize(forward + right * (ndc.x * tanHalfFovY * aspect) +
        up * (ndc.y * tanHalfFovY), forward);
}

float depth_at(float normalizedDepth, float nearDepth, float farDepth,
    float exponent)
{
    return nearDepth + (farDepth - nearDepth) *
        pow(saturate(normalizedDepth), max(exponent, 1.0f));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    uint slices;
    cameraVolume.GetDimensions(width, height, slices);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height ||
        dispatchThreadId.z >= slices)
        return;

    const float3 origin = finite_or_zero(cameraPosition.xyz);
    const float3 rayDirection = make_view_direction(
        (float2(dispatchThreadId.xy) + 0.5f) /
            max(float2((float)width, (float)height), float2(1.0f, 1.0f)),
        width, height);
    const float radius = safe_radius(planetParameters.x);
    const float atmosphereRadius = radius +
        max(finite_or_zero(planetParameters.y), MIN_VALUE);
    const float2 atmosphereHit = ray_sphere_intersection(origin, rayDirection,
        planetCenter.xyz, atmosphereRadius);
    const float2 groundHit = ray_sphere_intersection(origin, rayDirection,
        planetCenter.xyz, radius);
    const float nearDepth = max(finite_or_zero(depthParameters.x), 0.0f);
    const float authoredFarDepth = max(finite_or_zero(depthParameters.y),
        nearDepth + MIN_VALUE);
    const float physicalFarDepth = max(finite_or_zero(planetParameters.w),
        nearDepth + MIN_VALUE);
    // A zero/omitted physical distance keeps the authored depth range useful;
    // a positive atmosphere bound prevents an invalid project from dispatching
    // rays far beyond the medium represented by the LUTs.
    const float farDepth = planetParameters.w > MIN_VALUE
        ? min(authoredFarDepth, physicalFarDepth)
        : authoredFarDepth;
    const float exponent = max(finite_or_zero(depthParameters.z), 1.0f);
    const float depth01 = (float(dispatchThreadId.z) + 0.5f) /
        max((float)slices, 1.0f);
    const float depthEnd = depth_at(depth01, nearDepth, farDepth, exponent);

    // A volume cell before the atmosphere entry or after a ray miss is clear.
    // Keep the alpha contract explicit so an aerial-perspective composite can
    // distinguish clear air from a black/debug radiance value.
    if (atmosphereHit.y <= 0.0f || depthEnd <= max(atmosphereHit.x, 0.0f))
    {
        cameraVolume[dispatchThreadId] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float integrationStart = max(nearDepth, max(atmosphereHit.x, 0.0f));
    float integrationEnd = min(depthEnd, atmosphereHit.y);
    if (groundHit.x > integrationStart)
        integrationEnd = min(integrationEnd, groundHit.x);
    if (integrationEnd <= integrationStart + MIN_VALUE)
    {
        cameraVolume[dispatchThreadId] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    const uint sampleCount = clamp(quality.x, 4u, 64u);
    const float stepLength = (integrationEnd - integrationStart) /
        (float)sampleCount;
    const float3 sun = safe_normalize(sunDirection.xyz,
        float3(0.0f, 1.0f, 0.0f));
    const float3 sunRadiance = max(finite_or_zero(sunIrradiance.rgb), 0.0f);
    const float viewSunCosine = dot(rayDirection, sun);
    const float rayleighPhaseValue = rayleigh_phase(viewSunCosine);
    const float miePhaseValue = mie_phase(viewSunCosine, mieAbsorption.w);
    float3 radiance = 0.0f;
    float3 viewTransmittance = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (uint index = 0u; index < sampleCount; ++index)
    {
        const float distance = integrationStart +
            ((float)index + 0.5f) * stepLength;
        const float3 position = origin + rayDirection * distance;
        const float3 localUp = safe_normalize(position - planetCenter.xyz,
            float3(0.0f, 1.0f, 0.0f));
        const float altitude01 = saturate((length(position - planetCenter.xyz) -
            radius) / max(finite_or_zero(planetParameters.y), MIN_VALUE));
        const MediumSample medium = sample_medium(position);
        const float3 sunTransmittance = sample_transmittance(altitude01,
            dot(localUp, sun));
        const float3 multiScattering = sample_multi_scattering(altitude01,
            dot(localUp, sun));
        const float3 directScattering = medium.rayleigh * rayleighPhaseValue +
            medium.mie * miePhaseValue;
        const float3 source = safe_hdr(directScattering * sunTransmittance *
            sunRadiance + (medium.rayleigh + medium.mie) * multiScattering *
            0.25f);
        const float3 segmentTransmittance = exp(-min(medium.extinction *
            stepLength, float3(80.0f, 80.0f, 80.0f)));
        const float3 integrated = source * (1.0f - segmentTransmittance) /
            max(medium.extinction, float3(MIN_VALUE, MIN_VALUE, MIN_VALUE));
        radiance += viewTransmittance * integrated;
        viewTransmittance *= segmentTransmittance;
    }

    const float meanTransmittance = saturate((viewTransmittance.x +
        viewTransmittance.y + viewTransmittance.z) / 3.0f);
    cameraVolume[dispatchThreadId] = float4(safe_hdr(radiance),
        meanTransmittance);
}
