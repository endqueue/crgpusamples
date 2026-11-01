// D3D12SmallResources' PSMain: the quad's texture, sampled. Separate texture
// and sampler (not a combined Sampler2D), for the reason ../../HelloTexture
// gives.

struct FragmentInput {
    [[vk::location(0)]] float2 frag_uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> tex : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState samp : register(s1, space0);

[shader("fragment")]
float4 heaps_frag(FragmentInput fin) : SV_Target0 {
    return tex.Sample(samp, fin.frag_uv);
}
