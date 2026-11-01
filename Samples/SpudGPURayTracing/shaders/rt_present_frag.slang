// The present pass's fragment shader: the trace's output, texel for pixel
// (both are the window's size), row 0 at the top. The originals copy their
// output texture to the back buffer instead (CopyResource); here the back
// buffer is drawn into, since a back buffer's format and usages are the
// swap chain's to choose.

[[vk::binding(0, 0)]] Texture2D<float4> trace_output : register(t0, space0);

[shader("fragment")]
float4 rt_present_frag(float4 position : SV_Position) : SV_Target0 {
    return trace_output.Load(int3(int2(position.xy), 0));
}
