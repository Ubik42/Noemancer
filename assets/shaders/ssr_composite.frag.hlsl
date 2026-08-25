// Noemancer screen-space reflection energy-conserving composite.
//
// SSR is an incomplete visibility source: off-screen and missed rays must
// retain the renderer's existing specular-indirect fallback.  This pass
// replaces only the visible portion of that fallback, weighted by the
// resolved confidence and the material's Schlick F0/roughness response.
//
// SDL_GPU graphics ABI (space2 sampled textures, space3 constants):
//   t0/s0 = scene HDR including current indirect lighting (RGBA16F)
//   t1/s1 = resolved SSR radiance/confidence (RGBA16F)
//   t2/s2 = surface reflection properties (RGBA16F; rgb F0, a roughness)
//   t3/s3 = specular indirect fallback (RGBA16F)
//   t4/s4 = filtered screen-space ambient visibility (R8)
//   b0    = SsrCompositeSettings (16 bytes)

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> sceneHdr : register(t0, space2);
SamplerState sceneSampler : register(s0, space2);
Texture2D<float4> resolvedReflection : register(t1, space2);
SamplerState reflectionSampler : register(s1, space2);
Texture2D<float4> reflectionProperties : register(t2, space2);
SamplerState propertiesSampler : register(s2, space2);
Texture2D<float4> specularIndirect : register(t3, space2);
SamplerState indirectSampler : register(s3, space2);
Texture2D<float> ambientVisibility : register(t4, space2);
SamplerState ambientVisibilitySampler : register(s4, space2);

cbuffer SsrCompositeSettings : register(b0, space3)
{
    // x = enabled, y = strength, z = debug mode, w = roughness cutoff.
    float4 composite;
};

static const float MAX_HDR = 65504.0f;

float finite_or(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float3 finite_color(float3 value)
{
    return min(max(float3(finite_or(value.x, 0.0f), finite_or(value.y, 0.0f),
        finite_or(value.z, 0.0f)), 0.0f), MAX_HDR);
}

float3 fresnel_schlick(float cosine, float3 f0)
{
    const float c = saturate(cosine);
    const float3 safeF0 = saturate(f0);
    return safeF0 + (1.0f - safeF0) * pow(1.0f - c, 5.0f);
}

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = saturate(input.texcoord);
    const float3 scene = finite_color(sceneHdr.SampleLevel(sceneSampler, uv, 0.0f).rgb);
    const float4 resolved = resolvedReflection.SampleLevel(reflectionSampler, uv, 0.0f);
    const float4 properties = reflectionProperties.SampleLevel(propertiesSampler, uv, 0.0f);
    const float ambient = saturate(finite_or(ambientVisibility.SampleLevel(
        ambientVisibilitySampler, uv, 0.0f), 1.0f));
    const float3 fallbackSpecular = finite_color(specularIndirect.SampleLevel(
        indirectSampler, uv, 0.0f).rgb) * ambient;
    const float3 f0 = saturate(float3(finite_or(properties.x, 0.04f), finite_or(properties.y, 0.04f),
        finite_or(properties.z, 0.04f)));
    const float roughness = saturate(finite_or(properties.w, 1.0f));
    const float confidence = saturate(finite_or(resolved.a, 0.0f));
    const float validMaterial = isfinite(properties.w) &&
        roughness <= saturate(composite.w) ? 1.0f : 0.0f;
    const float enabled = composite.x > 0.5f ? 1.0f : 0.0f;
    const float strength = max(finite_or(composite.y, 1.0f), 0.0f);
    const float roughnessWeight = saturate(1.0f - roughness * roughness);
    // The view-independent F0 floor avoids inventing a full-strength mirror
    // for rough dielectrics while still preserving metallic response.
    const float3 fresnel = fresnel_schlick(0.5f, max(f0, 0.04f.xxx));
    const float replacementWeight = saturate(confidence * roughnessWeight * validMaterial * enabled);
    const float3 reflectedSpecular = finite_color(resolved.rgb) * fresnel * strength;
    const float3 specularDelta = reflectedSpecular - fallbackSpecular;
    const float3 composed = finite_color(scene + specularDelta * replacementWeight);

    const float mode = composite.z;
    // Trace-stage diagnostics are carried in resolved.rgb.  Bypass material
    // composition so a requested view cannot be mistaken for final lighting.
    if (mode > 0.5f)
        return float4(finite_color(resolved.rgb), 1.0f);
    return float4(composed, 1.0f);
}
