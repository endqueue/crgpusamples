// The hello mode: D3D12RaytracingHelloWorld's Raytracing.hlsl as one
// compute shader with an inline ray query. The original's ray generation
// shader is the body; its closest-hit (the hit's barycentrics as a colour)
// and miss (black) shaders are the two ends of the committed status.
//
// Orthographic rays along +z in screen space: x from the viewport's left to
// right across the target, y from its top to bottom down it (y down, as
// the original's screen space is). Inside the stencil window a ray is
// traced; outside it, the pixel's place in the target is its colour.

struct Viewport {
    float left;
    float top;
    float right;
    float bottom;
};

// RtHelloFrame in ../main.c.
struct Frame {
    Viewport viewport;
    Viewport stencil;
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene : register(t0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> output : register(u1, space0);
[[vk::push_constant]] ConstantBuffer<Frame> frame : register(b0, space4);

bool inside(float2 p, Viewport v) {
    return (p.x >= v.left && p.x <= v.right) && (p.y >= v.top && p.y <= v.bottom);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rt_hello_comp(uint3 id : SV_DispatchThreadID) {
    uint width;
    uint height;
    output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;

    const float2 lerp_values = float2(id.xy) / float2(width, height);
    const float3 origin = float3(lerp(frame.viewport.left, frame.viewport.right, lerp_values.x),
                                 lerp(frame.viewport.top, frame.viewport.bottom, lerp_values.y), 0.0);

    if (!inside(origin.xy, frame.stencil)) {
        output[id.xy] = float4(lerp_values, 0.0, 1.0);
        return;
    }

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = float3(0.0, 0.0, 1.0);
    // The original's: a small non-zero t min, against floating-point
    // self-intersection.
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, ray);
    // The triangle is opaque: no candidate is the shader's to decide, and
    // the loop ends with the closest hit committed.
    while (query.Proceed()) {
    }

    float4 color = float4(0.0, 0.0, 0.0, 1.0); // the miss shader's
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        // The closest-hit shader's.
        const float2 b = query.CommittedTriangleBarycentrics();
        color = float4(1.0 - b.x - b.y, b.x, b.y, 1.0);
    }
    output[id.xy] = color;
}
