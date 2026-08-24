struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.texcoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.texcoord.x * 2.0f - 1.0f, 1.0f - output.texcoord.y * 2.0f, 0.0f, 1.0f);
    return output;
}
