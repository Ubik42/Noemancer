// Noemancer low-cost analytic atmosphere fallback.
//
// This is deliberately a separate artifact from sky_atmosphere.frag.hlsl.  It
// is the bounded low/offline path: one full-screen draw, no texture reads and
// no raymarch loop.  The scattering model is a clean-room approximation of
// the same Rayleigh/Mie quantities used by the LUT path.  It uses a closed-form
// exponential column integral evaluated with a locally-curved planar slope,
// then evaluates the direct-light source at one representative point on the
// view ray.  This keeps the cost O(1) while retaining the useful physical
// controls (scale heights, per-metre coefficients, phase g and solar tint).
//
// Provenance and adaptation note:
// - The parameter vocabulary and phase-function choices are compatible with
//   Sebastien Hillaire's Unreal atmospheric model and Bruneton's reference
//   model, both of which are useful public descriptions of the equations.
// - The closed-form column approximation and the renderer-facing ABI below
//   are an original clean-room adaptation for Noemancer; no reference source
//   is copied here.  The full LUT path remains the quality path for curved
//   atmosphere, multiple scattering and detailed aerial perspective.
//
// ABI (must remain byte-compatible with SkyAtmosphereGpuData):
//   b0, space3 = SkyAtmosphereSettings (272 bytes).
//   No textures, samplers or storage buffers are sampled by this shader.
//
// Coordinate and exposure convention:
//   cameraPosition and planetCenter are camera-relative/rebased world-space
//   metres. planetRadii.x is the ground radius, planetRadii.y the atmosphere
//   radius, and planetRadii.z is the maximum analytic ray distance. The sun
//   direction points from the planet toward the light. sunColorExposure.rgb is
//   linear solar tint and sunDirectionIntensity.w is its scalar intensity.
//   sunColorExposure.w is exposure compensation in stops, applied to the
//   scene-linear HDR result before tone mapping. The full-screen vertex ABI
//   uses top-left UVs and reconstructs a far-plane ray from inverseViewProjection.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

cbuffer SkyAtmosphereSettings : register(b0, space3)
{
    // Camera-relative inverse view-projection. The renderer uses the same
    // rebased origin for scene geometry, cameraPosition and planetCenter.
    float4x4 inverseViewProjection;
    float4 cameraPosition; // xyz: camera position in the rebased world
    float4 cameraForward; // xyz: normalized fallback direction
    float4 planetCenter; // xyz: planet center in the rebased world
    // x: ground radius, y: atmosphere radius, z: maximum ray distance, w: reserved
    float4 planetRadii;
    // xyz: unit direction from a point on the planet toward the sun; w: radiance scale
    float4 sunDirectionIntensity;
    // rgb: linear solar tint; w: exposure compensation in stops
    float4 sunColorExposure;
    // rgb: Rayleigh scattering coefficient; w: density scale height
    float4 rayleighScatteringScaleHeight;
    // rgb: Mie scattering coefficient; w: density scale height
    float4 mieScatteringScaleHeight;
    // rgb: Mie absorption coefficient; w: Henyey-Greenstein anisotropy [-0.95, 0.95]
    float4 mieAbsorptionPhaseG;
    // rgb: ground albedo; w: ground bounce intensity
    float4 groundAlbedoIntensity;
    // rgb: fallback zenith tint; w: fallback sky intensity
    float4 fallbackSky;
    // x: nominal view samples, y: nominal light samples, z: feature flags,
    // w: frame index. The analytic path intentionally ignores sample counts:
    // its work is fixed and does not silently become a raymarch.
    uint4 quality;
    // xy: viewport size in pixels, z: time in seconds, w: reserved
    float4 viewportAndTime;
};

static const float PI = 3.14159265359f;
static const float INV_PI = 0.318309886184f;
static const float MIN_DISTANCE = 0.0001f;
static const float MAX_HDR = 65504.0f;
static const float SUN_ANGULAR_RADIUS_RAD = 0.004675f;

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

// Returns near/far distances along a ray, or (-1,-1) for a miss.
float2 ray_sphere_intersection(float3 origin, float3 direction,
    float3 center, float radius)
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

float3 planet_up(float3 position)
{
    return safe_normalize(position - safe_planet_center(), float3(0.0f, 1.0f, 0.0f));
}

