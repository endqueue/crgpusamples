module rt_common;

// What the lighting and procedural modes' trace shaders share: the camera
// ray through a pixel, unprojected through the inverse of the originals'
// view-projection, and three 16-bit indices read from a byte-address
// buffer. Both from the originals' RaytracingShaderHelper.hlsli /
// Raytracing.hlsl (GenerateCameraRay, Load3x16BitIndices).
//
// A matrix comes as its four rows, each a float4, and multiplies a row
// vector on its left, as DirectXMath's do (mul(v, M) in the originals):
// the C side writes the rows it computed, and nothing depends on how a
// target lays a matrix out.

public struct Ray {
    public float3 origin;
    public float3 direction;
};

// v * M for the matrix of rows r0 to r3.
public float4 row_mul(float4 v, float4 r0, float4 r1, float4 r2, float4 r3) {
    return v.x * r0 + v.y * r1 + v.z * r2 + v.w * r3;
}

// The ray from [camera] through the middle of pixel [index] of a [size]
// target, row 0 at the top, unprojected through the projection-to-world
// matrix of rows r0 to r3 (the inverse of a left-handed view-projection).
public Ray camera_ray(uint2 index, uint2 size, float3 camera, float4 r0, float4 r1, float4 r2, float4 r3) {
    float2 xy = float2(index) + 0.5;
    float2 screen = xy / float2(size) * 2.0 - 1.0;
    // Up the screen is +Y, as Direct3D's clip space is, and Carrottus GPU's.
    screen.y = -screen.y;
    const float4 world = row_mul(float4(screen, 0.0, 1.0), r0, r1, r2, r3);
    // Divided into a vector of its own: one written a component at a time
    // starts from an undefined one, which Metal's compiler refuses.
    const float3 on_far = world.xyz / world.w;
    Ray ray;
    ray.origin = camera;
    ray.direction = normalize(on_far - camera);
    return ray;
}

// The three 16-bit indices from byte [offset] of [indices], which is a
// multiple of 2: two whole words read at the word holding the first, as
// byte-address loads are of whole aligned words.
public uint3 load_3x16_indices(RWByteAddressBuffer indices, uint offset) {
    const uint aligned = offset & ~3u;
    const uint2 words = indices.Load2(aligned);
    // Each made whole: a vector filled a component at a time starts from an
    // undefined one, which Metal's compiler refuses to read.
    if (aligned == offset)
        return uint3(words.x & 0xffffu, (words.x >> 16) & 0xffffu, words.y & 0xffffu);
    return uint3((words.x >> 16) & 0xffffu, words.y & 0xffffu, (words.y >> 16) & 0xffffu);
}
