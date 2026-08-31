// noemancer.native-rt-full-frame/0.3
// Runtime-private DXR library. The Engine/Agent boundary observes only the
// versioned output metadata and never depends on these native bindings.
RaytracingAccelerationStructure sceneAccelerationStructure : register(t0);

struct MaterialData
{
    float4 baseColor;
    float4 materialParameters; // metallic, roughness, emissive intensity, reserved
    float4 emissive;
};

struct InstanceShadingData
{
    uint materialIndex;
    uint normalOffset;
    uint normalCount;
    uint reserved;
};

RWStructuredBuffer<float4> outputPixels : register(u0);
StructuredBuffer<MaterialData> materials : register(t1);
StructuredBuffer<InstanceShadingData> instanceShading : register(t2);
StructuredBuffer<float4> triangleNormals : register(t3);

// ABI noemancer.native-d3d12-raytracing-camera/0.1.  The CPU uploads five
// float4 values to a 256-byte CBV at b0.  Position and right/up/forward are
// world-space values; the basis is right-handed and already orthonormal, so
// no row/column-major matrix convention or transpose is involved.  This
// version adds scene-linear direct + ambient shading using the material,
// normal and lighting tables below.  It is deliberately still a bounded
// lighting diagnostic: it has no shadow rays, indirect bounce, denoising or
// temporal history and therefore does not claim RTGI.
cbuffer CameraConstants : register(b0)
{
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 cameraLens; // tan-half vertical FOV, aspect, near, far
};

cbuffer LightingConstants : register(b1)
{
    float4 directionalDirection;
    float4 directionalColorIntensity; // RGB color, intensity
    float4 ambientColorIntensity; // RGB color, intensity
};

struct RayPayload
{
    uint hit;
    float3 radiance;
};

[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    const float2 uv = ((float2(pixel) + 0.5F) / float2(dimensions)) * 2.0F - 1.0F;

    RayDesc ray;
    ray.Origin = cameraPosition.xyz;
    ray.Direction = normalize(
        cameraForward.xyz
        + cameraRight.xyz * (uv.x * cameraLens.y * cameraLens.x)
        - cameraUp.xyz * (uv.y * cameraLens.x));
    ray.TMin = cameraLens.z;
    ray.TMax = cameraLens.w;

    RayPayload payload;
    payload.hit = 0U;
    payload.radiance = 0.0F.xxx;
    TraceRay(sceneAccelerationStructure, RAY_FLAG_NONE, 0xFFU, 0U, 1U, 0U, ray, payload);

    const uint linearIndex = pixel.y * dimensions.x + pixel.x;
    const float3 missRadiance = ambientColorIntensity.rgb * ambientColorIntensity.a;
    outputPixels[linearIndex] = float4(
        payload.hit != 0U ? payload.radiance : missRadiance,
        payload.hit != 0U ? 1.0F : 0.0F);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.hit = 0U;
    payload.radiance = 0.0F.xxx;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceIndex = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const InstanceShadingData instance = instanceShading[instanceIndex];
    const MaterialData material = materials[instance.materialIndex];

    float3 normal = float3(0.0F, 1.0F, 0.0F);
    if (primitiveIndex < instance.normalCount)
    {
        normal = normalize(triangleNormals[instance.normalOffset + primitiveIndex].xyz);
    }
    const float3 viewDirection = normalize(-WorldRayDirection());
    if (dot(normal, viewDirection) < 0.0F)
    {
        normal = -normal;
    }
    const float3 lightDirection = normalize(directionalDirection.xyz);
    const float diffuseFactor = saturate(dot(normal, lightDirection));
    const float roughness = clamp(material.materialParameters.y, 0.02F, 1.0F);
    const float metallic = saturate(material.materialParameters.x);
    const float3 baseColor = max(material.baseColor.rgb, 0.0F.xxx);
    const float3 f0 = lerp(0.04F.xxx, baseColor, metallic);
    const float3 halfDirection = normalize(lightDirection + viewDirection);
    const float specularFactor = pow(
        saturate(dot(normal, halfDirection)), lerp(128.0F, 4.0F, roughness));
    const float3 direct =
        (baseColor * (1.0F - metallic) * diffuseFactor + f0 * specularFactor)
        * directionalColorIntensity.rgb * directionalColorIntensity.a;
    const float3 ambient = baseColor * (1.0F - metallic)
        * ambientColorIntensity.rgb * ambientColorIntensity.a;
    const float3 emission = max(material.emissive.rgb, 0.0F.xxx)
        * max(material.materialParameters.z, 0.0F);
    payload.hit = 1U;
    payload.radiance = max(direct + ambient + emission, 0.0F.xxx);
}
