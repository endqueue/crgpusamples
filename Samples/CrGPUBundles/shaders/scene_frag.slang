struct FragmentInput {
	[[vk::location(0)]] float3 frag_color : TEXCOORD0;
	[[vk::location(1)]] float3 frag_normal : TEXCOORD1;
};

// Basic directional (Lambertian) lighting -- a fixed light direction and no
// per-object light data, so no new uniform is needed beyond the existing
// per-building CBV.
static const float3 LIGHT_DIR = normalize(float3(0.4, -1.0, 0.3)); // direction the light travels
static const float AMBIENT    = 0.95;

[shader("fragment")]
float4 scene_frag(FragmentInput fin) : SV_Target0 {
	float3 n        = normalize(fin.frag_normal);
	float diffuse   = max(dot(n, -LIGHT_DIR), 0.0);
	float intensity = AMBIENT + (1.0 - AMBIENT) * diffuse;
	return float4(fin.frag_color * intensity, 1.0);
}
