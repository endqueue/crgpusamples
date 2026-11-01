// D3D12SmallResources' VSMain: the quad's position as given, its UV passed on.

struct VertexInput {
    [[vk::location(0)]] float3 in_position : TEXCOORD0;
    [[vk::location(1)]] float2 in_uv : TEXCOORD1;
};

struct VertexOutput {
    [[vk::location(0)]] float2 frag_uv : TEXCOORD0;
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput heaps_vert(VertexInput vin) {
    VertexOutput vout;
    vout.position = float4(vin.in_position, 1.0);
    vout.frag_uv = vin.in_uv;
    return vout;
}
