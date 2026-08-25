// Noemancer screen-space diffuse GI composite.
//
// SSGI is an incomplete visibility source. The resolved signal is therefore
// blended into the existing diffuse-IBL fallback only where confidence and
// material eligibility are valid. Off-screen rays, metallic surfaces and
// unsupported/disabled quality modes retain the original IBL term; there is
// no blind addition of the screen color.
//
// SDL_GPU graphics ABI (space2 sampled textures, space3 constants):
//   t0/s0 = scene HDR after opaque lighting (RGBA16F)
//   t1/s1 = resolved GI radiance/confidence (RGBA16F)
//   t2/s2 = resolved bent normal/visibility (RGBA16F)
//   t3/s3 = surface material properties (rgb base color, a metallic)
//   t4/s4 = diffuse indirect/IBL fallback (RGBA16F)
//   t5/s5 = specular indirect component to remove from the combined fallback
//   b0    = SsgiCompositeSettings (16 bytes)

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> sceneHdr : register(t0, space2);
SamplerState sceneSampler : register(s0, space2);
Texture2D<float4> resolvedRadiance : register(t1, space2);
SamplerState radianceSampler : register(s1, space2);
Texture2D<float4> resolvedBentNormal : register(t2, space2);
SamplerState bentSampler : register(s2, space2);
Texture2D<float4> materialProperties : register(t3, space2);
SamplerState materialSampler : register(s3, space2);
Texture2D<float4> diffuseIndirect : register(t4, space2);
SamplerState indirectSampler : register(s4, space2);
Texture2D<float4> specularIndirect : register(t5, space2);
SamplerState specularSampler : register(s5, space2);

cbuffer SsgiCompositeSettings : register(b0, space3)
{
    // x = enabled, y = strength, z = debug mode, w = material validity threshold.
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

float4 main(FragmentInput input) : SV_Target0
{
    const float2 uv = saturate(input.texcoord);
    const float4 sceneSample = sceneHdr.SampleLevel(sceneSampler, uv, 0.0f);
    const float3 scene = finite_color(sceneSample.rgb);
    const float4 resolved = resolvedRadiance.SampleLevel(radianceSampler, uv, 0.0f);
    const float4 bent = resolvedBentNormal.SampleLevel(bentSampler, uv, 0.0f);
    const float4 material = materialProperties.SampleLevel(materialSampler, uv, 0.0f);
    const float3 combinedIndirect = finite_color(diffuseIndirect.SampleLevel(indirectSampler, uv, 0.0f).rgb);
    const float3 fallbackDiffuse = max(combinedIndirect - finite_color(
        specularIndirect.SampleLevel(specularSampler, uv, 0.0f).rgb), 0.0f);
    const float3 albedo = saturate(float3(finite_or(material.x, 0.0f), finite_or(material.y, 0.0f),
        finite_or(material.z, 0.0f)));
    const float metallic = saturate(finite_or(material.w, 1.0f));
    const float diffuseWeight = saturate(1.0f - metallic);
    const float materialValidityThreshold = saturate(finite_or(composite.w, 0.0f));
    const bool materialValid = isfinite(material.w) && diffuseWeight >= materialValidityThreshold;
    const float confidence = saturate(finite_or(resolved.a, 0.0f));
    const float visibility = saturate(finite_or(bent.a, 0.0f));
    const float enabled = finite_or(composite.x, 0.0f) > 0.5f ? 1.0f : 0.0f;
    const float strength = max(finite_or(composite.y, 1.0f), 0.0f);
    const float replacementWeight = saturate(confidence * visibility * diffuseWeight *
        (materialValid ? 1.0f : 0.0f) * enabled);
    // Gather stores incoming diffuse radiance; surface albedo is applied once
    // here, while the previous diffuse IBL is removed only in the same region.
    const float3 ssgiDiffuse = finite_color(resolved.rgb) * albedo * strength;
    const float3 delta = ssgiDiffuse - fallbackDiffuse;
    const float3 composed = finite_color(scene + delta * replacementWeight);

    const float mode = finite_or(composite.z, 0.0f);
    if (mode > 0.5f && mode < 1.5f)
        return float4(confidence.xxx, 1.0f);
    if (mode > 1.5f && mode < 2.5f)
        return float4(replacementWeight.xxx, 1.0f);
    if (mode > 2.5f && mode < 3.5f)
        return float4(visibility.xxx, 1.0f);
    if (mode > 3.5f && mode < 4.5f)
        return float4(saturate(albedo * diffuseWeight), 1.0f);
    if (mode > 4.5f)
        return float4(finite_color(abs(delta)), 1.0f);
    return float4(composed, finite_or(sceneSample.a, 1.0f));
}
