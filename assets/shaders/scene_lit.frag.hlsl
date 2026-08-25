struct FragmentInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 color : TEXCOORD2;
    float4 material : TEXCOORD3;
    float2 texcoord : TEXCOORD4;
    float4 worldTangent : TEXCOORD5;
    float4 emissiveNormal : TEXCOORD6;
    float4 occlusionAlphaFlags : TEXCOORD7;
    float2 motion : TEXCOORD8;
    nointerpolation uint instanceObjectId : TEXCOORD9;
    bool frontFace : SV_IsFrontFace;
};

Texture2DArray<float> shadowMap : register(t0, space2);
SamplerState shadowSampler : register(s0, space2);
Texture2D<float4> baseColorTexture : register(t1, space2);
SamplerState materialSampler : register(s1, space2);
Texture2D<float4> normalTexture : register(t2, space2);
SamplerState normalSampler : register(s2, space2);
Texture2D<float4> metallicRoughnessTexture : register(t3, space2);
SamplerState metallicRoughnessSampler : register(s3, space2);
Texture2D<float4> occlusionTexture : register(t4, space2);
SamplerState occlusionSampler : register(s4, space2);
Texture2D<float4> emissiveTexture : register(t5, space2);
SamplerState emissiveSampler : register(s5, space2);
TextureCube<float4> irradianceMap : register(t6, space2);
SamplerState irradianceSampler : register(s6, space2);
TextureCube<float4> prefilteredSpecularMap : register(t7, space2);
SamplerState prefilteredSpecularSampler : register(s7, space2);
Texture2D<float2> brdfLut : register(t8, space2);
SamplerState brdfLutSampler : register(s8, space2);
Texture2DArray<float> localShadowMap : register(t9, space2);
SamplerState localShadowSampler : register(s9, space2);

struct LocalLightData
{
    float4 positionRange;
    float4 directionKind;
    float4 colorIntensity;
    float4 coneSource;
};
StructuredBuffer<LocalLightData> localLights : register(t10, space2);
StructuredBuffer<uint2> lightClusters : register(t11, space2);
StructuredBuffer<uint> lightClusterIndices : register(t12, space2);

cbuffer LightingData : register(b0, space3)
{
    float4 lightDirectionAndIntensity;
    float4 ambientAndBias;
    float4 lightColor;
    float4 cameraPosition;
    float4 cameraForward;
    float4 cascadeSplits;
    float4x4 cascadeViewProjections[4];
    float4 shadowParameters;
    float4 clusterDimensions;
    float4 clusterDepth;
    float4 renderDimensions;
    float4x4 localShadowViewProjections[8];
    float4 localShadowParameters;
};

struct FragmentOutput
{
    float4 color : SV_Target0;
    uint objectId : SV_Target1;
    float4 worldNormal : SV_Target2;
    float2 motion : SV_Target3;
    float reactive : SV_Target4;
    float4 indirectLighting : SV_Target5;
    float4 specularIndirect : SV_Target6;
    float4 reflectionProperties : SV_Target7;
};

static const float PI = 3.14159265359f;
static const float MAX_DIRECT_LIGHT = 65504.0f;

float distribution_ggx(float3 normal, float3 halfway, float roughness)
{
    const float safeRoughness = clamp(roughness, 0.045f, 1.0f);
    const float a = safeRoughness * safeRoughness;
    const float a2 = a * a;
    const float nDotH = saturate(dot(normal, halfway));
    const float denominator = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return min(a2 / max(PI * denominator * denominator, 0.00001f), MAX_DIRECT_LIGHT);
}

float geometry_schlick_ggx(float nDotDirection, float roughness)
{
    const float r = clamp(roughness, 0.045f, 1.0f) + 1.0f;
    const float k = (r * r) / 8.0f;
    const float safeNDotDirection = saturate(nDotDirection);
    return saturate(safeNDotDirection / max(safeNDotDirection * (1.0f - k) + k, 0.00001f));
}

float3 fresnel_schlick(float cosine, float3 f0)
{
    const float safeCosine = saturate(cosine);
    const float3 safeF0 = saturate(f0);
    return safeF0 + (1.0f - safeF0) * pow(1.0f - safeCosine, 5.0f);
}

