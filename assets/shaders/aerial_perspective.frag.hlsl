// Applies the camera-volume atmosphere LUT to opaque scene color.
//
// SDL graphics ABI:
//   t0/s0, space2 = scene-linear HDR
//   t1/s1, space2 = device depth
//   t2/s2, space2 = RGBA16F camera volume
//   b0,    space3 = AerialPerspectiveSettings
//
// Camera-volume RGB is cumulative in-scattered radiance and A is mean
// transmittance. Sky pixels remain untouched because the sky pass already
// evaluates the atmosphere to infinity.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> sceneHdr : register(t0, space2);
Texture2D<float> sceneDepth : register(t1, space2);
Texture3D<float4> cameraVolume : register(t2, space2);
SamplerState sceneSampler : register(s0, space2);
SamplerState depthSampler : register(s1, space2);
SamplerState volumeSampler : register(s2, space2);

cbuffer AerialPerspectiveSettings : register(b0, space3)
{
    float4x4 inverseViewProjection;
    float4 cameraPosition;
    // x: near distance, y: far distance, z: distribution exponent,
    // w: enabled (zero keeps this pass a deterministic copy).
    float4 depthParameters;
    // xyz: volume dimensions, w: SkyAtmosphereDebugView numeric value.
    float4 volumeParameters;
};

float safe_finite(float value)
{
    return isfinite(value) ? value : 0.0f;
}

float3 safe_hdr(float3 value)
{
    return min(max(float3(safe_finite(value.x), safe_finite(value.y),
        safe_finite(value.z)), 0.0f), 65504.0f);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float4 scene = sceneHdr.SampleLevel(sceneSampler, input.texcoord, 0.0f);
    const float deviceDepth = sceneDepth.SampleLevel(depthSampler, input.texcoord, 0.0f);
    if (depthParameters.w < 0.5f || deviceDepth >= 0.99999f)
        return scene;

    const float2 clip = float2(input.texcoord.x * 2.0f - 1.0f,
        1.0f - input.texcoord.y * 2.0f);
    float4 world = mul(inverseViewProjection, float4(clip, saturate(deviceDepth), 1.0f));
    // Preserve the homogeneous sign. Using abs(w) mirrors positions behind
    // the origin for projections whose clip convention produces negative w,
    // turning nearby surfaces into falsely distant atmosphere samples.
    const float safeW = abs(world.w) > 0.000001f
        ? world.w
        : (world.w < 0.0f ? -0.000001f : 0.000001f);
    world.xyz /= safeW;
    const float distanceToCamera = length(world.xyz - cameraPosition.xyz);
    const float nearDistance = max(depthParameters.x, 0.0f);
    const float farDistance = max(depthParameters.y, nearDistance + 0.0001f);
    const float exponent = max(depthParameters.z, 1.0f);
    const float linearDepth01 = saturate((distanceToCamera - nearDistance) /
        (farDistance - nearDistance));
    const float volumeDepth = pow(linearDepth01, rcp(exponent));

    const float3 dimensions = max(volumeParameters.xyz, 1.0f);
    const float3 halfTexel = 0.5f / dimensions;
    const float3 volumeUv = clamp(float3(input.texcoord, volumeDepth),
        halfTexel, 1.0f - halfTexel);
    const float4 atmosphere = cameraVolume.SampleLevel(volumeSampler, volumeUv, 0.0f);
    const float transmittance = saturate(atmosphere.a);
    const float3 radiance = safe_hdr(atmosphere.rgb);

    // Stable numeric values from SkyAtmosphereDebugView. These modes expose
    // the same data sampled by the final composite, not a second debug model.
    if (volumeParameters.w > 3.5f && volumeParameters.w < 4.5f)
        return float4(radiance, 1.0f);
    if (volumeParameters.w > 4.5f && volumeParameters.w < 5.5f)
        return float4(radiance + (1.0f - transmittance) * 0.05f, 1.0f);

    return float4(safe_hdr(max(scene.rgb, 0.0f) * transmittance + radiance), scene.a);
}
