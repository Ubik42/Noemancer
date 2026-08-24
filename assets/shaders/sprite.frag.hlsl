struct FragmentInput
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 worldTangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
    float2 motion : TEXCOORD4;
    nointerpolation uint objectId : TEXCOORD5;
    nointerpolation uint alphaMode : TEXCOORD6;
    nointerpolation float4 materialParameters : TEXCOORD7;
    nointerpolation float4 emissiveColor : TEXCOORD8;
    nointerpolation float4 surfaceParameters : TEXCOORD9;
};

// t0..t3 are the existing sprite material ABI. Shadow resources are appended
// so existing material texture ordering remains stable for the runtime binder.
Texture2D<float4> spriteTexture : register(t0, space2);
SamplerState spriteSampler : register(s0, space2);
Texture2D<float4> normalTexture : register(t1, space2);
SamplerState normalSampler : register(s1, space2);
Texture2D<float4> emissiveMaskTexture : register(t2, space2);
SamplerState emissiveMaskSampler : register(s2, space2);
Texture2D<float4> depthTexture : register(t3, space2);
SamplerState depthSampler : register(s3, space2);
Texture2DArray<float> shadowMap : register(t4, space2);
SamplerState shadowSampler : register(s4, space2);
Texture2DArray<float> localShadowMap : register(t5, space2);
SamplerState localShadowSampler : register(s5, space2);

struct LocalLightData
{
    float4 positionRange;
    float4 directionKind;
    float4 colorIntensity;
    float4 coneSource;
};

// SDL_GPU's DXIL root signature places fragment storage buffers directly
// after this shader's six sampled textures (t6..t8).
StructuredBuffer<LocalLightData> localLights : register(t6, space2);
StructuredBuffer<uint2> lightClusters : register(t7, space2);
StructuredBuffer<uint> lightClusterIndices : register(t8, space2);

// Keep this cbuffer byte-for-byte compatible with scene_lit.frag.hlsl. The
// renderer uploads one LightingData instance to both fragment pipelines.
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
    float depth : SV_Depth;
};

static const float PI = 3.14159265359f;
static const float MAX_DIRECT_LIGHT = 65504.0f;

float3 bounded_color(float3 value)
{
    const float3 finite = select(isfinite(value), value, float3(0.0f, 0.0f, 0.0f));
    return min(max(finite, float3(0.0f, 0.0f, 0.0f)),
        float3(MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT));
}

float3 safe_normalize(float3 value, float3 fallback)
{
    const float3 finite = select(isfinite(value), value, fallback);
    const float3 bounded = min(max(finite, float3(-MAX_DIRECT_LIGHT, -MAX_DIRECT_LIGHT, -MAX_DIRECT_LIGHT)),
        float3(MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT));
    const float lengthSquared = dot(bounded, bounded);
    return lengthSquared > 1.0e-8f ? bounded * rsqrt(lengthSquared) : fallback;
}

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
    const float3 halfway = safe_normalize(toView + toLight, normal);
    const float3 fresnel = fresnel_schlick(max(dot(halfway, toView), 0.0f), f0);
    const float distribution = distribution_ggx(normal, halfway, roughness);
    const float geometry = geometry_schlick_ggx(nDotV, roughness) * geometry_schlick_ggx(nDotL, roughness);
    const float3 directLimit = float3(MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT, MAX_DIRECT_LIGHT);
    const float3 safeRadiance = min(max(radiance, float3(0.0f, 0.0f, 0.0f)), directLimit);
    const float3 safeSurfaceColor = min(max(surfaceColor, float3(0.0f, 0.0f, 0.0f)), directLimit);
    const float3 specular = min(distribution * geometry * fresnel /
        max(4.0f * nDotV * nDotL, 0.0001f), directLimit);
    const float3 diffuseWeight = (1.0f - fresnel) * (1.0f - saturate(metallic));
    return min(max((diffuseWeight * safeSurfaceColor / PI + specular) * safeRadiance * nDotL,
        0.0f), directLimit);
}

