// Noemancer dynamic procedural sky and atmosphere.
//
// Provenance and adaptation note:
// - The production-oriented separation of atmosphere geometry, density
//   sampling, phase functions and in-scattered luminance follows the shape of
//   WickedEngine's MIT sky-atmosphere implementation at commit
//   f4a0d2635d5224b4509da164fa75d90fbdaaea26, especially
//   WickedEngine/shaders/skyAtmosphere.hlsli and
//   WickedEngine/shaders/skyAtmosphere_skyViewLutCS.hlsl.
// - The ray/sphere intersection and finite optical-depth integration below are
//   an original clean-room implementation for Noemancer's full-screen pass;
//   no WickedEngine or Unreal source is copied here.  The reference comments
//   that point to Sebastien Hillaire's MIT UnrealEngineSkyAtmosphere work are
//   useful for the physical model, but this pass intentionally starts with a
//   self-contained direct raymarch so it has no LUT or backend-specific
//   resource contract.
//
// Binding ABI (owned by the renderer integration):
//   t0/s0, space2 = cached sky-view LUT and linear sampler.
//   b0, space3 = SkyAtmosphereSettings.
// The output is scene-linear HDR and flows into tone-map/exposure.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> skyViewLut : register(t0, space2);
SamplerState skyViewSampler : register(s0, space2);

cbuffer SkyAtmosphereSettings : register(b0, space3)
{
    // Camera-relative inverse view-projection.  The renderer should use the
    // same camera-relative origin for scene geometry and planetCenter.
    float4x4 inverseViewProjection;
    float4 cameraPosition; // xyz: camera position in the rebased world
    float4 cameraForward; // xyz: normalized fallback direction
    float4 planetCenter; // xyz: planet center in the same rebased world
    // x: ground radius, y: atmosphere radius, z: maximum ray distance, w: reserved
    float4 planetRadii;
    // xyz: unit direction from a point on the planet toward the sun; w: radiance scale
    float4 sunDirectionIntensity;
    // rgb: sun spectral tint; w: exposure compensation in stops
    float4 sunColorExposure;
    // rgb: Rayleigh scattering coefficient; w: density scale height
    float4 rayleighScatteringScaleHeight;
    // rgb: Mie scattering coefficient; w: density scale height
    float4 mieScatteringScaleHeight;
    // rgb: Mie absorption coefficient; w: Henyey-Greenstein anisotropy [-0.95, 0.95]
    float4 mieAbsorptionPhaseG;
    // rgb: ground albedo; w: ground bounce intensity
    float4 groundAlbedoIntensity;
    // rgb: fallback zenith tint used only when the camera ray misses the atmosphere;
    // w: fallback sky intensity
    float4 fallbackSky;
    // x: view samples, y: light samples, z: flags (bit 0 temporal jitter,
    // bit 1 cached sky-view LUT),
    // w: frame index used by the optional jitter hash
    uint4 quality;
    // xy: viewport size in pixels, z: time in seconds, w: reserved
    float4 viewportAndTime;
};

static const float PI = 3.14159265359f;
static const float INV_FOUR_PI = 0.07957747154594767f;
static const float MAX_HDR = 65504.0f;
static const float MIN_DISTANCE = 0.0001f;

float safe_finite(float value)
{
    return isfinite(value) ? value : 0.0f;
}

float3 safe_finite(float3 value)
{
    return float3(safe_finite(value.x), safe_finite(value.y), safe_finite(value.z));
}

float3 safe_hdr(float3 value)
{
    const float3 finiteValue = safe_finite(value);
    return min(max(finiteValue, float3(0.0f, 0.0f, 0.0f)),
        float3(MAX_HDR, MAX_HDR, MAX_HDR));
}

float3 safe_normalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > MIN_DISTANCE * MIN_DISTANCE
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float safe_radius(float value)
{
    return max(safe_finite(value), MIN_DISTANCE);
}

float3 safe_planet_center()
{
    return safe_finite(planetCenter.xyz);
}

// Returns the near/far distances along a ray, or (-1,-1) for a miss.
float2 ray_sphere_intersection(float3 origin, float3 direction, float3 center, float radius)
{
    const float3 relativeOrigin = origin - center;
    const float b = dot(relativeOrigin, direction);
    const float c = dot(relativeOrigin, relativeOrigin) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f || radius <= MIN_DISTANCE)
        return float2(-1.0f, -1.0f);

    const float root = sqrt(max(discriminant, 0.0f));
    return float2(-b - root, -b + root);
}

