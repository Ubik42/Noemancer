// noemancer.native-rt-composite/0.1
//
// This is a debug composite, not RTGI.  The current native DXR probe writes
// an R32G32B32A32_UINT marker surface: RGB are bounded 0..255 debug values
// and A is a bounded alpha marker.  The shader performs an integer Load and
// normalizes those values into the bounded RGBA8_UNORM diagnostic presentation
// target. It intentionally does not apply an sRGB transfer or infer
// indirect-lighting radiance from the marker colors.
//
// SDL_GPU graphics ABI:
//   t0/s0, space2 = same-device R32G32B32A32_UINT Texture2D view + ABI sampler
//   b0, space3 = NativeRtCompositeSettings (32 bytes)
// No sampler is used: integer Texture2D.Load is required for exact marker
// values and avoids filtering/format reinterpretation.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<uint4> nativeRtOutput : register(t0, space2);
// SDL_GPU exposes sampled textures as texture/sampler pairs. Integer Load does
// not consume filtering state, but retaining the paired sampler keeps the
// graphics descriptor ABI portable across DXIL and SPIR-V backends.
SamplerState nativeRtAbiSampler : register(s0, space2);

cbuffer NativeRtCompositeSettings : register(b0, space3)
{
    uint2 outputExtent;
    uint debugMode;
    uint reserved;
    float4 clearColorLinear;
};

float4 main(FragmentInput input) : SV_Target0
{
    uint textureWidth;
    uint textureHeight;
    nativeRtOutput.GetDimensions(textureWidth, textureHeight);
    const uint2 declaredExtent = uint2(max(outputExtent.x, 1U), max(outputExtent.y, 1U));
    const uint2 dimensions = min(uint2(max(textureWidth, 1U), max(textureHeight, 1U)), declaredExtent);
    const uint2 lastPixel = dimensions - 1U;
    const uint2 pixel = min(uint2(max(input.position.x, 0.0f), max(input.position.y, 0.0f)), lastPixel);
    const uint4 encoded = nativeRtOutput.Load(int3(pixel, 0));

    // The producer's marker bytes are already a linear debug encoding.  Do
    // not gamma-decode them; they are not authored sRGB color data.
    const float3 linearColor = saturate(float3(encoded.rgb) / 255.0f);
    const float linearAlpha = saturate(float(encoded.a) / 255.0f);

    // Modes other than zero are reserved for a future versioned radiance
    // contract.  Returning an explicit clear color keeps that future route
    // visible without claiming that this marker shader is RTGI.
    if (debugMode != 0U)
        return float4(saturate(clearColorLinear.rgb), saturate(clearColorLinear.a));
    return float4(linearColor, linearAlpha);
}