float3 evaluate_direct_light(float3 normal, float3 toView, float3 toLight, float3 surfaceColor,
    float metallic, float roughness, float3 f0, float3 radiance)
{
    const float nDotL = saturate(dot(normal, toLight));
    const float nDotV = saturate(dot(normal, toView));
    if (nDotL <= 0.0f || nDotV <= 0.0f) return 0.0f;
    const float3 halfway = normalize(toView + toLight);
    const float3 fresnel = fresnel_schlick(max(dot(halfway, toView), 0.0f), f0);
    const float distribution = distribution_ggx(normal, halfway, roughness);
    const float geometry = geometry_schlick_ggx(nDotV, roughness) * geometry_schlick_ggx(nDotL, roughness);
    const float3 directLimit = float3(MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT);
    const float3 safeRadiance = min(max(radiance, float3(0.0f, 0.0f, 0.0f)), directLimit);
    const float3 safeSurfaceColor = min(max(surfaceColor, float3(0.0f, 0.0f, 0.0f)), directLimit);
    const float3 specular = min(distribution * geometry * fresnel /
        max(4.0f * nDotV * nDotL, 0.0001f), directLimit);
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - metallic);
    return min(max((diffuseWeight * safeSurfaceColor / PI + specular) * safeRadiance * nDotL,
        0.0f), directLimit);
}

float3 multi_scatter_specular(float3 f0, float2 environmentBrdf)
{
    // The BRDF LUT's x+y response at F0=1 is the single-scatter environment
    // energy. The remaining energy is returned through repeated GGX bounces;
    // the Schlick average keeps the compensation roughness/NoV aware through
    // the same split-sum LUT sample without adding another resource binding.
    const float singleScatterEnergy = saturate(environmentBrdf.x + environmentBrdf.y);
    const float missingEnergy = saturate(1.0f - singleScatterEnergy);
    const float3 averageFresnel = saturate(f0 + (1.0f - f0) / 21.0f);
    const float3 denominator = max(1.0f - averageFresnel * missingEnergy,
        float3(0.05f, 0.05f, 0.05f));
    return min(missingEnergy * averageFresnel / denominator,
        float3(8.0f, 8.0f, 8.0f));
}

float shadow_layer_visibility(uint cascadeIndex, float3 worldPosition, float3 normal)
{
    const float4 lightPosition = mul(cascadeViewProjections[cascadeIndex], float4(worldPosition, 1.0f));
    const float3 projected = lightPosition.xyz / lightPosition.w;
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    if (projected.z <= 0.0f || projected.z >= 1.0f || any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;

    const float3 lightDirection = normalize(-lightDirectionAndIntensity.xyz);
    const float bias = max(ambientAndBias.y * (1.0f - dot(normal, lightDirection)), ambientAndBias.z);
    const float2 texel = 1.0f / shadowParameters.xx;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            visibility += projected.z - bias <= shadowMap.Sample(shadowSampler, float3(uv + float2(x, y) * texel, cascadeIndex)) ? 1.0f : 0.0f;
    return visibility / 9.0f;
}

float shadow_visibility(float3 worldPosition, float3 normal)
{
    const float viewDepth = dot(worldPosition - cameraPosition.xyz, cameraForward.xyz);
    if (viewDepth <= 0.0f || viewDepth > cascadeSplits.w)
        return 1.0f;
    uint cascadeIndex = viewDepth <= cascadeSplits.x ? 0 : (viewDepth <= cascadeSplits.y ? 1 : (viewDepth <= cascadeSplits.z ? 2 : 3));
    const float visibility = shadow_layer_visibility(cascadeIndex, worldPosition, normal);
    if (cascadeIndex >= 3)
        return visibility;
    const float cascadeNear = cascadeIndex == 0 ? 0.0f : cascadeSplits[cascadeIndex - 1];
    const float blendWidth = max((cascadeSplits[cascadeIndex] - cascadeNear) * shadowParameters.z, 0.001f);
    const float blend = saturate((viewDepth - (cascadeSplits[cascadeIndex] - blendWidth)) / blendWidth);
    return lerp(visibility, shadow_layer_visibility(cascadeIndex + 1, worldPosition, normal), blend);
}

uint point_shadow_face(float3 direction)
{
    const float3 magnitude = abs(direction);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) return direction.x >= 0.0f ? 0 : 1;
    if (magnitude.y >= magnitude.z) return direction.y >= 0.0f ? 2 : 3;
    return direction.z >= 0.0f ? 4 : 5;
}

