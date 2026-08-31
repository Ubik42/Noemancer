// noemancer.native-rt-full-frame/0.2
// Runtime-private DXR library. The Engine/Agent boundary observes only the
// versioned output metadata and never depends on these native bindings.
RaytracingAccelerationStructure sceneAccelerationStructure : register(t0);
RWStructuredBuffer<uint4> outputPixels : register(u0);

// ABI noemancer.native-d3d12-raytracing-camera/0.1.  The CPU uploads five
// float4 values to a 256-byte CBV at b0.  Position and right/up/forward are
// world-space values; the basis is right-handed and already orthonormal, so
// no row/column-major matrix convention or transpose is involved.  This
// shader writes marker colours proving visibility only; it is not radiance,
// RTGI or a material-lighting implementation.
cbuffer CameraConstants : register(b0)
{
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 cameraLens; // tan-half vertical FOV, aspect, near, far
};

struct RayPayload
{
    uint hit;
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
    TraceRay(sceneAccelerationStructure, RAY_FLAG_NONE, 0xFFU, 0U, 1U, 0U, ray, payload);

    const uint linearIndex = pixel.y * dimensions.x + pixel.x;
    const uint3 missColour = uint3(20U, 25U, 29U);
    const uint3 hitColour = uint3(190U, 119U, 74U);
    outputPixels[linearIndex] = uint4(payload.hit != 0U ? hitColour : missColour, 255U);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.hit = 0U;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1U;
}
