struct VertexInput {
    [[vk::location(0)]] float2 in_position : TEXCOORD0;
    [[vk::location(1)]] float3 in_color : TEXCOORD1;
};

struct VertexOutput {
    [[vk::location(0)]] float3 frag_color : TEXCOORD0;
    float4 position : SV_Position;
};

struct SceneConstantBuffer {
    float4 offset;
};

[[vk::binding(0, 0)]] ConstantBuffer<SceneConstantBuffer> scene : register(b0, space0);

[shader("vertex")]
VertexOutput scene_vert(VertexInput vin) {
    VertexOutput vout;
    vout.position = float4(vin.in_position, 0.0, 1.0) + scene.offset;
    vout.frag_color = vin.in_color;
    return vout;
}
