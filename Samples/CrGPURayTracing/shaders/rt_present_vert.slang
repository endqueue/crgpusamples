// The present pass's vertex shader: one triangle that covers the whole
// back buffer, from the vertex index alone (3 vertices, no vertex buffer).
// Its corners are (-1, 1), (3, 1) and (-1, -3) in clip space, +Y up on
// every backend.

[shader("vertex")]
float4 rt_present_vert(uint vertex_index : SV_VertexID) : SV_Position {
    const float2 corner = float2(float((vertex_index << 1) & 2u), float(vertex_index & 2u));
    const float2 ndc = corner * 2.0 - 1.0;
    return float4(ndc.x, -ndc.y, 0.0, 1.0);
}
