struct FragmentInput {
    [[vk::location(0)]] float2 frag_uv : TEXCOORD0;
};

// Separate texture + sampler (not a combined Sampler2D) -- the split
// SAMPLED_IMAGE + SAMPLER pair this port binds, the idiom
// CrGPUDynamicIndexing's bindless design established (see
// ../../../README.md).
[[vk::binding(0, 0)]] Texture2D<float4> tex : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState samp : register(s1, space0);

[shader("fragment")]
float4 texture_frag(FragmentInput fin) : SV_Target0 {
    return tex.Sample(samp, fin.frag_uv);
}
