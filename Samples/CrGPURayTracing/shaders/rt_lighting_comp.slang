// The lighting mode: D3D12RaytracingSimpleLighting's Raytracing.hlsl as
// one compute shader with an inline ray query. The original's ray
// generation shader is the body; its closest-hit shader (the cube's
// interpolated normal, diffuse from the point light plus ambient) and its
// miss shader (the background) are the two ends of the committed status.

import rt_common;

// RtLightingScene in ../main.c: written once.
struct Scene {
    float4 light_ambient;
    float4 light_diffuse;
    float4 albedo;
    // x: the vertices' byte offset in the geometry buffer, y: the indices'.
    uint4 offsets;
};

// RtLightingFrame in ../main.c: pushed every frame.
struct Frame {
    float4 projection_to_world[4]; // rows
    float4 camera_position;
    float4 light_position;
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure rt_scene : register(t0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> output : register(u1, space0);
[[vk::binding(2, 0)]] ConstantBuffer<Scene> scene : register(b2, space0);
// The cube's vertices (position, normal: 24 bytes each) and 16-bit
// indices: a byte-address buffer, which is what a storage buffer's raw UAV
// on D3D12 is. The same bytes the bottom level was built from.
[[vk::binding(3, 0)]] RWByteAddressBuffer geometry : register(u3, space0);
[[vk::push_constant]] ConstantBuffer<Frame> frame : register(b0, space4);

float3 vertex_normal(uint index) {
    return asfloat(geometry.Load3(scene.offsets.x + index * 24u + 12u));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rt_lighting_comp(uint3 id : SV_DispatchThreadID) {
    uint width;
    uint height;
    output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;

    const Ray camera = camera_ray(id.xy, uint2(width, height), frame.camera_position.xyz, frame.projection_to_world[0],
                                  frame.projection_to_world[1], frame.projection_to_world[2], frame.projection_to_world[3]);
    RayDesc ray;
    ray.Origin = camera.origin;
    ray.Direction = camera.direction;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(rt_scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, ray);
    // The cube is opaque: the loop ends with the closest hit committed.
    while (query.Proceed()) {
    }

    float4 color = float4(0.0, 0.2, 0.4, 1.0); // the miss shader's background
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        // The closest-hit shader's.
        const float3 hit_position = camera.origin + query.CommittedRayT() * camera.direction;
        const uint3 indices = load_3x16_indices(geometry, scene.offsets.y + query.CommittedPrimitiveIndex() * 6u);
        const float3 n0 = vertex_normal(indices.x);
        const float3 n1 = vertex_normal(indices.y);
        const float3 n2 = vertex_normal(indices.z);
        const float2 b = query.CommittedTriangleBarycentrics();
        // As the original: interpolated, every vertex of a face having the
        // face's normal.
        const float3 normal = n0 + b.x * (n1 - n0) + b.y * (n2 - n0);
        const float3 to_light = normalize(frame.light_position.xyz - hit_position);
        const float n_dot_l = max(0.0, dot(to_light, normal));
        color = scene.light_ambient + scene.albedo * scene.light_diffuse * n_dot_l;
    }
    output[id.xy] = color;
}
