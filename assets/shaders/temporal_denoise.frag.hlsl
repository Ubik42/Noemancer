// Noemancer shared temporal denoising resolve.
//
// ABI (SDL_GPU full-screen fragment pass):
//   t0/s0, space2 = current HDR/RGBA color and clamp/linear sampler.
//   t1/s1, space2 = current-to-previous screen motion (UV delta).
//   t2/s2, space2 = current device depth.
//   t3/s3, space2 = current world-space normal (xyz; w is ignored).
//   t4/s4, space2 = previous resolved HDR/RGBA color.
//   t5/s5, space2 = previous device depth.
//   t6/s6, space2 = previous world-space normal.
//   t7/s7, space2 = current reactive mask (0 = stable, 1 = reject history).
//   b0, space3 = TemporalDenoiseSettings (64 bytes, four complete float4
//                rows; see the declaration below).
//
// MRT outputs:
//   SV_Target0 = resolved display color (debug mode may replace RGB only).
//   SV_Target1 = historyColor, always the non-debug resolved color.
//   SV_Target2 = historyDepth, current safe device depth.
//   SV_Target3 = historyNormal, current safe-normalized normal (w is 1 when
//                the normal was valid and 0 for an invalid/background sample).
// The pass is deliberately a fragment shader so that it can share the
// existing SDL_GPU full-screen triangle path while all history decisions
// remain explicit and deterministic.  It does not assume a mip chain or
// implicit derivatives: every texture read is LOD 0.

struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

Texture2D<float4> currentColor : register(t0, space2);
SamplerState currentColorSampler : register(s0, space2);
Texture2D<float2> motionVectors : register(t1, space2);
SamplerState motionSampler : register(s1, space2);
Texture2D<float> currentDepth : register(t2, space2);
SamplerState currentDepthSampler : register(s2, space2);
Texture2D<float4> currentNormal : register(t3, space2);
SamplerState currentNormalSampler : register(s3, space2);
Texture2D<float4> previousColor : register(t4, space2);
SamplerState previousColorSampler : register(s4, space2);
Texture2D<float> previousDepth : register(t5, space2);
SamplerState previousDepthSampler : register(s5, space2);
Texture2D<float4> previousNormal : register(t6, space2);
SamplerState previousNormalSampler : register(s6, space2);
Texture2D<float> reactiveMask : register(t7, space2);
SamplerState reactiveSampler : register(s7, space2);

cbuffer TemporalDenoiseSettings : register(b0, space3)
{
    // xy: inverse output resolution; z: base history weight; w: history
    // validity (0 on a reset frame, 1 after a valid previous surface exists).
    float4 resolutionAndHistory;
    // x/y: near/far clip distances; z: non-zero for reversed-Z device depth;
    // w: reserved for an infinite-far policy (currently treated as finite).
    float4 projectionParameters;
    // x: relative depth rejection tolerance; y: minimum normal dot product;
    // z: motion rejection cutoff in pixels; w: reactive mask multiplier.
    float4 rejectionParameters;
    // x: neighborhood clamp strength; y: debug mode; z/w reserved.
    float4 outputParameters;
};

static const float MIN_VALUE = 0.0001f;
static const float NORMAL_EPSILON_SQUARED = 0.000001f;

float finite_or(float value, float fallback)
{
    return isfinite(value) ? value : fallback;
}

float2 finite_or(float2 value, float2 fallback)
{
    return float2(finite_or(value.x, fallback.x), finite_or(value.y, fallback.y));
}

float3 finite_or(float3 value, float3 fallback)
{
    return float3(finite_or(value.x, fallback.x), finite_or(value.y, fallback.y),
        finite_or(value.z, fallback.z));
}

float safe_near_clip()
{
    return max(finite_or(projectionParameters.x, MIN_VALUE), MIN_VALUE);
}

float safe_far_clip(float nearClip)
{
    return max(finite_or(projectionParameters.y, nearClip + 1.0f), nearClip + MIN_VALUE);
}

float far_device_depth()
{
    return projectionParameters.z > 0.5f ? 0.0f : 1.0f;
}

float safe_device_depth(float value)
{
    return saturate(finite_or(value, far_device_depth()));
}

