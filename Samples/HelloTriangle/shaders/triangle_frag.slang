struct FragmentInput {
    [[vk::location(0)]] float3 frag_color : TEXCOORD0;
};

[shader("fragment")]
float4 triangle_frag(FragmentInput fin) : SV_Target0 {
    return float4(fin.frag_color, 1.0);
}
