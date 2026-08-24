struct VertexInput { float2 position : TEXCOORD0; float4 color : TEXCOORD1; float2 texcoord : TEXCOORD2; };
struct VertexOutput { float4 position : SV_Position; float4 color : TEXCOORD0; float2 texcoord : TEXCOORD1; };
cbuffer UiViewport : register(b0, space1) { float2 viewportSize; float2 padding; };
VertexOutput main(VertexInput input) {
    VertexOutput output;
    output.position=float4(input.position.x*2.0f/viewportSize.x-1.0f,1.0f-input.position.y*2.0f/viewportSize.y,0.0f,1.0f);
    output.color=input.color; output.texcoord=input.texcoord; return output;
}