float shadow_layer_visibility(uint cascadeIndex, float3 worldPosition, float3 normal)
{
    const float4 lightPosition = mul(cascadeViewProjections[cascadeIndex], float4(worldPosition, 1.0f));
    if (!isfinite(lightPosition.w) || any(!isfinite(lightPosition.xyz)) || abs(lightPosition.w) <= 0.00001f)
        return 1.0f;
    const float3 projected = lightPosition.xyz / lightPosition.w;
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    if (any(!isfinite(projected)) || any(!isfinite(uv)) || projected.z <= 0.0f || projected.z >= 1.0f ||
        any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;

    const float3 lightDirection = safe_normalize(-lightDirectionAndIntensity.xyz, float3(0.0f, 0.0f, 1.0f));
    const float bias = max(ambientAndBias.y * (1.0f - dot(normal, lightDirection)), ambientAndBias.z);
    const float2 texel = 1.0f / max(shadowParameters.xx, float2(1.0f, 1.0f));
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
            visibility += projected.z - bias <= shadowMap.Sample(shadowSampler,
                float3(uv + float2(x, y) * texel, cascadeIndex)) ? 1.0f : 0.0f;
    return visibility / 9.0f;
}

float shadow_visibility(float3 worldPosition, float3 normal)
{
    const float viewDepth = dot(worldPosition - cameraPosition.xyz, cameraForward.xyz);
    const float maximumDistance = max(cascadeSplits.w, 0.0f);
    if (!isfinite(viewDepth) || viewDepth <= 0.0f || viewDepth > maximumDistance) return 1.0f;
    const uint cascadeIndex = viewDepth <= cascadeSplits.x ? 0 :
        (viewDepth <= cascadeSplits.y ? 1 : (viewDepth <= cascadeSplits.z ? 2 : 3));
    const float visibility = shadow_layer_visibility(cascadeIndex, worldPosition, normal);
    if (cascadeIndex >= 3) return visibility;
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
    if (!isfinite(light.coneSource.w) || light.coneSource.w < -0.5f || localShadowParameters.y < 0.5f) return 1.0f;
    const uint baseLayer = (uint)max(light.coneSource.w, 0.0f);
    const uint layer = baseLayer + (light.directionKind.w > 0.5f ? 0 : point_shadow_face(lightToSurface));
    if (layer >= (uint)max(localShadowParameters.y, 0.0f) || layer >= 8U) return 1.0f;
    const float4 lightPosition = mul(localShadowViewProjections[layer], float4(worldPosition, 1.0f));
    if (!isfinite(lightPosition.w) || any(!isfinite(lightPosition.xyz)) || abs(lightPosition.w) <= 0.00001f)
        return 1.0f;
    const float3 projected = lightPosition.xyz / lightPosition.w;
    const float2 uv = float2(projected.x * 0.5f + 0.5f, -projected.y * 0.5f + 0.5f);
    if (any(!isfinite(projected)) || any(!isfinite(uv)) || projected.z <= 0.0f || projected.z >= 1.0f ||
        any(uv < 0.0f) || any(uv > 1.0f)) return 1.0f;
    const float3 toLight = safe_normalize(-lightToSurface, float3(0.0f, 0.0f, 1.0f));
    const float bias = max(localShadowParameters.z * (1.0f - dot(normal, toLight)), localShadowParameters.w);
    const float2 texel = 1.0f / max(localShadowParameters.xx, float2(1.0f, 1.0f));
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
    const float4 sampled = spriteTexture.Sample(spriteSampler, input.texcoord);
    if (input.alphaMode == 1) clip(sampled.a - 0.5f);

    float3 geometricNormal = safe_normalize(input.worldNormal,float3(0.0f,0.0f,1.0f));
    if(dot(geometricNormal,cameraPosition.xyz-input.worldPosition)<0.0f)geometricNormal=-geometricNormal;
    const float3 projectedTangent = input.worldTangent.xyz - geometricNormal * dot(input.worldTangent.xyz, geometricNormal);
    const float3 tangentFallback = abs(geometricNormal.z) < 0.999f
        ? safe_normalize(cross(float3(0.0f, 0.0f, 1.0f), geometricNormal), float3(1.0f, 0.0f, 0.0f))
        : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = safe_normalize(projectedTangent, tangentFallback);
    const float3 bitangent = safe_normalize(cross(geometricNormal, tangent) * input.worldTangent.w,
        float3(0.0f, 1.0f, 0.0f));
    float3 tangentNormal = normalTexture.Sample(normalSampler, input.texcoord).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= clamp(input.materialParameters.x, 0.0f, 4.0f);
    tangentNormal = safe_normalize(tangentNormal, float3(0.0f, 0.0f, 1.0f));
    const float3 normal = safe_normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
        geometricNormal * tangentNormal.z, geometricNormal);

    const float4 safeSampled = float4(bounded_color(sampled.rgb), saturate(sampled.a));
    const float emissiveMask = saturate(emissiveMaskTexture.Sample(emissiveMaskSampler, input.texcoord).r);
    const float3 emissive = bounded_color(input.emissiveColor.rgb *
        clamp(input.materialParameters.y, 0.0f, 100.0f) * emissiveMask);
    const float height = saturate(depthTexture.Sample(depthSampler, input.texcoord).r);

    FragmentOutput output;
    output.objectId = input.objectId;
    output.worldNormal = float4(normal, 1.0f);
    output.motion = input.motion;
    output.reactive = max(input.alphaMode == 2 ? 1.0f : 0.0f,
        saturate(max(emissive.r, max(emissive.g, emissive.b))));
    output.depth = saturate(input.position.z - height * clamp(input.materialParameters.z, 0.0f, 0.01f) *
        saturate(input.materialParameters.w));

    if (input.surfaceParameters.z <= 0.5f)
    {
        output.color = float4(bounded_color(safeSampled.rgb + emissive), safeSampled.a);
        output.indirectLighting = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return output;
    }

    const float metallic = saturate(input.surfaceParameters.x);
    const float roughness = clamp(input.surfaceParameters.y, 0.045f, 1.0f);
    const float3 surfaceColor = safeSampled.rgb;
    const float3 toView = safe_normalize(cameraPosition.xyz - input.worldPosition,
        geometricNormal);
    const float nDotV = saturate(dot(normal, toView));
    const float3 f0 = saturate(lerp(float3(0.04f, 0.04f, 0.04f), surfaceColor, metallic));
    const float receivesShadows = saturate(input.surfaceParameters.w);

    const float directionalVisibility = receivesShadows > 0.5f
        ? shadow_visibility(input.worldPosition, normal) : 1.0f;
    const float3 directionalRadiance = bounded_color(lightColor.rgb *
        max(lightDirectionAndIntensity.w, 0.0f) * directionalVisibility);
    float3 direct = evaluate_direct_light(normal, toView,
        safe_normalize(-lightDirectionAndIntensity.xyz, float3(0.0f, 0.0f, 1.0f)),
        surfaceColor, metallic, roughness, f0, directionalRadiance);

    const float3 finiteClusterDimensions = select(isfinite(clusterDimensions.xyz), clusterDimensions.xyz,
        float3(1.0f, 1.0f, 1.0f));
    const float3 clusterDimensionSource = clamp(finiteClusterDimensions,
        float3(1.0f, 1.0f, 1.0f), float3(1024.0f, 1024.0f, 256.0f));
    const uint3 dimensions = (uint3)clusterDimensionSource;
    if (clusterDimensions.w > 0.5f && clusterDepth.w > 0.0f &&
        renderDimensions.x > 0.0f && renderDimensions.y > 0.0f)
    {
        const float2 finiteRenderDimensions = select(isfinite(renderDimensions.xy), renderDimensions.xy,
            float2(1.0f, 1.0f));
        const float2 renderSize = max(finiteRenderDimensions, float2(1.0f, 1.0f));
        const float2 normalizedPixel = saturate(input.position.xy / renderSize);
        const uint2 tile = min((uint2)(normalizedPixel * dimensions.xy), dimensions.xy - 1U);
        const float nearDepth = max(isfinite(clusterDepth.x) ? clusterDepth.x : 0.0001f, 0.0001f);
        const float farDepth = max(isfinite(clusterDepth.y) ? clusterDepth.y : nearDepth, nearDepth);
        const float viewDepth = clamp(dot(input.worldPosition - cameraPosition.xyz, cameraForward.xyz),
            nearDepth, farDepth);
        const float normalizedDepth = saturate(log(max(viewDepth / nearDepth, 1.0f)) * max(clusterDepth.z, 0.0f));
        const uint slice = min((uint)(normalizedDepth * dimensions.z), dimensions.z - 1U);
        const uint clusterIndex = tile.x + tile.y * dimensions.x + slice * dimensions.x * dimensions.y;
        const uint2 cluster = lightClusters[clusterIndex];
        const uint clusterCount = min(cluster.y, 1024U);
        [loop]
        for (uint localIndex = 0; localIndex < clusterCount; ++localIndex)
        {
            if (cluster.x > 0xffffffffU - localIndex) break;
            const uint lightIndexOffset = cluster.x + localIndex;
            const uint lightIndex = lightClusterIndices[lightIndexOffset];
            if (lightIndex >= (uint)max(clusterDepth.w, 0.0f)) continue;
            const LocalLightData light = localLights[lightIndex];
            const float3 lightToSurface = input.worldPosition - light.positionRange.xyz;
            const float distanceSquared = dot(lightToSurface, lightToSurface);
            const float distanceToLight = sqrt(max(distanceSquared, 0.000001f));
            const float range = max(isfinite(light.positionRange.w) ? light.positionRange.w : 0.0001f, 0.0001f);
            if (!isfinite(distanceSquared) || !isfinite(distanceToLight) || distanceToLight >= range) continue;
            const float3 lightDirectionToSurface = safe_normalize(lightToSurface,
                float3(0.0f, 0.0f, 1.0f));
            float cone = 1.0f;
            if (light.directionKind.w > 0.5f)
            {
                const float3 spotDirection = safe_normalize(light.directionKind.xyz,
                    float3(0.0f, 0.0f, -1.0f));
                const float innerCone = clamp(light.coneSource.x, -1.0f, 1.0f);
                const float outerCone = clamp(light.coneSource.y, -1.0f, innerCone);
                cone = smoothstep(outerCone, innerCone, dot(spotDirection, lightDirectionToSurface));
            }
            const float rangeRatio = saturate(distanceToLight / range);
            const float rangeWindow = saturate(1.0f - rangeRatio * rangeRatio * rangeRatio * rangeRatio);
            const float sourceRadius = max(light.coneSource.z, 0.0f);
            const float attenuation = rangeWindow * rangeWindow /
                max(distanceSquared + sourceRadius * sourceRadius, 0.01f);
            const float localVisibility = receivesShadows > 0.5f
                ? local_shadow_visibility(light, input.worldPosition, normal, lightToSurface) : 1.0f;
            const float3 localRadiance = bounded_color(light.colorIntensity.rgb *
                max(light.colorIntensity.w, 0.0f) * attenuation * cone * localVisibility);
            direct += evaluate_direct_light(normal, toView, -lightDirectionToSurface,
                surfaceColor, metallic, roughness, f0, localRadiance);
            direct = bounded_color(direct);
        }
    }

    // Sprite materials do not bind the scene IBL textures. Use the shared
    // ambient authority for a bounded diffuse indirect term and keep it in
    // target five so the AO composite only modulates ambient/indirect light.
    const float3 ambientFresnel = fresnel_schlick(nDotV, f0);
    const float3 ambientDiffuseWeight = (1.0f - ambientFresnel) * (1.0f - metallic);
    const float3 indirect = bounded_color(surfaceColor * ambientDiffuseWeight *
        max(ambientAndBias.x, 0.0f) * 4.0f);
    output.indirectLighting = float4(indirect, 1.0f);
    output.color = float4(bounded_color(indirect + direct + emissive), safeSampled.a);
    return output;
}