float linear_view_depth(float deviceDepth)
{
    const float nearClip = safe_near_clip();
    const float farClip = safe_far_clip(nearClip);
    float z = safe_device_depth(deviceDepth);
    if (projectionParameters.z > 0.5f)
        z = 1.0f - z;
    const float denominator = max(farClip - z * (farClip - nearClip), MIN_VALUE);
    return clamp(finite_or(nearClip * farClip / denominator, farClip), nearClip, farClip);
}

float4 safe_color(float4 value, float4 fallback)
{
    return float4(finite_or(value.rgb, fallback.rgb), finite_or(value.a, fallback.a));
}

float3 load_normal(Texture2D<float4> normalTexture, SamplerState normalTextureSampler,
                   float2 uv, out bool valid)
{
    const float3 value = finite_or(normalTexture.SampleLevel(normalTextureSampler, uv, 0.0f).xyz,
        0.0f.xxx);
    const float lengthSquared = dot(value, value);
    valid = isfinite(lengthSquared) && lengthSquared > NORMAL_EPSILON_SQUARED;
    return valid ? value * rsqrt(lengthSquared) : 0.0f.xxx;
}

float3 safe_history_clamp(float3 value, float3 minimumValue, float3 maximumValue)
{
    const float3 repairedMinimum = min(minimumValue, maximumValue);
    const float3 repairedMaximum = max(minimumValue, maximumValue);
    return clamp(finite_or(value, repairedMinimum), repairedMinimum, repairedMaximum);
}

float3 neighborhood_minimum(float2 uv, float3 currentValue)
{
    float3 result = currentValue;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 sampleUv = saturate(uv + float2(x, y) *
                resolutionAndHistory.xy);
            result = min(result, safe_color(currentColor.SampleLevel(
                currentColorSampler, sampleUv, 0.0f), float4(currentValue, 1.0f)).rgb);
        }
    }
    return finite_or(result, currentValue);
}

float3 neighborhood_maximum(float2 uv, float3 currentValue)
{
    float3 result = currentValue;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 sampleUv = saturate(uv + float2(x, y) *
                resolutionAndHistory.xy);
            result = max(result, safe_color(currentColor.SampleLevel(
                currentColorSampler, sampleUv, 0.0f), float4(currentValue, 1.0f)).rgb);
        }
    }
    return finite_or(result, currentValue);
}

float debug_mode_is(float mode, float expected)
{
    return abs(mode - expected) < 0.5f ? 1.0f : 0.0f;
}

struct FragmentOutput
{
    float4 resolved : SV_Target0;
    float4 historyColor : SV_Target1;
    float historyDepth : SV_Target2;
    float4 historyNormal : SV_Target3;
};

