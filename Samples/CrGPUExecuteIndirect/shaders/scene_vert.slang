struct VertexInput {
	[[vk::location(0)]] float3 in_position : TEXCOORD0;

	// Per-instance (scene_buffer bound at vertex binding slot 1, per_instance =
	// true, see ../main.c) -- fetched by the fixed-function input assembler
	// using first_instance/StartInstanceLocation, not SV_InstanceID. D3D12's
	// SV_InstanceID doesn't include StartInstanceLocation the way Vulkan's
	// InstanceIndex does, so indexing a storage buffer with the instance ID
	// isn't portable; the IA's own per-instance vertex fetch is.
	[[vk::location(1)]] float4 in_offset : TEXCOORD1;
	[[vk::location(2)]] float4 in_color : TEXCOORD2;
};

struct VertexOutput {
	[[vk::location(0)]] float4 frag_color : TEXCOORD0;
	float4 position : SV_Position;
};

struct ProjectionBuffer {
	float4x4 projection;
};

[[vk::binding(0, 0)]] ConstantBuffer<ProjectionBuffer> proj : register(b0, space0);

[shader("vertex")]
VertexOutput scene_vert(VertexInput vin) {
	VertexOutput vout;
	vout.position = mul(proj.projection, float4(vin.in_position, 1.0) + vin.in_offset);

	float intensity = clamp((4.0 - vout.position.z) / 2.0, 0.0, 1.0);
	vout.frag_color = float4(vin.in_color.rgb * intensity, 1.0);
	return vout;
}
