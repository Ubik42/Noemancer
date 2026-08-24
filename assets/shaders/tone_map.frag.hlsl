struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> hdrScene : register(t0, space2);
Texture2D<float4> bloomScene : register(t1, space2);
Texture2D<float> exposureHistory : register(t2, space2);
SamplerState hdrSampler : register(s0, space2);
SamplerState bloomSampler : register(s1, space2);
SamplerState exposureSampler : register(s2, space2);

static const float MAX_SCENE_LINEAR = 65504.0f;
static const float3x3 ACES_INPUT_MATRIX = float3x3(
    0.59719f, 0.35458f, 0.04823f,
    0.07600f, 0.90834f, 0.01566f,
    0.02840f, 0.13383f, 0.83777f);
static const float3x3 ACES_OUTPUT_MATRIX = float3x3(
    1.60475f, -0.53108f, -0.07367f,
   -0.10208f,  1.10813f, -0.00605f,
   -0.00327f, -0.07276f,  1.07602f);

cbuffer ToneMapSettings : register(b0, space3)
{
    float exposureCompensation;
    float whitePoint;
    float bloomStrength;
    float debugBypass;
    float4 lift;
    float4 gamma;
    float4 gain;
    float saturation;
    float contrast;
    float temperature;
    float tint;
};

float finite_or_zero(float value)
{
    return isfinite(value) ? value : 0.0f;
}

float3 finite_or_zero(float3 value)
{
    return float3(finite_or_zero(value.r), finite_or_zero(value.g), finite_or_zero(value.b));
}

float3 sanitize_scene_linear(float3 value)
{
    const float3 finiteValue = finite_or_zero(value);
    return min(max(finiteValue, float3(0.0f, 0.0f, 0.0f)),
        float3(MAX_SCENE_LINEAR, MAX_SCENE_LINEAR, MAX_SCENE_LINEAR));
}

float3 aces_fitted(float3 color)
{
    // Scene-linear Rec.709 -> ACES working space -> fitted RRT/ODT -> Rec.709.
    const float3 acesColor = mul(ACES_INPUT_MATRIX, sanitize_scene_linear(color));
    const float3 numerator = acesColor * (acesColor + 0.0245786f) - 0.000090537f;
    const float3 denominator = acesColor * (0.983729f * acesColor + 0.4329510f) + 0.238081f;
    const float3 fitted = finite_or_zero(numerator / max(denominator,
        float3(0.0001f, 0.0001f, 0.0001f)));
    const float3 rec709 = finite_or_zero(mul(ACES_OUTPUT_MATRIX, fitted));
    return saturate(max(rec709, float3(0.0f, 0.0f, 0.0f)));
}

float linear_to_srgb(float value)
{
    const float safeValue = saturate(finite_or_zero(value));
    return safeValue <= 0.0031308f ? safeValue * 12.92f : 1.055f * pow(safeValue, 1.0f / 2.4f) - 0.055f;
}

float3 color_grade(float3 color)
{
    const float safeTemperature = clamp(finite_or_zero(temperature), -16.0f, 16.0f);
    const float safeTint = clamp(finite_or_zero(tint), -16.0f, 16.0f);
    const float3 safeLift = clamp(finite_or_zero(lift.rgb), float3(-16.0f, -16.0f, -16.0f),
        float3(16.0f, 16.0f, 16.0f));
    const float3 safeGamma = clamp(finite_or_zero(gamma.rgb), float3(0.01f, 0.01f, 0.01f),
        float3(16.0f, 16.0f, 16.0f));
    const float3 safeGain = clamp(finite_or_zero(gain.rgb), float3(-16.0f, -16.0f, -16.0f),
        float3(16.0f, 16.0f, 16.0f));
    const float safeSaturation = clamp(finite_or_zero(saturation), -16.0f, 16.0f);
    const float safeContrast = clamp(finite_or_zero(contrast), -16.0f, 16.0f);
    const float3 balance = float3(safeTemperature - safeTint * 0.5f, safeTint,
        -safeTemperature - safeTint * 0.5f);
    color = max(finite_or_zero(color) + safeLift + balance * 0.05f,
        float3(0.0f, 0.0f, 0.0f));
    const float3 safeExponent = clamp(1.0f / safeGamma, float3(0.0625f, 0.0625f, 0.0625f),
        float3(4.0f, 4.0f, 4.0f));
    const float3 powLimit = pow(float3(MAX_SCENE_LINEAR, MAX_SCENE_LINEAR, MAX_SCENE_LINEAR),
        min(1.0f / safeExponent, float3(1.0f, 1.0f, 1.0f)));
    color = pow(min(color, powLimit), safeExponent) * safeGain;
    const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    color = lerp(luminance.xxx, color, safeSaturation);
    return sanitize_scene_linear(max((finite_or_zero(color) - 0.18f) * safeContrast + 0.18f,
        float3(0.0f, 0.0f, 0.0f)));
}

float4 main(FragmentInput input) : SV_Target0
{
    const float3 hdr = sanitize_scene_linear(hdrScene.Sample(hdrSampler, input.texcoord).rgb);
    if (debugBypass > 0.5f)
        return float4(saturate(hdr), 1.0f);
    const float3 bloom = sanitize_scene_linear(bloomScene.Sample(bloomSampler, input.texcoord).rgb);
    const float automaticExposure = clamp(finite_or_zero(
        exposureHistory.SampleLevel(exposureSampler, float2(0.5f, 0.5f), 0.0f)), 0.0f, 16.0f);
    const float safeBloomStrength = clamp(finite_or_zero(bloomStrength), 0.0f, 16.0f);
    const float safeExposureCompensation = clamp(finite_or_zero(exposureCompensation), 0.0f, 16.0f);
    const float safeWhitePoint = max(finite_or_zero(whitePoint), 0.0001f);
    const float3 sceneLinear = sanitize_scene_linear(
        (hdr + bloom * safeBloomStrength) * automaticExposure * safeExposureCompensation);
    const float3 graded = color_grade(sceneLinear);
    const float3 mapped = aces_fitted(graded / safeWhitePoint);
    return float4(
        linear_to_srgb(mapped.r),
        linear_to_srgb(mapped.g),
        linear_to_srgb(mapped.b),
        1.0f);
}
