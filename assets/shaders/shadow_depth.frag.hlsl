struct FragmentInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 occlusionAlphaFlags : TEXCOORD2;
};

Texture2D<float4> baseColorTexture : register(t0, space2);
SamplerState materialSampler : register(s0, space2);

void main(FragmentInput input)
{
    if (input.occlusionAlphaFlags.z > 0.5f && input.occlusionAlphaFlags.z < 1.5f)
        clip(input.color.a * baseColorTexture.Sample(materialSampler, input.texcoord).a - input.occlusionAlphaFlags.y);
}
