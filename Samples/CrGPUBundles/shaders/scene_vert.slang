struct VertexInput {
	[[vk::location(0)]] float3 in_position : TEXCOORD0;
	[[vk::location(1)]] float3 in_normal : TEXCOORD1;
};

struct VertexOutput {
	[[vk::location(0)]] float3 frag_color : TEXCOORD0;
	[[vk::location(1)]] float3 frag_normal : TEXCOORD1;
	float4 position : SV_Position;
};

// One CBV per building, matching the fixed per-object descriptor set bound
// inside the bundle -- only the buffer bytes behind this binding change
// frame to frame (see UpdateObjectConstants in ../main.c), not the binding
// itself, which is what lets a bundle recorded once keep working forever.
struct CBV {
	float4x4 mvp;
	float4 color;
};

[[vk::binding(0, 0)]] ConstantBuffer<CBV> cb : register(b0, space0);

[shader("vertex")]
VertexOutput scene_vert(VertexInput vin) {
	VertexOutput vout;
	vout.position = mul(cb.mvp, float4(vin.in_position, 1.0));

	// Every building's model matrix is a pure translation (see
	// model_matrices in ../main.c) -- no rotation/scale, so the object-space
	// normal already equals the world-space one, with no separate normal
	// matrix needed.
	vout.frag_normal = vin.in_normal;
	vout.frag_color  = cb.color.rgb;
	return vout;
}