// Closed-form integral of exp(-height/H) along one straight segment. A
// second-order curvature term adjusts the local slope toward a spherical
// atmosphere, without introducing any loop or texture dependency.
float exponential_column(float3 origin, float3 direction, float distance, float scaleHeight)
{
    const float h = max(length(origin - safe_planet_center()) - safe_radius(planetRadii.x), 0.0f);
    const float heightScale = max(safe_finite(scaleHeight), 1.0f);
    const float radialSlope = dot(direction, planet_up(origin));
    const float curvature = 1.0f / (2.0f * safe_radius(planetRadii.x));
    const float effectiveSlope = clamp(radialSlope + curvature * distance, -0.5f, 0.5f);
    const float initialDensity = exp(-clamp(h / heightScale, 0.0f, 80.0f));
    const float normalizedSlope = effectiveSlope / heightScale;
    const float safeDistance = max(safe_finite(distance), 0.0f);
    if (abs(normalizedSlope) < 0.00001f)
        return min(initialDensity * safeDistance, 100000000.0f);
    const float exponent = clamp(-normalizedSlope * safeDistance, -80.0f, 80.0f);
    const float integral = initialDensity * (1.0f - exp(exponent)) / normalizedSlope;
    return min(max(safe_finite(integral), 0.0f), 100000000.0f);
}

struct OpticalColumns
{
    float rayleigh;
    float mie;
};

OpticalColumns view_columns(float3 origin, float3 direction, float distance)
{
    OpticalColumns columns;
    columns.rayleigh = exponential_column(origin, direction, distance,
        rayleighScatteringScaleHeight.w);
    columns.mie = exponential_column(origin, direction, distance,
        mieScatteringScaleHeight.w);
    return columns;
}

float3 optical_depth(OpticalColumns columns)
{
    const float3 rayleighExtinction = max(rayleighScatteringScaleHeight.rgb, 0.0f);
    const float3 mieExtinction = max(mieScatteringScaleHeight.rgb, 0.0f) +
        max(mieAbsorptionPhaseG.rgb, 0.0f);
    return min(max(safe_finite(columns.rayleigh * rayleighExtinction +
        columns.mie * mieExtinction), 0.0f),
        float3(80.0f, 80.0f, 80.0f));
}

float3 rayleigh_phase(float cosine)
{
    return (3.0f / (16.0f * PI)) * (1.0f + cosine * cosine);
}

float mie_phase(float cosine, float anisotropy)
{
    const float g = clamp(safe_finite(anisotropy), -0.95f, 0.95f);
    const float g2 = g * g;
    const float denominator = max(1.0f + g2 - 2.0f * g * cosine, 0.0001f);
    return (1.0f - g2) / (4.0f * PI * denominator * sqrt(denominator));
}

float3 sun_transmittance(float3 position, float3 sunDirection)
{
    const float3 center = safe_planet_center();
    const float groundRadius = safe_radius(planetRadii.x);
    const float atmosphereRadius = safe_radius(planetRadii.y);
    const float2 groundHit = ray_sphere_intersection(position, sunDirection, center, groundRadius);
    if (groundHit.y > MIN_DISTANCE)
        return 0.0f;

    const float2 atmosphereHit = ray_sphere_intersection(position, sunDirection, center,
        atmosphereRadius);
    if (atmosphereHit.y <= 0.0f)
        return 1.0f;

    const float start = max(atmosphereHit.x, 0.0f);
    const float distance = max(atmosphereHit.y - start, 0.0f);
    const float3 segmentOrigin = position + sunDirection * start;
    const OpticalColumns columns = view_columns(segmentOrigin, sunDirection, distance);
    return safe_finite(exp(-optical_depth(columns)));
}

