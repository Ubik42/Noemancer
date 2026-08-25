// Noemancer sky atmosphere full-screen triangle.
//
// The vertex stage deliberately has no vertex buffer.  Keeping the triangle
// in shader space makes the atmosphere pass independent of scene geometry and
// lets the renderer use the same source on D3D12 and Vulkan.

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.texcoord = uv;
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