float local_shadow_visibility(LocalLightData light, float3 worldPosition, float3 normal, float3 lightToSurface)
{
    if (light.coneSource.w < -0.5f || localShadowParameters.y < 0.5f) return 1.0f;
    const uint baseLayer = (uint)(light.coneSource.w + 0.5f);
    const uint layer = baseLayer + (light.directionKind.w > 0.5f ? 0 : point_shadow_face(lightToSurface));
    if (layer >= (uint)localShadowParameters.y) return 1.0f;
    const float4 lightPosition = mul(localShadowViewProjections[layer], float4(worldPosition, 1.0f));
    const float3 projected = lightPosition.xyz / lightPosition.w;
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    if (projected.z <= 0.0f || projected.z >= 1.0f || any(uv < 0.0f) || any(uv > 1.0f)) return 1.0f;
    const float3 toLight = normalize(-lightToSurface);
    const float bias = max(localShadowParameters.z * (1.0f - dot(normal, toLight)), localShadowParameters.w);
    const float2 texel = 1.0f / localShadowParameters.xx;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            visibility += projected.z - bias <= localShadowMap.Sample(
                localShadowSampler, float3(uv + float2(x, y) * texel, layer)) ? 1.0f : 0.0f;
    return visibility / 9.0f;
}

FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    const float faceSign = input.frontFace ? 1.0f : -1.0f;
    const float3 geometricNormal = normalize(input.worldNormal) * faceSign;
    const float3 projectedTangent = input.worldTangent.xyz - geometricNormal * dot(input.worldTangent.xyz, geometricNormal);
    const float3 fallbackAxis = abs(geometricNormal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    const float3 tangent = dot(projectedTangent, projectedTangent) > 1.0e-6f
        ? normalize(projectedTangent)
        : normalize(cross(fallbackAxis, geometricNormal));
    const float3 bitangent = normalize(cross(geometricNormal, tangent)) * input.worldTangent.w;
    float3 tangentNormal = normalTexture.Sample(normalSampler, input.texcoord).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= input.emissiveNormal.w;
    const float3 normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y + geometricNormal * tangentNormal.z);
    output.objectId = input.instanceObjectId;
    output.motion = input.motion;
    const float4 surfaceColor = input.color * baseColorTexture.Sample(materialSampler, input.texcoord);
    if (input.occlusionAlphaFlags.z > 0.5f && input.occlusionAlphaFlags.z < 1.5f)
        clip(surfaceColor.a - input.occlusionAlphaFlags.y);
    const float4 metallicRoughness = metallicRoughnessTexture.Sample(metallicRoughnessSampler, input.texcoord);
    const float metallic = saturate(input.material.x * metallicRoughness.b);
    const float roughness = clamp(input.material.y * metallicRoughness.g, 0.045f, 1.0f);
    output.worldNormal = float4(normal, roughness);
    const float ambientOcclusion = lerp(1.0f, occlusionTexture.Sample(occlusionSampler, input.texcoord).r, saturate(input.occlusionAlphaFlags.x));
    const float3 emissive = emissiveTexture.Sample(emissiveSampler, input.texcoord).rgb * input.emissiveNormal.rgb;
    const float emissiveLuma = dot(emissive, float3(0.2126f, 0.7152f, 0.0722f));
    output.reactive = saturate((input.occlusionAlphaFlags.z > 1.5f ? 1.0f : 0.0f) + emissiveLuma * 0.25f);
    if (input.material.z > 0.5f)
    {
        output.color = float4(surfaceColor.rgb + emissive, surfaceColor.a);
        output.indirectLighting = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.specularIndirect = 0.0f.xxxx;
        output.reflectionProperties = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return output;
    }
    const float3 toView = normalize(cameraPosition.xyz - input.worldPosition);
    const float nDotV = max(dot(normal, toView), 0.0f);
    const float3 f0 = saturate(lerp(float3(0.04f, 0.04f, 0.04f), surfaceColor.rgb, metallic));
    const float visibility = input.material.w > 0.5f ? shadow_visibility(input.worldPosition, normal) : 1.0f;
    const float3 radiance = lightColor.rgb * lightDirectionAndIntensity.w;
    float3 direct = evaluate_direct_light(normal, toView, normalize(-lightDirectionAndIntensity.xyz),
        surfaceColor.rgb, metallic, roughness, f0, radiance) * visibility;
    if (clusterDimensions.w > 0.5f)
    {
        const uint3 dimensions = (uint3)clusterDimensions.xyz;
        const uint2 tile = min((uint2)(input.position.xy / renderDimensions.xy * dimensions.xy), dimensions.xy - 1);
        const float viewDepth = clamp(dot(input.worldPosition - cameraPosition.xyz, cameraForward.xyz), clusterDepth.x, clusterDepth.y);
        const float normalizedDepth = saturate(log(viewDepth / clusterDepth.x) * clusterDepth.z);
        const uint slice = min((uint)(normalizedDepth * dimensions.z), dimensions.z - 1);
        const uint clusterIndex = tile.x + tile.y * dimensions.x + slice * dimensions.x * dimensions.y;
        const uint2 cluster = lightClusters[clusterIndex];
        [loop]
        for (uint localIndex = 0; localIndex < cluster.y; ++localIndex)
        {
            const uint lightIndex = lightClusterIndices[cluster.x + localIndex];
            if (lightIndex >= (uint)clusterDepth.w) continue;
            const LocalLightData light = localLights[lightIndex];
            const float3 lightToSurface = input.worldPosition - light.positionRange.xyz;
            const float distanceSquared = dot(lightToSurface, lightToSurface);
            const float distanceToLight = sqrt(max(distanceSquared, 0.000001f));
            if (distanceToLight >= light.positionRange.w) continue;
            const float3 lightDirectionToSurface = lightToSurface / distanceToLight;
            float cone = 1.0f;
            if (light.directionKind.w > 0.5f)
                cone = smoothstep(light.coneSource.y, light.coneSource.x,
                    dot(normalize(light.directionKind.xyz), lightDirectionToSurface));
            const float rangeRatio = distanceToLight / light.positionRange.w;
            const float rangeWindow = saturate(1.0f - rangeRatio * rangeRatio * rangeRatio * rangeRatio);
            const float attenuation = rangeWindow * rangeWindow /
                max(distanceSquared + light.coneSource.z * light.coneSource.z, 0.01f);
            const float localVisibility = input.material.w > 0.5f
                ? local_shadow_visibility(light, input.worldPosition, normal, lightToSurface) : 1.0f;
            const float3 localRadiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation * cone * localVisibility;
            direct += evaluate_direct_light(normal, toView, -lightDirectionToSurface, surfaceColor.rgb,
                metallic, roughness, f0, localRadiance);
        }
    }
    const float3 reflectionDirection = reflect(-toView, normal);
    const float3 fresnelRoughness = f0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), f0) - f0)
        * pow(1.0f - nDotV, 5.0f);
    const float3 diffuseWeightIbl = (1.0f - fresnelRoughness) * (1.0f - metallic);
    const float3 irradiance = irradianceMap.Sample(irradianceSampler, normal).rgb;
    const float3 diffuseIbl = irradiance * surfaceColor.rgb * diffuseWeightIbl * ambientAndBias.x * 4.0f * ambientOcclusion;
    const float3 prefilteredSpecular = prefilteredSpecularMap.SampleLevel(
        prefilteredSpecularSampler, reflectionDirection, roughness * 6.0f).rgb;
    const float2 environmentBrdf = brdfLut.Sample(brdfLutSampler, float2(nDotV, roughness)).rg;
    const float3 singleScatterSpecular = max(fresnelRoughness * environmentBrdf.x + environmentBrdf.y,
        float3(0.0f, 0.0f, 0.0f));
    const float3 multiScatterSpecular = multi_scatter_specular(f0, environmentBrdf);
    const float3 specularIbl = prefilteredSpecular
        * (singleScatterSpecular + multiScatterSpecular)
        * ambientAndBias.x * 4.0f * ambientOcclusion;
    const float3 indirectIbl = diffuseIbl + specularIbl;
    output.indirectLighting = float4(indirectIbl, 1.0f);
    output.specularIndirect = float4(specularIbl, 1.0f);
    output.reflectionProperties = float4(f0, roughness);
    output.color = float4(indirectIbl + direct + emissive, surfaceColor.a);
    return output;
}
