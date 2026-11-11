struct VertexInput {
    [[vk::location(0)]] float3 in_position : TEXCOORD0;
    [[vk::location(1)]] float4 in_color : TEXCOORD1;
};

struct VertexOutput {
    [[vk::location(0)]] float4 frag_color : TEXCOORD0;
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput triangle_vert(VertexInput vin) {
    VertexOutput vout;
    vout.position = float4(vin.in_position, 1.0);
    vout.frag_color = vin.in_color;
    return vout;
}