float3 fallback_sky_color(float3 direction, float3 sunDirection)
{
    const float3 up = planet_up(cameraPosition.xyz);
    const float horizon = saturate(0.5f + 0.5f * dot(direction, up));
    const float zenithWeight = sqrt(horizon);
    const float3 zenith = max(safe_finite(fallbackSky.rgb), 0.0f);
    const float3 horizonTint = lerp(zenith * 0.30f, zenith * 0.75f, horizon);
    const float sunCosine = dot(direction, sunDirection);
    const float sunEdge = cos(SUN_ANGULAR_RADIUS_RAD * 1.35f);
    const float sunCore = cos(SUN_ANGULAR_RADIUS_RAD * 0.35f);
    const float sunDisc = smoothstep(sunEdge, sunCore, sunCosine);
    const float3 solarRadiance = max(safe_finite(sunColorExposure.rgb), 0.0f) *
        max(safe_finite(sunDirectionIntensity.w), 0.0f);
    return safe_hdr(lerp(horizonTint, zenith, zenithWeight) + sunDisc * solarRadiance);
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

float4 main(FragmentInput input) : SV_Target0
{
    const float3 origin = safe_finite(cameraPosition.xyz);
    const float3 direction = reconstruct_world_direction(input.texcoord);
    const float3 sunDirection = safe_normalize(sunDirectionIntensity.xyz,
        float3(0.0f, 1.0f, 0.0f));
    const float3 fallback = fallback_sky_color(direction, sunDirection);
    const float3 center = safe_planet_center();

    const float2 atmosphereHit = ray_sphere_intersection(origin, direction, center,
        safe_radius(planetRadii.y));
    if (atmosphereHit.y <= 0.0f)
        return float4(fallback, 1.0f);

    const float start = max(atmosphereHit.x, 0.0f);
    float end = max(atmosphereHit.y, start);
    const float maxDistance = max(safe_finite(planetRadii.z), 0.0f);
    if (maxDistance > MIN_DISTANCE)
        end = min(end, start + maxDistance);

    const float2 groundHit = ray_sphere_intersection(origin, direction, center,
        safe_radius(planetRadii.x));
    const bool hitsGround = groundHit.y > start + MIN_DISTANCE;
    if (hitsGround)
        end = min(end, groundHit.y);
    const float distance = max(end - start, 0.0f);
    if (distance <= MIN_DISTANCE)
        return float4(fallback, 1.0f);

    const float3 segmentOrigin = origin + direction * start;
    const OpticalColumns columns = view_columns(segmentOrigin, direction, distance);
    const float3 viewOpticalDepth = optical_depth(columns);
    const float3 viewTransmittance = safe_finite(exp(-viewOpticalDepth));
    const float3 samplePosition = segmentOrigin + direction * (0.5f * distance);
    const float3 lightTransmittance = sun_transmittance(samplePosition, sunDirection);
    const float3 solarRadiance = max(safe_finite(sunColorExposure.rgb), 0.0f) *
        max(safe_finite(sunDirectionIntensity.w), 0.0f);
    const float cosine = dot(direction, sunDirection);
    const float3 scattering = columns.rayleigh * max(rayleighScatteringScaleHeight.rgb, 0.0f) *
        rayleigh_phase(cosine) + columns.mie * max(mieScatteringScaleHeight.rgb, 0.0f) *
        mie_phase(cosine, mieAbsorptionPhaseG.w);
    float3 radiance = scattering * lightTransmittance * solarRadiance;

    if (hitsGround && groundHit.y <= end + MIN_DISTANCE)
    {
        const float3 groundPosition = origin + direction * groundHit.y;
        const float3 groundNormal = planet_up(groundPosition);
        const float groundSun = saturate(dot(groundNormal, sunDirection));
        const float3 groundAlbedo = max(safe_finite(groundAlbedoIntensity.rgb), 0.0f) *
            max(safe_finite(groundAlbedoIntensity.w), 0.0f);
        const float3 directGround = groundAlbedo * groundSun *
            sun_transmittance(groundPosition, sunDirection) * solarRadiance * INV_PI;
        // The authored fallback is a deliberately small sky-irradiance term;
        // it keeps the lower hemisphere readable when the sun is occluded.
        const float3 ambientGround = groundAlbedo * fallback * 0.08f;
        radiance += viewTransmittance * (directGround + ambientGround);
    }
    else
    {
        // A small horizon floor avoids a black seam in very thin/low-density
        // atmospheres while the physically-controlled single scatter remains
        // the dominant signal.
        const float horizon = saturate(1.0f - abs(dot(direction, planet_up(origin))) * 8.0f);
        radiance += fallback * horizon * 0.025f * (1.0f - viewTransmittance);
    }

    const float exposureStops = clamp(safe_finite(sunColorExposure.w), -16.0f, 16.0f);
    const float exposure = min(max(exp2(exposureStops), 0.0f), 256.0f);
    // The one-point source estimate otherwise over-integrates long zenith
    // columns relative to the multi-scattering LUT reference.  This fixed
    // energy calibration keeps the low tier inside the shared HDR contract;
    // it is not a hidden quality-dependent sample loop.
    static const float ANALYTIC_RADIANCE_CALIBRATION = 0.5f;
    return float4(safe_hdr(radiance * exposure * ANALYTIC_RADIANCE_CALIBRATION), 1.0f);
}
