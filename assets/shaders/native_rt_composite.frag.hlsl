// noemancer.native-rt-composite/0.2
//
// This is a diagnostic composite, not RTGI.  The SDL export surface remains
// R32G32B32A32_UINT for the same-device copy ABI.  Producer contract 0.1
// stores bounded marker bytes, while native full-frame contract 0.3 stores
// IEEE-754 scene-linear float bits in the same four uint lanes.  debugMode
// selects the versioned decode; mode 1 uses asfloat and never divides the
// float bit pattern by 255.
//
// The graph schedules this pass after the regular tone-map pass and before
// presentation.  The radiance diagnostic therefore applies a bounded ACES
// display mapping and explicit sRGB transfer locally.  It replaces the final
// presentation image for inspection; it does not feed radiance back into the
// scene lighting graph and makes no indirect-lighting claim.
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
    // 0 = native-rt-marker-probe/0.1, 1 = native-rt-full-frame/0.3
    uint debugMode;
    uint reserved;
    float4 clearColorLinear;
};

static const float MAX_SCENE_LINEAR = 65504.0f;
static const float3x3 ACES_INPUT_MATRIX = float3x3(
    0.59719f, 0.35458f, 0.04823f,
    0.07600f, 0.90834f, 0.01566f,
    0.02840f, 0.13383f, 0.83777f);
static const float3x3 ACES_OUTPUT_MATRIX = float3x3(
    1.60475f, -0.53108f, -0.07367f,
   -0.10208f,  1.10813f, -0.00605f,
   -0.00327f, -0.07276f,  1.07602f);

float finite_or_zero(float value)
{
    return isfinite(value) ? value : 0.0f;
}

float3 sanitize_scene_linear(float3 value)
{
    const float3 finiteValue = float3(
        finite_or_zero(value.r), finite_or_zero(value.g), finite_or_zero(value.b));
    return min(max(finiteValue, float3(0.0f, 0.0f, 0.0f)),
        float3(MAX_SCENE_LINEAR, MAX_SCENE_LINEAR, MAX_SCENE_LINEAR));
}

float3 aces_fitted(float3 color)
{
    const float3 acesColor = mul(ACES_INPUT_MATRIX, sanitize_scene_linear(color));
    const float3 numerator = acesColor * (acesColor + 0.0245786f) - 0.000090537f;
    const float3 denominator = acesColor * (0.983729f * acesColor + 0.4329510f) + 0.238081f;
    const float3 fitted = numerator / max(denominator,
        float3(0.0001f, 0.0001f, 0.0001f));
    const float3 rec709 = mul(ACES_OUTPUT_MATRIX, fitted);
    return saturate(max(rec709,
        float3(0.0f, 0.0f, 0.0f)));
}

float linear_to_srgb(float value)
{
    const float safeValue = saturate(finite_or_zero(value));
    return safeValue <= 0.0031308f
        ? safeValue * 12.92f
        : 1.055f * pow(safeValue, 1.0f / 2.4f) - 0.055f;
}

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

    if (debugMode == 0U)
    {
        // The producer's marker bytes are already a linear debug encoding.
        // Do not gamma-decode them; they are not authored sRGB color data.
        const float3 linearColor = saturate(float3(encoded.rgb) / 255.0f);
        const float linearAlpha = saturate(float(encoded.a) / 255.0f);
        return float4(linearColor, linearAlpha);
    }

    if (debugMode == 1U)
    {
        // The native full-frame producer copied float bits through the UINT
        // footprint.  asfloat is the only correct decode; treating these
        // values as 0..255 marker bytes would turn common 1.0 bits (0x3f800000)
        // into a saturated display value.
        const float4 encodedRadiance = asfloat(encoded);
        const float3 mapped = aces_fitted(encodedRadiance.rgb);
        const float alpha = saturate(finite_or_zero(encodedRadiance.a));
        return float4(linear_to_srgb(mapped.r), linear_to_srgb(mapped.g),
            linear_to_srgb(mapped.b), alpha);
    }

    // Modes other than the two explicit producer contracts fail closed.
    // Returning an explicit clear color keeps future routes visible without
    // claiming that this diagnostic shader is RTGI.
    return float4(saturate(clearColorLinear.rgb), saturate(clearColorLinear.a));
}
