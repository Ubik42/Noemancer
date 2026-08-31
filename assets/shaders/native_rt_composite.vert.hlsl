// noemancer.native-rt-composite/0.1
// Full-screen triangle for the native RT output debug composite.  The vertex
// stage has no resources; SDL_GPU supplies SV_VertexID for the three vertices.

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    const float2 position = float2((vertexId << 1) & 2, vertexId & 2);
    output.texcoord = position;
    output.position = float4(position * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}
