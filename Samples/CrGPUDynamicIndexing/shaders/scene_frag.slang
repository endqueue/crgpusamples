// An unsized (runtime) array of textures below. The indices into it need no
// NonUniformResourceIndex(): both are per-draw push-constant values, uniform
// across every invocation of one draw, which already satisfies Vulkan's
// "dynamically uniform" requirement on its own -- see ../../../README.md.

struct FragmentInput {
	[[vk::location(0)]] float2 frag_uv : TEXCOORD0;
};

// Matches crgpu_device_get_bindless_descriptor_set_layout()'s binding 0 (sampled
// images) -- Texture2D, not Sampler2D, since the sampler is bound separately
// (set 1) per Carrottus GPU's bindless design (see ../../../README.md).
[[vk::binding(0, 0)]] Texture2D<float4> bindless_images[] : register(t0, space0);

[[vk::binding(0, 1)]] SamplerState samp : register(s0, space1);

struct PushConstants {
	uint diffuse_index;
	uint material_index;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0, space4);

[shader("fragment")]
float4 scene_frag(FragmentInput fin) : SV_Target0 {
	float3 diffuse = bindless_images[pc.diffuse_index].Sample(samp, fin.frag_uv).rgb;
	float3 mat     = bindless_images[pc.material_index].Sample(samp, fin.frag_uv).rgb;
	return float4(diffuse * mat, 1.0);
}
