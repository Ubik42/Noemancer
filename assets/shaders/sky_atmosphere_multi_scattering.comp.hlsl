// Noemancer atmosphere multi-scattering LUT.
//
// Original clean-room implementation.  The stage boundary follows the
// transmittance -> multi-scattering split documented by WickedEngine's MIT
// sky-atmosphere shaders at commit f4a0d2635d5224b4509da164fa75d90fbdaaea26;
// no third-party shader code or binding layout is copied.
//
// SDL compute ABI: t0/s0 in space0 = transmittance LUT and linear sampler.
// u0 in space1 = RGBA16F multi-scattering result. b0 in space2 is shared
// with the transmittance stage so every LUT observes one physical revision.

Texture2D<float4> transmittanceLut : register(t0, space0);
SamplerState linearSampler : register(s0, space0);
RWTexture2D<float4> multiScatteringLut : register(u0, space1);

cbuffer SkyAtmosphereLutParameters : register(b0, space2)
{
    float4 planetParameters;
    float4 densityParameters;
    float4 groundAlbedo;
    float4 rayleighScattering;
    float4 rayleighAbsorption;
    float4 mieScattering;
    float4 mieAbsorption;
    float4 ozoneAbsorption;
    float4 sunDirection;
    float4 sunIrradiance;
    float4 targetParameters;
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
    return float3(finite_or_zero(value.x), finite_or_zero(value.y), finite_or_zero(value.z));
}

float3 safe_normalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > MIN_VALUE * MIN_VALUE ? value * rsqrt(lengthSquared) : fallback;
}

float3 safe_hdr(float3 value)
{
    return min(max(finite_or_zero(value), 0.0f), MAX_HDR);
}

float safe_radius(float value)
{
    return max(finite_or_zero(value), MIN_VALUE);
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
    const float ozoneDensity = saturate(1.0f - abs(height - finite_or_zero(densityParameters.z)) /
        (0.5f * ozoneWidth));
    const float rayleighDensity = exp(-min(height / rayleighHeight, 80.0f));
    const float mieDensity = exp(-min(height / mieHeight, 80.0f));
    const float3 rayleigh = max(finite_or_zero(rayleighScattering.rgb), 0.0f) * rayleighDensity;
    const float3 mie = max(finite_or_zero(mieScattering.rgb), 0.0f) * mieDensity;
    const float3 extinction = rayleigh + mie +
        max(finite_or_zero(rayleighAbsorption.rgb), 0.0f) * rayleighDensity +
        max(finite_or_zero(mieAbsorption.rgb), 0.0f) * mieDensity +
        max(finite_or_zero(ozoneAbsorption.rgb), 0.0f) * ozoneDensity;
    MediumSample result;
    result.scattering = max(rayleigh + mie, 0.0f);
    result.extinction = max(extinction, MIN_VALUE);
    return result;
}

float3 sample_transmittance(float altitude01, float zenithCosine)
{
    uint width;
    uint height;
    transmittanceLut.GetDimensions(width, height);
    const float2 size = max(float2((float)width, (float)height), float2(1.0f, 1.0f));
    const float2 uv = float2(saturate(zenithCosine * 0.5f + 0.5f), saturate(altitude01));
    const float2 texel = 0.5f / size;
    return saturate(transmittanceLut.SampleLevel(linearSampler, clamp(uv, texel, 1.0f - texel), 0.0f).rgb);
}

float3 make_direction(float cosine, float azimuth)
{
    const float sine = sqrt(saturate(1.0f - cosine * cosine));
    return float3(cos(azimuth) * sine, cosine, sin(azimuth) * sine);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    multiScatteringLut.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) /
        max(float2((float)width, (float)height), float2(1.0f, 1.0f));
    const float radius = safe_radius(planetParameters.x);
    const float altitude01 = saturate(uv.y);
    const float altitude = altitude01 * max(finite_or_zero(planetParameters.y), MIN_VALUE);
    const float3 position = float3(0.0f, radius + altitude, 0.0f);
    const float sunCosine = uv.x * 2.0f - 1.0f;
    const float3 mediumScattering = sample_medium(position).scattering;
    const float3 mediumExtinction = sample_medium(position).extinction;
    const float3 albedo = saturate(mediumScattering / max(mediumExtinction,
        float3(MIN_VALUE, MIN_VALUE, MIN_VALUE)));
    const float3 sun = max(finite_or_zero(sunIrradiance.rgb), 0.0f);
    const float3 transmittanceToSun = sample_transmittance(altitude01, sunCosine);

    // Estimate the diffuse incoming field with a bounded cosine-weighted
    // angular set.  This makes quality.y a real cost/quality control while
    // keeping the LUT cheap enough to regenerate when the sun moves.
    const uint sampleCount = clamp(quality.y, 4u, 64u);
    float3 diffuseVisibility = 0.0f;
    [loop]
    for (uint index = 0u; index < sampleCount; ++index)
    {
        const float u = ((float)index + 0.5f) / (float)sampleCount;
        const float cosine = sqrt(saturate(u));
        const float azimuth = 2.0f * PI * frac((float)index * 0.61803398875f);
        const float3 direction = make_direction(cosine, azimuth);
        diffuseVisibility += (1.0f - sample_transmittance(altitude01, direction.y)) * cosine;
    }
    diffuseVisibility /= (float)sampleCount;

    // The first term is the sun-lit first bounce; the second is a bounded
    // diffuse estimate of higher orders.  The result is intentionally stored
    // in scene-linear HDR so the later sky-view and tone-map stages can apply
    // one exposure policy rather than baking display transforms into a LUT.
    const float3 firstBounce = sun * (1.0f - transmittanceToSun) * albedo;
    const float3 higherOrders = sun * diffuseVisibility * albedo * 0.5f;
    const float3 result = safe_hdr(firstBounce + higherOrders);
    multiScatteringLut[dispatchThreadId.xy] = float4(result, 1.0f);
}
