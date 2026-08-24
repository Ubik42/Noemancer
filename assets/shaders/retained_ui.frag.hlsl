struct FragmentInput { float4 position : SV_Position; float4 color : TEXCOORD0; float2 texcoord : TEXCOORD1; };
Texture2D ui_texture : register(t0, space2);
SamplerState ui_sampler : register(s0, space2);
float4 main(FragmentInput input) : SV_Target0 { return ui_texture.Sample(ui_sampler, input.texcoord) * input.color; }