FragmentOutput main(FragmentInput input)
{
    const float4 currentSample = safe_color(currentColor.SampleLevel(
        currentColorSampler, input.texcoord, 0.0f), float4(0.0f, 0.0f, 0.0f, 1.0f));
    const float2 motion = finite_or(motionVectors.SampleLevel(
        motionSampler, input.texcoord, 0.0f), 0.0f.xx);
    const float2 historyUv = input.texcoord - motion;
    const bool historyInside = all(historyUv >= 0.0f.xx) && all(historyUv <= 1.0f.xx);

    // Clamp the lookup even when it is rejected.  This keeps the shader free
    // of undefined texture behavior at the screen edge; historyInside still
    // guarantees a zero history weight for an out-of-bounds reprojection.
    const float2 safeHistoryUv = saturate(historyUv);
    const float4 previousSample = safe_color(previousColor.SampleLevel(
        previousColorSampler, safeHistoryUv, 0.0f), currentSample);

    const float currentDeviceDepth = safe_device_depth(currentDepth.SampleLevel(
        currentDepthSampler, input.texcoord, 0.0f));
    const float previousDeviceDepth = safe_device_depth(previousDepth.SampleLevel(
        previousDepthSampler, safeHistoryUv, 0.0f));
    const float currentLinearDepth = linear_view_depth(currentDeviceDepth);
    const float previousLinearDepth = linear_view_depth(previousDeviceDepth);
    const float depthTolerance = max(MIN_VALUE, currentLinearDepth *
        max(finite_or(rejectionParameters.x, 0.02f), 0.0f));
    const bool depthCompatible = abs(currentLinearDepth - previousLinearDepth) <= depthTolerance;

    bool currentNormalValid;
    bool previousNormalValid;
    const float3 currentNormalValue = load_normal(currentNormal, currentNormalSampler,
        input.texcoord, currentNormalValid);
    const float3 previousNormalValue = load_normal(previousNormal, previousNormalSampler,
        safeHistoryUv, previousNormalValid);
    const float normalThreshold = clamp(finite_or(rejectionParameters.y, 0.85f), -1.0f, 1.0f);
    const bool normalCompatible = (!currentNormalValid && !previousNormalValid) ||
        (currentNormalValid && previousNormalValid &&
            dot(currentNormalValue, previousNormalValue) >= normalThreshold);

    const float2 motionPixels = motion / max(resolutionAndHistory.xy, MIN_VALUE.xx);
    const float motionMagnitude = length(finite_or(motionPixels, 0.0f.xx));
    const float motionCutoff = max(finite_or(rejectionParameters.z, 64.0f), 1.0f);
    const float motionFactor = saturate(1.0f - motionMagnitude / motionCutoff);
    const float reactive = saturate(finite_or(reactiveMask.SampleLevel(
        reactiveSampler, input.texcoord, 0.0f), 0.0f) *
        max(finite_or(rejectionParameters.w, 1.0f), 0.0f));

    const bool disoccluded = !historyInside || !depthCompatible || !normalCompatible ||
        motionFactor <= 0.0f;
    const float historyValid = saturate(finite_or(resolutionAndHistory.w, 0.0f));
    const float baseHistoryWeight = saturate(finite_or(resolutionAndHistory.z, 0.0f));
    const float rejection = (depthCompatible ? 1.0f : 0.0f) *
        (normalCompatible ? 1.0f : 0.0f) * motionFactor;

    const float3 neighborhoodMinimum = neighborhood_minimum(input.texcoord, currentSample.rgb);
    const float3 neighborhoodMaximum = neighborhood_maximum(input.texcoord, currentSample.rgb);
    const float3 clampedHistory = safe_history_clamp(previousSample.rgb,
        neighborhoodMinimum, neighborhoodMaximum);
    const float clampStrength = saturate(finite_or(outputParameters.x, 1.0f));
    const float3 boundedHistory = lerp(previousSample.rgb, clampedHistory, clampStrength);
    float historyWeight = baseHistoryWeight * historyValid * rejection *
        (1.0f - reactive);
    if (disoccluded)
        historyWeight = 0.0f;
    historyWeight = saturate(finite_or(historyWeight, 0.0f));

    const float4 resolved = float4(lerp(currentSample.rgb, boundedHistory, historyWeight),
        currentSample.a);
    float3 displayColor = resolved.rgb;
    const float debugMode = finite_or(outputParameters.y, 0.0f);
    if (debug_mode_is(debugMode, 1.0f))
        displayColor = saturate(float3(abs(motionPixels) / 16.0f, 0.0f));
    else if (debug_mode_is(debugMode, 2.0f))
        displayColor = reactive.xxx;
    else if (debug_mode_is(debugMode, 3.0f))
        displayColor = disoccluded ? float3(0.95f, 0.05f, 0.02f) : float3(0.05f, 0.80f, 0.12f);
    else if (debug_mode_is(debugMode, 4.0f))
        displayColor = historyWeight.xxx;
    else if (debug_mode_is(debugMode, 5.0f))
        displayColor = saturate(abs(clampedHistory - previousSample.rgb) * 4.0f);
    else if (debug_mode_is(debugMode, 6.0f))
        displayColor = saturate(currentLinearDepth / max(safe_far_clip(safe_near_clip()), MIN_VALUE)).xxx;
    else if (debug_mode_is(debugMode, 7.0f))
        displayColor = currentNormalValid ? currentNormalValue * 0.5f + 0.5f : float3(0.5f, 0.0f, 0.5f);

    FragmentOutput output;
    output.resolved = float4(finite_or(displayColor, 0.0f.xxx), finite_or(resolved.a, 1.0f));
    // History must never contain the visualization selected by debugMode.
    output.historyColor = resolved;
    output.historyDepth = currentDeviceDepth;
    output.historyNormal = float4(currentNormalValue, currentNormalValid ? 1.0f : 0.0f);
    return output;
}