struct MediumSample
{
    float3 rayleighScattering;
    float3 mieScattering;
    float3 extinction;
};

MediumSample sample_medium(float3 position)
{
    MediumSample medium;
    const float3 fromCenter = position - safe_planet_center();
    const float height = max(length(fromCenter) - safe_radius(planetRadii.x), 0.0f);
    const float rayleighHeight = max(rayleighScatteringScaleHeight.w, MIN_DISTANCE);
    const float mieHeight = max(mieScatteringScaleHeight.w, MIN_DISTANCE);
    // Exponential density is clamped before multiplication so invalid or
    // extreme author values cannot create infinities in the HDR path.
    const float rayleighDensity = exp(-min(height / rayleighHeight, 80.0f));
    const float mieDensity = exp(-min(height / mieHeight, 80.0f));
    medium.rayleighScattering = safe_hdr(max(rayleighScatteringScaleHeight.rgb, 0.0f) * rayleighDensity);
    medium.mieScattering = safe_hdr(max(mieScatteringScaleHeight.rgb, 0.0f) * mieDensity);
    medium.extinction = safe_hdr(medium.rayleighScattering + medium.mieScattering +
        max(mieAbsorptionPhaseG.rgb, 0.0f) * mieDensity);
    return medium;
}

float rayleigh_phase(float cosine)
{
    return 3.0f * (1.0f + cosine * cosine) / (16.0f * PI);
}

float mie_phase(float cosine, float anisotropy)
{
    const float g = clamp(safe_finite(anisotropy), -0.95f, 0.95f);
    const float g2 = g * g;
    const float denominator = max(1.0f + g2 - 2.0f * g * cosine, 0.0001f);
    return (1.0f - g2) / (4.0f * PI * denominator * sqrt(denominator));
}

// Integrate optical depth toward the sun.  The explicit light sample count
// is intentionally separate from the view count: an editor can trade the
// expensive shadowed light integral for stable camera samples independently.
float3 sample_sun_transmittance(float3 position, float3 sunDirection, uint lightSamples)
{
    const float2 groundHit = ray_sphere_intersection(position, sunDirection,
        safe_planet_center(), safe_radius(planetRadii.x));
    if (groundHit.y >= 0.0f && groundHit.y > MIN_DISTANCE)
        return 0.0f;

    const float2 atmosphereHit = ray_sphere_intersection(position, sunDirection,
        safe_planet_center(), safe_radius(planetRadii.y));
    if (atmosphereHit.y <= 0.0f)
        return 1.0f;

    const float start = max(atmosphereHit.x, 0.0f);
    const float end = max(atmosphereHit.y, start);
    const uint samples = clamp(lightSamples, 1u, 64u);
    const float stepLength = (end - start) / (float)samples;
    float3 opticalDepth = 0.0f;
    [loop]
    for (uint index = 0u; index < samples; ++index)
    {
        const float distance = start + ((float)index + 0.5f) * stepLength;
        const MediumSample medium = sample_medium(position + sunDirection * distance);
        opticalDepth = min(opticalDepth + medium.extinction * stepLength,
            float3(80.0f, 80.0f, 80.0f));
    }
    return safe_finite(exp(-opticalDepth));
}

