struct FragmentInput {
	[[vk::location(0)]] float3 in_position_vs : TEXCOORD0;
	[[vk::location(1)]] float3 in_normal : TEXCOORD1;
	[[vk::location(2)]] nointerpolation uint in_meshlet_index : TEXCOORD2;
};

struct Globals {
	float4x4 world;
	float4x4 world_view;
	float4x4 world_view_proj;
	uint draw_meshlets;
};

[[vk::binding(0, 1)]] ConstantBuffer<Globals> globals : register(b0, space1);

[shader("fragment")]
float4 scene_frag(FragmentInput fin) : SV_Target0 {
	float ambient_intensity = 0.1;
	float3 light_dir = -normalize(float3(1.0, -1.0, 1.0));

	float3 diffuse_color;
	float shininess;
	if (globals.draw_meshlets != 0u) {
		uint mi = fin.in_meshlet_index;
		diffuse_color = float3(
		    float(mi & 1u),
		    float(mi & 3u) / 4.0,
		    float(mi & 7u) / 8.0);
		shininess = 16.0;
	} else {
		diffuse_color = float3(0.8);
		shininess = 64.0;
	}

	float3 normal = normalize(fin.in_normal);
	float cos_angle = clamp(dot(normal, light_dir), 0.0, 1.0);
	float3 view_dir = -normalize(fin.in_position_vs);
	float3 half_angle = normalize(light_dir + view_dir);

	float blinn_term = clamp(dot(normal, half_angle), 0.0, 1.0);
	blinn_term = cos_angle != 0.0 ? blinn_term : 0.0;
	blinn_term = pow(blinn_term, shininess);

	float3 final_color = (cos_angle + blinn_term + ambient_intensity) * diffuse_color;
	return float4(final_color, 1.0);
}
