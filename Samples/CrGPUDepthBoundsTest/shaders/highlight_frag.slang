// Deliberately ignores the vertex color varying triangle_vert.slang still
// emits -- see ../../README.md's CrGPUDepthBoundsTest section for why the
// depth-bounds-tested pass uses a fixed highlight color instead of the
// D3D12 original's colorless depth-only priming pass.

[shader("fragment")]
float4 highlight_frag() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