uint hash_u32(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float hash01(uint2 pixel, uint frame)
{
    const uint seed = hash_u32(pixel.x ^ hash_u32(pixel.y + 0x9e3779b9u) ^ hash_u32(frame));
    return (float)(seed & 0x00ffffffu) / 16777216.0f;
}

float3 fallback_sky_color(float3 direction, float3 sunDirection)
{
    const float horizon = saturate(0.5f + 0.5f * direction.y);
    const float zenithWeight = sqrt(horizon);
    const float3 zenith = max(fallbackSky.rgb, 0.0f);
    const float3 horizonTint = lerp(zenith * 0.30f, zenith * 0.75f, horizon);
    const float sunDisc = smoothstep(0.99925f, 0.99985f, dot(direction, sunDirection));
    return safe_hdr(lerp(horizonTint, zenith, zenithWeight) +
        sunDisc * max(sunColorExposure.rgb, 0.0f) * max(sunDirectionIntensity.w, 0.0f));
}

float3 reconstruct_world_direction(float2 texcoord)
{
    const float2 clip = texcoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 farPoint = mul(inverseViewProjection, float4(clip, 1.0f, 1.0f));
    const float safeW = safe_finite(farPoint.w);
    if (abs(safeW) <= MIN_DISTANCE)
        return safe_normalize(cameraForward.xyz, float3(0.0f, 1.0f, 0.0f));
    farPoint.xyz = safe_finite(farPoint.xyz / safeW);
    return safe_normalize(farPoint.xyz - cameraPosition.xyz,
        safe_normalize(cameraForward.xyz, float3(0.0f, 1.0f, 0.0f)));
}

float2 sky_view_uv(float3 direction, float3 sunDirection)
{
    const float radius = safe_radius(planetRadii.x);
    const float cameraHeight = max(length(cameraPosition.xyz - safe_planet_center()), radius + 1.0f);
    const float horizonLength = sqrt(max(cameraHeight * cameraHeight - radius * radius, 0.0f));
    const float beta = acos(saturate(horizonLength / cameraHeight));
    const float horizonAngle = PI - beta;
    const float viewAngle = acos(clamp(direction.y, -1.0f, 1.0f));
    float vertical;
    if (viewAngle <= horizonAngle)
    {
        const float coordinate = saturate(viewAngle / max(horizonAngle, MIN_DISTANCE));
        vertical = 0.5f * (1.0f - sqrt(saturate(1.0f - coordinate)));
    }
    else
    {
        const float coordinate = saturate((viewAngle - horizonAngle) / max(beta, MIN_DISTANCE));
        vertical = 0.5f * (1.0f + sqrt(coordinate));
    }
    const float horizontal = sqrt(saturate((1.0f - dot(direction, sunDirection)) * 0.5f));
    return float2(horizontal, vertical);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float3 rayOrigin = safe_finite(cameraPosition.xyz);
    const float3 rayDirection = reconstruct_world_direction(input.texcoord);
    const float3 sunDirection = safe_normalize(sunDirectionIntensity.xyz,
        float3(0.0f, 1.0f, 0.0f));
    const float3 fallback = fallback_sky_color(rayDirection, sunDirection);

    // Bit 1 selects the cached multi-LUT path. Bit 0 remains temporal jitter
    // for the direct fallback. Keeping both paths in one artifact lets the
    // runtime fail soft when LUT creation is unsupported by a backend.
    if ((quality.z & 2u) != 0u)
    {
        uint lutWidth;
        uint lutHeight;
        skyViewLut.GetDimensions(lutWidth, lutHeight);
        const float2 texel = 0.5f / max(float2((float)lutWidth, (float)lutHeight), 1.0f);
        const float2 uv = clamp(sky_view_uv(rayDirection, sunDirection), texel, 1.0f - texel);
        const float exposureStops = clamp(safe_finite(sunColorExposure.w), -16.0f, 16.0f);
        float3 lutRadiance = skyViewLut.SampleLevel(skyViewSampler, uv, 0.0f).rgb;
        // A sky background is not a planetary terrain renderer. Fade the LUT's
        // below-horizon ground solution into the authored lower hemisphere;
        // real scene geometry drawn by the opaque pass still occludes it.
        const float lowerHemisphere = saturate(-rayDirection.y * 6.0f);
        lutRadiance = lerp(lutRadiance, fallback * 0.35f, lowerHemisphere);
        return float4(safe_hdr(lutRadiance * exp2(exposureStops)), 1.0f);
    }

    const float2 atmosphereHit = ray_sphere_intersection(rayOrigin, rayDirection,
        safe_planet_center(), safe_radius(planetRadii.y));
    if (atmosphereHit.y <= 0.0f)
        return float4(fallback, 1.0f);

    const float start = max(atmosphereHit.x, 0.0f);
    float end = atmosphereHit.y;
    const float maxDistance = max(safe_finite(planetRadii.z), 0.0f);
    if (maxDistance > MIN_DISTANCE)
        end = min(end, maxDistance);

    const float2 groundHit = ray_sphere_intersection(rayOrigin, rayDirection,
        safe_planet_center(), safe_radius(planetRadii.x));
    const bool hitsGround = groundHit.y >= 0.0f && groundHit.y > start + MIN_DISTANCE;
    if (hitsGround)
        end = min(end, groundHit.y);
    if (end <= start + MIN_DISTANCE)
        return float4(fallback, 1.0f);

    const uint viewSamples = clamp(quality.x, 4u, 128u);
    const uint lightSamples = clamp(quality.y, 1u, 64u);
    const bool temporalJitter = (quality.z & 1u) != 0u;
    const uint2 pixel = (uint2)max(input.position.xy, 0.0f);
    const float jitter = temporalJitter ? hash01(pixel, quality.w) - 0.5f : 0.0f;
    const float rayLength = end - start;
    const float3 sunRadiance = max(sunColorExposure.rgb, 0.0f) *
        max(sunDirectionIntensity.w, 0.0f);
    const float rayleighPhaseValue = rayleigh_phase(dot(rayDirection, sunDirection));
    const float miePhaseValue = mie_phase(dot(rayDirection, sunDirection), mieAbsorptionPhaseG.w);
    float3 radiance = 0.0f;
    float3 viewTransmittance = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (uint index = 0u; index < viewSamples; ++index)
    {
        const float sample0 = ((float)index + max(jitter, -0.49f)) / (float)viewSamples;
        const float sample1 = ((float)index + 1.0f + max(jitter, -0.49f)) / (float)viewSamples;
        const float u0 = saturate(sample0);
        const float u1 = saturate(sample1);
        // Squared distribution spends more samples near the camera/ground,
        // where the density gradient and horizon silhouette change fastest.
        const float distance0 = start + rayLength * u0 * u0;
        const float distance1 = start + rayLength * u1 * u1;
        const float distance = 0.5f * (distance0 + distance1);
        const float stepLength = max(distance1 - distance0, MIN_DISTANCE);
        const float3 position = rayOrigin + rayDirection * distance;
        const MediumSample medium = sample_medium(position);
        const float3 sunTransmittance = sample_sun_transmittance(position,
            sunDirection, lightSamples);
        const float3 scattering = medium.rayleighScattering * rayleighPhaseValue +
            medium.mieScattering * miePhaseValue;
        const float3 source = safe_hdr(scattering * sunTransmittance * sunRadiance);
        const float3 segmentTransmittance = safe_finite(exp(-min(medium.extinction * stepLength,
            float3(80.0f, 80.0f, 80.0f))));
        const float3 integrated = source * (1.0f - segmentTransmittance) /
            max(medium.extinction, float3(MIN_DISTANCE, MIN_DISTANCE, MIN_DISTANCE));
        radiance += viewTransmittance * integrated;
        viewTransmittance *= segmentTransmittance;
    }

    if (hitsGround && end <= groundHit.y + MIN_DISTANCE)
    {
        const float3 groundPosition = rayOrigin + rayDirection * groundHit.y;
        const float3 groundNormal = safe_normalize(groundPosition - safe_planet_center(),
            float3(0.0f, 1.0f, 0.0f));
        const float groundSun = saturate(dot(groundNormal, sunDirection));
        const float3 groundTransmittance = sample_sun_transmittance(groundPosition,
            sunDirection, lightSamples);
        const float3 groundAlbedo = max(groundAlbedoIntensity.rgb, 0.0f) *
            max(groundAlbedoIntensity.w, 0.0f);
        // The planet surface is a Lambertian receiver, so direct lighting must
        // include the authored solar radiance.  Keep a small sky-irradiance
        // approximation in the direct-raymarch fallback: otherwise every
        // below-horizon background pixel becomes mathematically black when the
        // sun is behind the local horizon.  The LUT path replaces this term
        // with multi-scattered atmosphere irradiance.
        const float3 directGround = groundAlbedo * groundSun * groundTransmittance *
            sunRadiance * (4.0f * INV_FOUR_PI);
        const float3 skyGround = groundAlbedo * fallback * 0.08f;
        radiance += viewTransmittance * (directGround + skyGround);
    }

    // Exposure is applied before the pass hands scene-linear HDR to tone map.
    // Clamp the authoring range and preserve finite behavior for hot sun values.
    const float exposureStops = clamp(safe_finite(sunColorExposure.w), -16.0f, 16.0f);
    const float exposure = min(max(exp2(exposureStops), 0.0f), 256.0f);
    return float4(safe_hdr(radiance * exposure), 1.0f);
}
