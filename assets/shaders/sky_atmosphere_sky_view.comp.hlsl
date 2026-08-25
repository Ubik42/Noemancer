// Noemancer atmosphere sky-view LUT.
//
// Original clean-room implementation.  The parameterization keeps the
// production split used by WickedEngine's MIT atmosphere stages (commit
// f4a0d2635d5224b4509da164fa75d90fbdaaea26) while keeping all resources and
// ownership inside Noemancer's explicit SDL_GPU artifact ABI.
//
// SDL compute ABI: t0/s0 and t1/s1 in space0 = transmittance and
// multi-scattering sampled LUTs. u0 in space1 = RGBA16F sky-view result.
// b0 in space2 is
// shared by all atmosphere LUT stages.

Texture2D<float4> transmittanceLut : register(t0, space0);
Texture2D<float4> multiScatteringLut : register(t1, space0);
SamplerState transmittanceSampler : register(s0, space0);
SamplerState multiScatteringSampler : register(s1, space0);
RWTexture2D<float4> skyViewLut : register(u0, space1);

cbuffer SkyAtmosphereLutParameters : register(b0, space2)
{
    // x: planet radius (m), y: atmosphere height (m), z: camera height (m),
    // w: maximum ray distance (m).
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
static const float INV_PI = 0.31830988618f;
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

float3 safe_hdr(float3 value)
{
    return min(max(finite_or_zero(value), 0.0f), MAX_HDR);
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
    float3 rayleigh;
    float3 mie;
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
    MediumSample result;
    result.rayleigh = max(finite_or_zero(rayleighScattering.rgb), 0.0f) * rayleighDensity;
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
    const float2 size = max(float2((float)width, (float)height), float2(1.0f, 1.0f));
    const float2 uv = float2(saturate(zenithCosine * 0.5f + 0.5f), saturate(altitude01));
    const float2 texel = 0.5f / size;
    return saturate(transmittanceLut.SampleLevel(transmittanceSampler, clamp(uv, texel, 1.0f - texel), 0.0f).rgb);
}

float3 sample_multi_scattering(float altitude01, float zenithCosine)
{
    uint width;
    uint height;
    multiScatteringLut.GetDimensions(width, height);
    const float2 size = max(float2((float)width, (float)height), float2(1.0f, 1.0f));
    const float2 uv = float2(saturate(zenithCosine * 0.5f + 0.5f), saturate(altitude01));
    const float2 texel = 0.5f / size;
    return safe_hdr(multiScatteringLut.SampleLevel(multiScatteringSampler, clamp(uv, texel, 1.0f - texel), 0.0f).rgb);
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

float view_zenith_from_uv(float uv, float cameraHeight, float radius, out bool groundSide)
{
    const float safeHeight = max(cameraHeight, radius + 1.0f);
    const float horizonLength = sqrt(max(safeHeight * safeHeight - radius * radius, 0.0f));
    const float cosBeta = saturate(horizonLength / safeHeight);
    const float beta = acos(cosBeta);
    const float zenithHorizonAngle = PI - beta;
    if (uv < 0.5f)
    {
        float coordinate = saturate(uv * 2.0f);
        coordinate = 1.0f - coordinate;
        coordinate *= coordinate;
        coordinate = 1.0f - coordinate;
        groundSide = false;
        return cos(zenithHorizonAngle * coordinate);
    }

    float coordinate = saturate(uv * 2.0f - 1.0f);
    coordinate *= coordinate;
    groundSide = true;
    return cos(zenithHorizonAngle + beta * coordinate);
}

float3 make_view_direction(float viewCosine, float lightViewCosine, float3 sun)
{
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    const float sunUp = dot(sun, up);
    const float sunHorizontalLength = sqrt(max(1.0f - sunUp * sunUp, 0.0f));
    float3 sunHorizontal = sun - up * sunUp;
    sunHorizontal = safe_normalize(sunHorizontal, float3(1.0f, 0.0f, 0.0f));
    const float3 right = safe_normalize(cross(up, sunHorizontal), float3(0.0f, 0.0f, 1.0f));
    const float horizontalLength = sqrt(saturate(1.0f - viewCosine * viewCosine));
    const float requested = (lightViewCosine - viewCosine * sunUp) /
        max(sunHorizontalLength, MIN_VALUE);
    const float alongSun = clamp(requested, -horizontalLength, horizontalLength);
    const float acrossSun = sqrt(max(horizontalLength * horizontalLength - alongSun * alongSun, 0.0f));
    return safe_normalize(up * viewCosine + sunHorizontal * alongSun + right * acrossSun,
        up);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    skyViewLut.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) /
        max(float2((float)width, (float)height), float2(1.0f, 1.0f));
    const float radius = safe_radius(planetParameters.x);
    const float atmosphereHeight = max(finite_or_zero(planetParameters.y), MIN_VALUE);
    const float cameraHeight = max(finite_or_zero(planetParameters.z), radius + 1.0f);
    bool groundSide;
    const float viewCosine = view_zenith_from_uv(uv.y, cameraHeight, radius, groundSide);
    const float lightViewCosine = -((uv.x * uv.x) * 2.0f - 1.0f);
    const float3 sun = safe_sun_direction();
    const float3 viewDirection = make_view_direction(viewCosine, lightViewCosine, sun);
    const float3 origin = float3(0.0f, cameraHeight, 0.0f);
    const float atmosphereRadius = radius + atmosphereHeight;
    const float2 atmosphereHit = ray_sphere_intersection(origin, viewDirection, atmosphereRadius);
    if (atmosphereHit.y <= 0.0f)
    {
        skyViewLut[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    const float2 groundHit = ray_sphere_intersection(origin, viewDirection, radius);
    const bool hitsGround = groundHit.x > 0.0f;
    const float start = max(atmosphereHit.x, 0.0f);
    float end = atmosphereHit.y;
    if (hitsGround)
        end = min(end, groundHit.x);
    if (end <= start + MIN_VALUE)
    {
        skyViewLut[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    const uint sampleCount = clamp(quality.z, 4u, 128u);
    const float stepLength = (end - start) / (float)sampleCount;
    const float3 sunRadiance = max(finite_or_zero(sunIrradiance.rgb), 0.0f);
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 radiance = 0.0f;
    float3 viewTransmittance = 1.0f;
    [loop]
    for (uint index = 0u; index < sampleCount; ++index)
    {
        const float distance = start + ((float)index + 0.5f) * stepLength;
        const float3 position = origin + viewDirection * distance;
        const float3 localUp = safe_normalize(position, up);
        const MediumSample medium = sample_medium(position);
        const float altitude01 = saturate((length(position) - radius) / atmosphereHeight);
        const float sunCosine = dot(localUp, sun);
        const float3 transmittanceToSun = sample_transmittance(altitude01, sunCosine);
        const float3 multiScattering = sample_multi_scattering(altitude01, sunCosine);
        const float viewSunCosine = dot(viewDirection, sun);
        const float3 directScattering = medium.rayleigh * rayleigh_phase(viewSunCosine) +
            medium.mie * mie_phase(viewSunCosine, mieAbsorption.w);
        const float3 source = safe_hdr(directScattering * transmittanceToSun * sunRadiance +
            medium.rayleigh * multiScattering * 0.25f + medium.mie * multiScattering * 0.25f);
        const float3 segmentTransmittance = exp(-min(medium.extinction * stepLength,
            float3(80.0f, 80.0f, 80.0f)));
        const float3 integrated = source * (1.0f - segmentTransmittance) /
            max(medium.extinction, float3(MIN_VALUE, MIN_VALUE, MIN_VALUE));
        radiance += viewTransmittance * integrated;
        viewTransmittance *= segmentTransmittance;
    }

    if (hitsGround && end <= groundHit.x + MIN_VALUE)
    {
        const float3 groundPosition = origin + viewDirection * groundHit.x;
        const float3 normal = safe_normalize(groundPosition, up);
        const float3 groundTransmittance = sample_transmittance(0.0f, dot(normal, sun));
        const float groundLight = saturate(dot(normal, sun));
        const float3 albedo = max(finite_or_zero(groundAlbedo.rgb), 0.0f);
        const float3 directGround = albedo * groundLight * groundTransmittance *
            sunRadiance * INV_PI;
        const float3 skyGround = albedo * sample_multi_scattering(0.0f, dot(normal, sun)) * 0.08f;
        radiance += viewTransmittance * (directGround + skyGround);
    }

    const float angularRadius = max(finite_or_zero(sunDirection.w), MIN_VALUE);
    const float sunCosine = dot(viewDirection, sun);
    const float sunDisc = smoothstep(cos(angularRadius * 2.0f), cos(angularRadius), sunCosine);
    radiance += viewTransmittance * sunRadiance * sunDisc;
    skyViewLut[dispatchThreadId.xy] = float4(safe_hdr(radiance), 1.0f);
}
